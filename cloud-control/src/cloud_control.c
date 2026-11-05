/*
 * cloud_control.c - 巴法云（bemfa.com）云端开关机守护进程
 *
 * 功能概述：
 *   1. 通过 TCP 长连接订阅巴法云主题；
 *   2. 收到 "on"  主题消息 -> 发送 Wake-on-LAN 魔术包唤醒目标主机；
 *   3. 收到 "off" 主题消息 -> 通过 SSH 执行关机命令，成功后才向云端回报 off。
 *
 * 安全设计要点（相对上游版本的重写）：
 *   - 全进程不使用 system()/popen()，所有外部命令走 fork+execvp（run_argv），
 *     彻底消除命令字符串拼接导致的命令注入；
 *   - SSH 密码通过环境变量 SSHPASS 传递（sshpass -e），不会出现在 ps 输出中；
 *   - 先后台执行、后上报云端，SSH 失败时回滚云端状态，保证小爱/云端状态与真实一致；
 *   - 日志按行缓冲解析、大小上限轮转、client_id 脱敏；
 *   - procd 托管，SIGTERM 优雅退出。
 *
 * 依赖：无库依赖（不链接 libuci / libubox，配置自行解析）。
 *       运行期外部命令：ssh + sshpass（关机）、curl（状态上报，可选）。
 *       Wake-on-LAN 由本程序直接发送 UDP 魔术包，不需要 etherwake。
 *
 * License: MIT
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pwd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/*
 * 本程序刻意不链接 libuci：
 *   配置只在启动/重载时读一次，格式就是 "config/option" 的纯文本，
 *   自己解析比引入一个运行时库更省依赖、更省 flash。
 *   好处是整份程序可以静态链接成零依赖二进制，在任意 OpenWrt 上直接跑，
 *   不会因为固件的 musl/glibc、libuci ABI 版本不同而起不来。
 */

/* ------------------------------------------------------------------ */
/* 常量                                                               */
/* ------------------------------------------------------------------ */

#define BEMFA_HOST          "bemfa.com"
#define BEMFA_PORT          8344
#define BEMFA_PUSH_URL      "https://api.bemfa.com/api/device/v1/data/3/push/get/" \
                            "?uid=%s&topic=%s&msg=%s"

#ifndef LOG_PATH
#define LOG_PATH            "/var/log/cloud_control.log"
#endif
#define LOG_PATH_ROTATED    "/var/log/cloud_control.log.1"
#define LOG_DEFAULT_KB      256

#define RECV_BUF_SIZE       4096        /* 接收缓冲（按行缓冲解析） */
#define LINE_MAX_LEN        512         /* 单行报文最大长度 */
#define HEARTBEAT_SEC       20          /* 心跳间隔（select 粒度 1s，留足余量） */
#define RECHECK_SEC         60          /* 配置复查 / 重新订阅间隔 */
#define CONN_TIMEOUT_SEC    8           /* 建连超时 */
#define RETRY_DELAY_SEC     10          /* 连接失败后的重试间隔 */

/* 关机后等待目标机离线的超时；可用 -D 覆盖，便于测试 */
#ifndef VERIFY_TIMEOUT_SEC
#define VERIFY_TIMEOUT_SEC  60
#endif
#define VERIFY_FAILS_NEEDED 3           /* 连续几次连不上才算离线 */
#define SSH_TIMEOUT_SEC     15          /* 单次 SSH 执行上限，防止卡死拖垮页面 */

/* 防误动作 / 防死循环 */
#define ECHO_GUARD_SEC      10          /* 自身回显去重窗口（秒） */
#define ON_MIN_INTERVAL_SEC  10         /* on  指令最小执行间隔（秒） */
#define OFF_MIN_INTERVAL_SEC 30         /* off 指令最小执行间隔（秒） */

#ifndef CC_VERSION
#define CC_VERSION          "dev"
#endif

#define CFG_PACKAGE         "cloud_control"
#define CFG_SECTION_TYPE    "main"

/* ------------------------------------------------------------------ */
/* 配置结构体                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char enabled[8];        /* "0" / "1" 总开关 */
    char client_id[128];    /* 巴法云私钥 UID */
    char topic[128];        /* 订阅主题 */
    char host[128];         /* 目标机 IP（兼容旧字段 ip） */
    char ssh_port[16];      /* SSH 端口 */
    char user[96];          /* SSH 用户名 */
    char password[192];     /* SSH 密码 */
    char nic[64];           /* WoL 出口网卡 */
    char mac[64];           /* 目标机 MAC */
    char host_os[16];       /* windows / linux */
    char shutdown_cmd[256]; /* 自定义关机命令，非空时优先 */
    char log_max_kb[16];    /* 日志上限 KB */
    char verify_offline[8]; /* 关机后是否探测确认离线：1 是 / 0 否 */
} cc_config_t;

/* ------------------------------------------------------------------ */
/* 全局状态                                                           */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_running = 1;
/* 上一次 SSH 是否因为连接被中断而无法确认结果（例如 /t 0 导致 sshd 先被关掉） */
static int g_ssh_inconclusive = 0;  /* 主循环开关，SIGTERM 置 0 */
static FILE *g_logfp = NULL;                 /* 常开日志句柄，避免反复 fopen */
static long g_log_max_bytes = LOG_DEFAULT_KB * 1024L;
static int g_log_to_stdout = 0;              /* 测试模式：日志打到 stdout */
static int g_log_to_file = 1;                /* 测试模式：不写文件 */
static int g_verbose = 0;                    /* 测试模式：子进程保留 stdio */

/*
 * 防「自回显打成正反馈」：
 * bemfa_push() 是往同一个 topic 发消息，本连接很可能把自己刚推的那条收回来。
 * 若不去重就会出现「关了自动开机」「电脑被反复唤醒」的死循环。
 */
static char g_last_push_msg[8] = "";         /* 最近一次成功上报的 msg */
static time_t g_last_push_time = 0;          /* 该次上报的时刻 */
static time_t g_last_kind_time[3] = { 0, 0, 0 };  /* [1]=on 上次执行, [2]=off 上次执行 */

/* ------------------------------------------------------------------ */
/* 基础工具                                                           */
/* ------------------------------------------------------------------ */

/* 带长度上限的字符串拷贝，保证目标串以 '\0' 结尾。 */
static void cc_strlcpy(char *dst, const char *src, size_t dstsz)
{
    if (dst == NULL || dstsz == 0)
        return;

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    size_t n = strlen(src);
    if (n >= dstsz)
        n = dstsz - 1;

    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* 去掉字符串首尾空白，原地修改。 */
static char *cc_trim(char *s)
{
    if (s == NULL)
        return NULL;

    while (*s != '\0' && isspace((unsigned char)*s))
        s++;

    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';

    return s;
}

/* 脱敏 client_id：仅保留前 4 个字符（巴法云私钥等同账号密码）。 */
static void mask_id(const char *id, char *out, size_t outsz)
{
    if (out == NULL || outsz == 0)
        return;

    if (id == NULL || id[0] == '\0') {
        cc_strlcpy(out, "(未配置)", outsz);
        return;
    }

    if (strlen(id) <= 4)
        snprintf(out, outsz, "****");
    else
        snprintf(out, outsz, "%.4s****", id);
}

/* 判断字符是否为 uid 值之后的终止符。 */
static int is_uid_delim(char c)
{
    return (c == '\0' || c == '&' || c == ',' || c == ';' || c == '"' ||
            c == '\'' || c == '}' || c == '|' || c == '\r' || c == '\n' ||
            c == ' ' || c == '\t');
}

/*
 * redact_secrets - 把一行文本里 uid= 后面的值替换成前 4 位 + ****。
 *
 * 用途：巴法云下发的每条报文都带 uid=<32 位私钥>，私钥等同账号密码，
 * 绝不能明文落盘（/var/log 会被 LuCI 页面读出来展示），也不能原样
 * 打到管理员浏览器上。日志与测试输出一律先过这个函数。
 *
 * 注意：只用于「输出」，解析必须用原文。
 * 找不到 uid= 时原样拷贝。
 */
static void redact_secrets(const char *in, char *out, size_t outsz)
{
    const char *p;
    size_t o = 0;

    if (out == NULL || outsz == 0)
        return;

    if (in == NULL) {
        out[0] = '\0';
        return;
    }

    p = in;
    while (*p != '\0') {
        if (strncasecmp(p, "uid", 3) == 0) {
            const char *sep = p + 3;

            /* 兼容 uid=xxx 与 {"uid":"xxx"} 两种形态 */
            while (*sep == '"' || *sep == '\'' || *sep == ' ' || *sep == '\t')
                sep++;

            if (*sep == '=' || *sep == ':') {
                const char *v = sep + 1;
                size_t kept = 0;

                while (*v == '"' || *v == '\'' || *v == ' ' || *v == '\t')
                    v++;

                /* 输出 "uid=" */
                if (o + 4 >= outsz)
                    break;
                memcpy(out + o, "uid=", 4);
                o += 4;

                /* 值的前 4 个字符原样保留 */
                while (*v != '\0' && !is_uid_delim(*v) && kept < 4) {
                    if (o + 1 >= outsz)
                        goto done;
                    out[o++] = *v++;
                    kept++;
                }

                /* 跳过值的剩余部分 */
                while (*v != '\0' && !is_uid_delim(*v))
                    v++;

                /* 追加 **** 掩码 */
                if (o + 4 >= outsz)
                    goto done;
                memcpy(out + o, "****", 4);
                o += 4;

                p = v;
                continue;
            }
        }

        if (o + 1 >= outsz)
            break;
        out[o++] = *p++;
    }

done:
    out[o] = '\0';
}

/* URL 参数编码，防止 topic 中的 & = 等字符破坏请求或注入额外参数。 */
static void url_encode(const char *in, char *out, size_t outsz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    if (out == NULL || outsz == 0)
        return;

    if (in == NULL) {
        out[0] = '\0';
        return;
    }

    for (; *in != '\0'; in++) {
        unsigned char c = (unsigned char)*in;

        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (o + 1 >= outsz)
                break;
            out[o++] = (char)c;
        } else {
            if (o + 3 >= outsz)
                break;
            out[o++] = '%';
            out[o++] = hex[(c >> 4) & 0x0F];
            out[o++] = hex[c & 0x0F];
        }
    }

    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* 日志                                                               */
/* ------------------------------------------------------------------ */

/*
 * 时区：本次进程最终使用的 TZ 值与其来源，仅供 -t 测试模式打印排查信息。
 * 空串表示没有从环境变量或配置文件拿到任何时区，此时实际生效的是 libc 的回落行为
 * （读 /etc/localtime，该文件也不存在才是 UTC）—— 因此这里只说「未设置」，
 * 不承诺是 UTC，真正的偏移由 cc_tz_offset_sec() 实测后打印。
 */
static char g_tz_value[64] = "";
static const char *g_tz_source = "默认";

#define SYSTEM_CFG_PATH     "/etc/config/system"

/*
 * cc_tz_offset_sec() 取不到偏移时的哨兵值。
 * 不能用 0：0 表示「真的是 UTC+0」，两者必须能区分开，否则诊断行又会说假话。
 */
#define CC_TZ_OFFSET_UNKNOWN    INT_MIN

/*
 * cc_init_timezone - 自行解析 OpenWrt 时区并注入环境变量 TZ。
 *
 * 为什么需要它：
 *   log_write() 用 localtime_r() 生成时间戳，而 localtime_r() 依赖环境变量 TZ，
 *   TZ 未设置时由 libc 自行决定时区（OpenWrt 上通常是 UTC）。本进程由 procd 拉起，
 *   而 OpenWrt 只在 /etc/profile 里给登录 shell 设置 TZ，procd 与 LuCI 调起的进程
 *   环境里都没有 TZ，于是日志时间戳比本地时间差出一个时区偏移（中国时区早 8 小时）。
 *   在程序内部自行解析，一处修复即可覆盖 procd 托管、-t 测试、手工前台运行所有入口，
 *   也不必依赖某一条启动路径去注入环境变量。
 */
static void cc_init_timezone(void)
{
    const char *env;
    FILE *fp;
    char line[256];
    char tz[64];
    int got = 0;

    /* 外部环境已经给了非空 TZ：尊重它，绝不覆盖 */
    env = getenv("TZ");
    if (env != NULL && env[0] != '\0') {
        cc_strlcpy(g_tz_value, env, sizeof(g_tz_value));
        g_tz_source = "环境变量";
        return;
    }

    memset(tz, 0, sizeof(tz));

    fp = fopen(SYSTEM_CFG_PATH, "r");
    if (fp == NULL)
        return;             /* 文件不存在（非 OpenWrt 环境）：不动 TZ */

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = line;
        char quote = '\0';
        size_t n = 0;

        while (*p == ' ' || *p == '\t')
            p++;

        /* 空行与整行注释（含被 # 注释掉的 option timezone）一律跳过 */
        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#')
            continue;

        if (strncmp(p, "option", 6) != 0 ||
            !(p[6] == ' ' || p[6] == '\t'))
            continue;

        p += 6;
        while (*p == ' ' || *p == '\t')
            p++;

        /* key 必须是 timezone，且后面要紧跟分隔符才算是这个键 */
        if (strncmp(p, "timezone", 8) != 0 ||
            !(p[8] == ' ' || p[8] == '\t'))
            continue;

        p += 8;
        while (*p == ' ' || *p == '\t')
            p++;

        /* 值通常是单引号包裹的 POSIX TZ 串，也兼容双引号与无引号 */
        if (*p == '\'' || *p == '"') {
            quote = *p;
            p++;
        }

        while (p[n] != '\0' && p[n] != '\n' && p[n] != '\r') {
            if (quote != '\0') {
                if (p[n] == quote)
                    break;
            } else if (p[n] == ' ' || p[n] == '\t') {
                break;
            }
            n++;
        }

        if (n >= sizeof(tz))
            n = sizeof(tz) - 1;

        memcpy(tz, p, n);
        tz[n] = '\0';

        /*
         * 裁掉值首尾的空白：'CST-8 ' 这种尾随空格会让 TZ 变成 "CST-8 "，
         * 是非法的 POSIX TZ 串，libc 会静默回落到默认值 —— 修复等于失效，
         * 而且不会有任何报错，极难排查。前导空白同理。
         * cc_trim 原地去掉尾部空白并返回首个非空白字符，前导部分用 memmove 搬回。
         */
        {
            char *t = cc_trim(tz);

            if (t != tz)
                memmove(tz, t, strlen(t) + 1);
        }

        if (tz[0] != '\0') {
            got = 1;
            break;
        }
        /* 值为空：继续看后面的行，可能存在多条 timezone 配置 */
    }

    fclose(fp);

    if (!got)
        return;             /* 没有配置时区：不动 TZ，交给 libc 的回落行为 */

    setenv("TZ", tz, 1);
    tzset();
    cc_strlcpy(g_tz_value, tz, sizeof(g_tz_value));
    g_tz_source = SYSTEM_CFG_PATH;
}

/*
 * cc_tz_offset_sec - 当前生效时区相对 UTC 的偏移（秒）。
 *
 * 为什么需要它：
 *   TZ 没拿到时，实际生效的时区由 libc 决定（读 /etc/localtime，读不到才是 UTC），
 *   我们并没有控制权。与其在诊断信息里「猜」成 UTC，不如把真实偏移取出来，
 *   如实告诉使用者日志现在到底是哪个偏移。必须在 cc_init_timezone() 之后调用，
 *   否则读到的是未初始化的时区。
 *
 * 取值方式：直接读 localtime_r() 填好的 tm_gmtoff。
 *   曾经用「localtime 时分 − gmtime 时分」自己换算，那套算法假定偏移必在 ±12 小时
 *   以内，会把 +12:45（查塔姆群岛）、+13（新西兰夏令时）、+14（基里巴斯）这类
 *   时区符号翻反、并整体差 24 小时 —— 比不打印更误导。tm_gmtoff 是 glibc 与 musl
 *   都提供的扩展字段（本文件已定义 _GNU_SOURCE），直接用它没有这些假设。
 */
static int cc_tz_offset_sec(void)
{
    time_t now = time(NULL);
    struct tm lt;

    memset(&lt, 0, sizeof(lt));

    if (localtime_r(&now, &lt) == NULL)
        return CC_TZ_OFFSET_UNKNOWN;

    return (int)lt.tm_gmtoff;
}

/* 打开（或重新打开）日志文件。 */
static void log_open(void)
{
    if (!g_log_to_file)
        return;

    if (g_logfp != NULL)
        fclose(g_logfp);

    g_logfp = fopen(LOG_PATH, "a");
    if (g_logfp != NULL)
        setvbuf(g_logfp, NULL, _IOLBF, 0);
}

/* 检查日志大小，超过上限则 rename 成 .log.1（覆盖旧文件）。 */
static void log_check_rotate(void)
{
    struct stat st;

    if (!g_log_to_file)
        return;

    if (g_logfp == NULL) {
        log_open();
        return;
    }

    if (stat(LOG_PATH, &st) != 0)
        return;

    if (st.st_size < g_log_max_bytes)
        return;

    fclose(g_logfp);
    g_logfp = NULL;

    /* 覆盖式轮转，只保留一份历史，避免 tmpfs 被写满 */
    remove(LOG_PATH_ROTATED);
    if (rename(LOG_PATH, LOG_PATH_ROTATED) != 0) {
        /* 轮转失败也要保证后续日志能继续写 */
    }

    log_open();
}

/* 统一日志输出：写文件 + 可选写 stdout（测试模式）。 */
static void log_write(const char *level, const char *fmt, ...)
{
    char msg[1024];
    char ts[32];
    time_t now;
    struct tm tmv;
    va_list ap;

    if (fmt == NULL)
        return;

    now = time(NULL);
    memset(&tmv, 0, sizeof(tmv));
    if (localtime_r(&now, &tmv) == NULL)
        cc_strlcpy(ts, "--", sizeof(ts));
    else
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (g_log_to_file) {
        log_check_rotate();
        if (g_logfp != NULL) {
            fprintf(g_logfp, "[%s] [%s] %s\n", ts, level, msg);
            fflush(g_logfp);
        }
    }

    if (g_log_to_stdout) {
        fprintf(stdout, "[%s] %s\n", level, msg);
        fflush(stdout);
    }
}

/* ------------------------------------------------------------------ */
/* 外部命令执行（唯一出口，禁止字符串拼接）                            */
/* ------------------------------------------------------------------ */

/* 前置声明：子进程环境里要修正 HOME，实现放在后面 */
static const char *cc_home_dir(void);

/*
 * run_argv - fork + execvp 执行外部命令，返回其退出码。
 *
 * argv        以 NULL 结尾的参数数组，argv[0] 为程序名
 * env_name    需要额外注入的环境变量名（可为 NULL）
 * env_val     对应值（可为 NULL）
 *
 * 返回值：>=0 为子进程退出码；-1 为 fork/exec 失败。
 * 非 verbose 模式下子进程 stdout/stderr 重定向到 /dev/null，
 * 避免污染 procd 的日志管道。
 */
static int run_argv(char *const argv[], const char *env_name, const char *env_val)
{
    pid_t pid;
    int status = 0;

    if (argv == NULL || argv[0] == NULL)
        return -1;

    pid = fork();
    if (pid < 0) {
        log_write("ERROR", "fork 失败：%s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        int fd;

        /* 子进程：注入环境变量（SSHPASS 只存在于子进程环境，父进程不受污染） */
        if (env_name != NULL && env_val != NULL)
            setenv(env_name, env_val, 1);

        /*
         * procd 拉起的进程环境非常干净，HOME 往往是空的。
         * dbclient 用 $HOME 拼私钥路径（~/.ssh/id_dropbear），
         * HOME 缺失就找不到密钥 → 回落到密码认证 → 没有终端可输 →
         * 会话在认证阶段就断掉。手动在 shell 里跑没有这个问题。
         */
        setenv("HOME", cc_home_dir(), 1);

        if (!g_verbose) {
            fd = open("/dev/null", O_RDWR);
            if (fd >= 0) {
                dup2(fd, STDIN_FILENO);
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd > STDERR_FILENO)
                    close(fd);
            }
        }

        /*
         * 关掉继承来的多余 fd。
         * 关键是巴法云的长连接 socket：curl/ssh 最长会跑 8 秒，
         * 若被子进程继承，父进程 close() 也要等子进程退出才真正 FIN，
         * 重连时可能撞上巴法云「同一私钥长连接数」的限制。
         */
        for (fd = STDERR_FILENO + 1; fd < 256; fd++)
            close(fd);

        execvp(argv[0], argv);
        /* exec 失败 */
        _exit(127);
    }

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            log_write("ERROR", "waitpid 失败：%s", strerror(errno));
            return -1;
        }
        /* 被信号打断则继续等待，避免僵尸进程 */
    }

    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    return -1;
}

/*
 * run_argv_capture - 执行命令并捕获它的标准输出（含 stderr）。
 * 返回退出码；-1 表示 pipe/fork 失败。输出超过 outsz 的部分丢弃。
 */
static int run_argv_capture(char *const argv[], const char *env_name,
                           const char *env_val, int timeout_sec,
                           char *out, size_t outsz)
{
    char chunk[256];
    int pfd[2];
    int spfd[2];
    pid_t pid;
    int status = 0;
    size_t used = 0;
    ssize_t n;
    time_t deadline;
    int timed_out = 0;

    if (out == NULL || outsz == 0)
        return -1;

    out[0] = '\0';

    if (pipe(pfd) != 0)
        return -1;

    /*
     * 给子进程一个「永远不 EOF」的 stdin。
     * 守护进程由 procd 拉起，fd 0 通常是 /dev/null；dbclient 一读就得到
     * EOF，会把 EOF 立刻推给远端通道，Windows 那边命令还没执行完会话就关了，
     * 表现为 "Connection ... exited: Interrupted." 且没有任何输出。
     * 手动在终端里跑没有这个问题，因为 stdin 是终端。
     */
    if (pipe(spfd) != 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        close(spfd[0]);
        close(spfd[1]);
        return -1;
    }

    if (pid == 0) {
        int fd;

        close(pfd[0]);
        close(spfd[1]);

        dup2(spfd[0], STDIN_FILENO);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);

        close(spfd[0]);
        close(pfd[1]);

        if (env_name != NULL && env_val != NULL)
            setenv(env_name, env_val, 1);

        /*
         * procd 拉起的进程环境非常干净，HOME 往往是空的。
         * dbclient 用 $HOME 拼私钥路径（~/.ssh/id_dropbear），
         * HOME 缺失就找不到密钥 → 回落到密码认证 → 没有终端可输 →
         * 会话在认证阶段就断掉。手动在 shell 里跑没有这个问题。
         */
        setenv("HOME", cc_home_dir(), 1);

        for (fd = 3; fd < 256; fd++)
            close(fd);

        execvp(argv[0], argv);
        _exit(127);
    }

    close(pfd[1]);
    close(spfd[0]);     /* 写端留在父进程手里，保证子进程读不到 EOF */

    /*
     * 必须读到 EOF，否则子进程写满管道会阻塞。
     * 带超时是为了兜住「远端卡住不返回」的情况 —— 没有看门狗时
     * 整个请求会一直挂着，把 LuCI 的 XHR 拖到超时。
     */
    deadline = time(NULL) + ((timeout_sec > 0) ? timeout_sec : 0);

    for (;;) {
        if (timeout_sec > 0) {
            fd_set rfds;
            struct timeval tv;
            int remain = (int)(deadline - time(NULL));

            if (remain <= 0) {
                timed_out = 1;
                break;
            }

            FD_ZERO(&rfds);
            FD_SET(pfd[0], &rfds);
            tv.tv_sec = (remain > 1) ? 1 : remain;
            tv.tv_usec = 0;

            if (select(pfd[0] + 1, &rfds, NULL, NULL, &tv) < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }

            if (!FD_ISSET(pfd[0], &rfds))
                continue;
        }

        n = read(pfd[0], chunk, sizeof(chunk));
        if (n <= 0)
            break;

        if (used + (size_t)n < outsz) {
            memcpy(out + used, chunk, (size_t)n);
            used += (size_t)n;
            out[used] = '\0';
        }
    }

    if (timed_out) {
        kill(pid, SIGTERM);
        usleep(200000);
        kill(pid, SIGKILL);
    }

    close(pfd[0]);

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }

    close(spfd[1]);

    if (timed_out)
        return 124;         /* 与 timeout(1) 的约定一致 */

    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    return -1;
}

/*
 * cc_home_dir - 取当前用户的 home 目录，永不返回 NULL。
 *
 * 为什么要自己算：dbclient 用 $HOME 展开默认私钥路径 ~/.ssh/id_dropbear，
 * 而 procd 拉起的进程 HOME 往往是空的、甚至存在但为空串。
 * 一旦展开失败，dbclient 会「静默跳过」这个私钥（源码里 loadidentityfile
 * 的 warnfail 参数是 0，不打印任何提示），接着退回密码认证；
 * 而没有终端时 getpass() 读不到密码，直接抛
 * "Connection to ... exited: Interrupted."
 * —— 这就是「命令行里能关、插件里不行」的真正原因。
 */
static const char *cc_home_dir(void)
{
    static char home[160];
    struct passwd *pw;

    pw = getpwuid(getuid());

    if (pw != NULL && pw->pw_dir != NULL && pw->pw_dir[0] != '\0') {
        cc_strlcpy(home, pw->pw_dir, sizeof(home));
        return home;
    }

    if (getenv("HOME") != NULL && getenv("HOME")[0] != '\0') {
        cc_strlcpy(home, getenv("HOME"), sizeof(home));
        return home;
    }

    cc_strlcpy(home, "/root", sizeof(home));
    return home;
}

/*
 * json_get_code - 从巴法云的 JSON 响应里取 code 字段。
 * 响应形如 {"code":"40212","status":"topic error"} 或 {"code":0,...}。
 * 找到返回 1 并把数值写入 *code，找不到返回 0。
 */
static int json_get_code(const char *resp, long *code)
{
    const char *p;
    char *end = NULL;

    if (resp == NULL || code == NULL)
        return 0;

    p = strstr(resp, "\"code\"");
    if (p == NULL)
        return 0;

    p += 6;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != ':')
        return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '"')
        p++;

    *code = strtol(p, &end, 10);

    return (end != p) ? 1 : 0;
}

/*
 * find_bin - 在常见目录里定位可执行文件的绝对路径，找到返回 0。
 *
 * 为什么要自己找：procd 拉起的进程环境变量和登录 shell 不一样，
 * PATH 可能不全。只写 "ssh"/"sshpass" 一旦 PATH 缺失就会以 exit=127
 * 失败，而 127 只表示「命令没找到」，很容易被误读成密码错误。
 */
static int find_bin(const char *name, char *out, size_t outsz)
{
    static const char *dirs[] = {
        "/usr/bin", "/usr/sbin", "/bin", "/sbin", "/usr/local/bin", NULL
    };
    int i;

    if (name == NULL || out == NULL || outsz == 0)
        return -1;

    /* 已经是路径就直接校验 */
    if (strchr(name, '/') != NULL) {
        if (access(name, X_OK) == 0) {
            cc_strlcpy(out, name, outsz);
            return 0;
        }
        return -1;
    }

    for (i = 0; dirs[i] != NULL; i++) {
        char path[256];

        snprintf(path, sizeof(path), "%s/%s", dirs[i], name);
        if (access(path, X_OK) == 0) {
            cc_strlcpy(out, path, outsz);
            return 0;
        }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* 配置读取                                                           */
/* ------------------------------------------------------------------ */

/* 配置文件路径（与 LuCI 页面、init 脚本保持一致） */
#ifndef CFG_PATH
#define CFG_PATH "/etc/config/cloud_control"
#endif

/*
 * uci_unquote - 去掉首尾空白与包裹的引号，并处理行尾注释。
 *
 * UCI 的值标准写法是 'value'，但也可能不带引号；两种都兼容。
 * 这个函数会原地修改字符串。
 */
static void uci_unquote(char *s)
{
    char *b = s;
    char *e;
    char q;

    if (s == NULL)
        return;

    while (*b == ' ' || *b == '\t')
        b++;

    e = b + strlen(b);
    while (e > b && (e[-1] == ' ' || e[-1] == '\t' ||
                     e[-1] == '\r' || e[-1] == '\n'))
        e--;
    *e = '\0';

    if (*b == '\'' || *b == '"') {
        q = *b;
        b++;
        e = strrchr(b, q);
        if (e != NULL)
            *e = '\0';
    } else {
        /* 无引号形态：把 # 之后的内容当注释丢弃 */
        e = strchr(b, '#');
        if (e != NULL)
            *e = '\0';

        e = b + strlen(b);
        while (e > b && (e[-1] == ' ' || e[-1] == '\t'))
            e--;
        *e = '\0';
    }

    if (b != s)
        memmove(s, b, strlen(b) + 1);
}

/* 把一条 option 的键值写进配置结构体；不认识的名字直接忽略。 */
static void cfg_apply(cc_config_t *cfg, const char *key, const char *val)
{
    if (strcmp(key, "enabled") == 0)
        cc_strlcpy(cfg->enabled, val, sizeof(cfg->enabled));
    else if (strcmp(key, "client_id") == 0)
        cc_strlcpy(cfg->client_id, val, sizeof(cfg->client_id));
    else if (strcmp(key, "topic") == 0)
        cc_strlcpy(cfg->topic, val, sizeof(cfg->topic));
    else if (strcmp(key, "host") == 0)
        cc_strlcpy(cfg->host, val, sizeof(cfg->host));
    else if (strcmp(key, "ssh_port") == 0)
        cc_strlcpy(cfg->ssh_port, val, sizeof(cfg->ssh_port));
    else if (strcmp(key, "user") == 0)
        cc_strlcpy(cfg->user, val, sizeof(cfg->user));
    else if (strcmp(key, "password") == 0)
        cc_strlcpy(cfg->password, val, sizeof(cfg->password));
    else if (strcmp(key, "nic") == 0)
        cc_strlcpy(cfg->nic, val, sizeof(cfg->nic));
    else if (strcmp(key, "mac") == 0)
        cc_strlcpy(cfg->mac, val, sizeof(cfg->mac));
    else if (strcmp(key, "host_os") == 0)
        cc_strlcpy(cfg->host_os, val, sizeof(cfg->host_os));
    else if (strcmp(key, "shutdown_cmd") == 0)
        cc_strlcpy(cfg->shutdown_cmd, val, sizeof(cfg->shutdown_cmd));
    else if (strcmp(key, "log_max_kb") == 0)
        cc_strlcpy(cfg->log_max_kb, val, sizeof(cfg->log_max_kb));
    else if (strcmp(key, "verify_offline") == 0)
        cc_strlcpy(cfg->verify_offline, val, sizeof(cfg->verify_offline));
    else if (strcmp(key, "ip") == 0) {
        /* 向后兼容：老版本用 option ip 表示目标机地址，且不得覆盖 host */
        if (cfg->host[0] == '\0')
            cc_strlcpy(cfg->host, val, sizeof(cfg->host));
    }
}

/* 计算派生参数（日志上限），每次读入新配置后调用。 */
static void cfg_finish(cc_config_t *cfg)
{
    long kb = strtol(cfg->log_max_kb, NULL, 10);

    if (kb < 16)
        kb = LOG_DEFAULT_KB;
    if (kb > 1024 * 64)
        kb = 1024 * 64;             /* 最大 64MB，防止误配置 */
    g_log_max_bytes = kb * 1024L;
}

/*
 * cfg_load - 解析 /etc/config/cloud_control。
 *
 * 只认 "config main" 这一节里的 "option key value"。
 * 返回 0 表示读到了 main 节；-1 表示文件缺失或没有 main 节
 * （此时 cfg 里是内置默认值，调用方仍可继续运行）。
 */
static int cfg_load(cc_config_t *cfg)
{
    FILE *fp;
    char line[512];
    char key[128];
    char val[256];
    int in_main = 0;
    int found = 0;

    if (cfg == NULL)
        return -1;

    memset(cfg, 0, sizeof(*cfg));

    /* 默认值：配置文件缺项时以这里为准 */
    cc_strlcpy(cfg->enabled, "0", sizeof(cfg->enabled));
    cc_strlcpy(cfg->topic, "PC001", sizeof(cfg->topic));
    cc_strlcpy(cfg->ssh_port, "22", sizeof(cfg->ssh_port));
    cc_strlcpy(cfg->nic, "br-lan", sizeof(cfg->nic));
    cc_strlcpy(cfg->host_os, "windows", sizeof(cfg->host_os));
    cc_strlcpy(cfg->log_max_kb, "256", sizeof(cfg->log_max_kb));
    cc_strlcpy(cfg->verify_offline, "1", sizeof(cfg->verify_offline));

    fp = fopen(CFG_PATH, "r");
    if (fp == NULL) {
        log_write("WARN", "无法打开 %s（%s），使用默认配置",
                  CFG_PATH, strerror(errno));
        cfg_finish(cfg);
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = line;
        char *e;

        while (*p == ' ' || *p == '\t')
            p++;

        /* 空行与整行注释 */
        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#')
            continue;

        if (strncmp(p, "config", 6) == 0 &&
            (p[6] == ' ' || p[6] == '\t' || p[6] == '\0')) {
            size_t n = 0;

            p += 6;
            while (*p == ' ' || *p == '\t')
                p++;

            while (p[n] != '\0' && p[n] != ' ' && p[n] != '\t' &&
                   p[n] != '\r' && p[n] != '\n')
                n++;

            /* 只关心 type 为 main 的节，节名（匿名的或 'main'）不影响判断 */
            in_main = (n == strlen(CFG_SECTION_TYPE) &&
                       strncmp(p, CFG_SECTION_TYPE, n) == 0);
            if (in_main)
                found = 1;
            continue;
        }

        if (!in_main)
            continue;

        if (strncmp(p, "option", 6) != 0 ||
            !(p[6] == ' ' || p[6] == '\t'))
            continue;

        p += 6;
        while (*p == ' ' || *p == '\t')
            p++;

        /* key */
        e = p;
        while (*e != '\0' && *e != ' ' && *e != '\t' &&
               *e != '\r' && *e != '\n')
            e++;

        if ((size_t)(e - p) >= sizeof(key))
            continue;

        memcpy(key, p, (size_t)(e - p));
        key[e - p] = '\0';

        /* value */
        while (*e == ' ' || *e == '\t')
            e++;

        cc_strlcpy(val, e, sizeof(val));
        uci_unquote(val);

        if (val[0] == '\0' && strcmp(key, "client_id") != 0 &&
            strcmp(key, "password") != 0 && strcmp(key, "host") != 0)
            continue;

        cfg_apply(cfg, key, val);
    }

    fclose(fp);

    if (!found)
        log_write("WARN", "%s 中没有 config %s 节，使用默认配置",
                  CFG_PATH, CFG_SECTION_TYPE);

    cfg_finish(cfg);
    return found ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* 巴法云上报                                                         */
/* ------------------------------------------------------------------ */

/* 向巴法云推送设备状态（on / off），返回 curl 退出码。 */
static int bemfa_push(const cc_config_t *cfg, const char *msg)
{
    char uid_enc[384];
    char topic_enc[384];
    char url[1024];
    char *argv[8];
    char curl_bin[256];
    int rc;

    if (cfg == NULL || msg == NULL)
        return -1;

    if (cfg->client_id[0] == '\0') {
        log_write("WARN", "未配置 client_id，跳过云端上报");
        return -1;
    }

    url_encode(cfg->client_id, uid_enc, sizeof(uid_enc));
    url_encode(cfg->topic, topic_enc, sizeof(topic_enc));

    snprintf(url, sizeof(url), BEMFA_PUSH_URL, uid_enc, topic_enc, msg);

    /* curl 参数全部为常量，URL 已做百分号编码，无 shell 参与 */
    if (find_bin("curl", curl_bin, sizeof(curl_bin)) != 0) {
        log_write("WARN", "未安装 curl，跳过云端状态上报");
        return -1;
    }

    argv[0] = curl_bin;
    argv[1] = (char *)"-s";
    argv[2] = (char *)"-f";     /* 业务错误码返回非 0，避免误记为上报成功 */
    argv[3] = (char *)"--max-time";
    argv[4] = (char *)"8";
    argv[5] = url;
    argv[6] = NULL;

    rc = run_argv(argv, NULL, NULL);
    if (rc != 0) {
        log_write("WARN", "云端上报 msg=%s 失败（curl exit=%d）", msg, rc);
    } else {
        /* 只有真正发出去了才记录，供 handle_cmd 做自回显去重 */
        cc_strlcpy(g_last_push_msg, msg, sizeof(g_last_push_msg));
        g_last_push_time = time(NULL);
    }

    return rc;
}

/* ------------------------------------------------------------------ */
/* Wake-on-LAN                                                        */
/* ------------------------------------------------------------------ */

/* 发送 WoL 魔术包，返回 0 表示成功。 */
/*
 * parse_mac - 把 AA:BB:CC:DD:EE:FF 解析成 6 个字节，成功返回 0。
 * 兼容 '-' 与 ':' 两种分隔符。
 */
static int parse_mac(const char *str, unsigned char out[6])
{
    const char *p = str;
    int i;

    if (str == NULL || out == NULL)
        return -1;

    while (*p == ' ')
        p++;

    for (i = 0; i < 6; i++) {
        unsigned int b = 0;
        int digits = 0;

        while (*p != '\0' && *p != ':' && *p != '-') {
            char c = *p;
            int v;

            if (c >= '0' && c <= '9')
                v = c - '0';
            else if (c >= 'a' && c <= 'f')
                v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                v = c - 'A' + 10;
            else
                return -1;

            if (digits >= 2)
                return -1;

            b = (b << 4) | (unsigned int)v;
            digits++;
            p++;
        }

        if (digits == 0)
            return -1;

        out[i] = (unsigned char)b;

        if (i < 5) {
            if (*p != ':' && *p != '-')
                return -1;
            p++;
        }
    }

    while (*p == ' ')
        p++;

    return (*p == '\0') ? 0 : -1;
}

/*
 * do_wol - 直接构造并广播 Wake-on-LAN 魔术包。
 *
 * 不再调用 etherwake：少一个外部依赖，也避免不同版本 etherwake
 * 参数集不一致导致的「发送失败」误报。
 *
 * 多网口设备（软路由）必须用 SO_BINDTODEVICE 指定出口网卡，
 * 否则广播可能从 WAN 侧发出去，目标机永远收不到。
 */
static int do_wol(const cc_config_t *cfg, int verbose)
{
    unsigned char mac[6];
    unsigned char pkt[102];
    struct sockaddr_in dst;
    ssize_t n;
    int sock;
    int one = 1;
    int bound = 0;
    int i;

    (void)verbose;

    if (cfg == NULL)
        return -1;

    if (cfg->mac[0] == '\0') {
        log_write("ERROR", "WoL 参数不完整：MAC 为空");
        return -1;
    }

    if (parse_mac(cfg->mac, mac) != 0) {
        log_write("ERROR", "MAC 地址格式错误：%s（应形如 AA:BB:CC:DD:EE:FF）",
                  cfg->mac);
        return -1;
    }

    /* 6 字节 0xFF + MAC 重复 16 次 = 102 字节 */
    memset(pkt, 0xFF, 6);
    for (i = 0; i < 16; i++)
        memcpy(pkt + 6 + i * 6, mac, 6);

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        log_write("ERROR", "创建 socket 失败：%s", strerror(errno));
        return -1;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) < 0)
        log_write("WARN", "设置 SO_BROADCAST 失败：%s", strerror(errno));

    if (cfg->nic[0] != '\0') {
        if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE,
                       cfg->nic, (socklen_t)(strlen(cfg->nic) + 1)) == 0) {
            bound = 1;
        } else {
            log_write("WARN", "绑定网卡 %s 失败：%s（改用默认路由广播）",
                      cfg->nic, strerror(errno));
        }
    }

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(9);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    n = sendto(sock, pkt, sizeof(pkt), 0,
               (struct sockaddr *)&dst, sizeof(dst));
    close(sock);

    if (n != (ssize_t)sizeof(pkt)) {
        log_write("ERROR", "发送魔术包失败：%s", strerror(errno));
        return -1;
    }

    log_write("INFO", "已向 %s 广播唤醒包（出口：%s）",
              cfg->mac, bound ? cfg->nic : "默认路由");

    return 0;
}

/* ------------------------------------------------------------------ */
/* SSH 关机                                                           */
/* ------------------------------------------------------------------ */

/*
 * ssh 客户端实现：OpenWrt 默认是 dropbear 的 dbclient，也可能装了 openssh-client。
 * 两者命令行不兼容，必须分别下参数。
 */
#define SSH_FLAVOR_UNKNOWN  0
#define SSH_FLAVOR_DROPBEAR 1
#define SSH_FLAVOR_OPENSSH  2

/*
 * detect_ssh_flavor - 判断 ssh 客户端是 dropbear 还是 OpenSSH。
 *
 * 为什么必须判断：
 *   OpenSSH 用 -o StrictHostKeyChecking=no 跳过首次连接的主机指纹确认；
 *   dropbear(dbclient) 完全不认 -o，这些选项会被静默忽略，
 *   然后 dbclient 在非交互环境下等待「Do you want to continue connecting?」
 *   的键盘输入，最终以 "Didn't validate host key" 退出。
 *   只看退出码根本无法定位，实际表现就是「SSH 关机失败 exit=1」。
 */
static int detect_ssh_flavor(const char *ssh_bin)
{
    char out[512];
    char lnk[256];
    char *argv[3];
    ssize_t n;

    if (ssh_bin == NULL)
        return SSH_FLAVOR_UNKNOWN;

    /* 两种实现都支持 -V，会立刻打印版本并退出 */
    argv[0] = (char *)ssh_bin;
    argv[1] = (char *)"-V";
    argv[2] = NULL;

    if (run_argv_capture(argv, NULL, NULL, 5, out, sizeof(out)) >= 0) {
        if (strstr(out, "Dropbear") != NULL || strstr(out, "dropbear") != NULL)
            return SSH_FLAVOR_DROPBEAR;
        if (strstr(out, "OpenSSH") != NULL)
            return SSH_FLAVOR_OPENSSH;
    }

    /* 兜底一：OpenWrt 里 /usr/bin/ssh 通常是指向 dropbear 的符号链接 */
    n = readlink(ssh_bin, lnk, sizeof(lnk) - 1);
    if (n > 0) {
        lnk[n] = '\0';
        if (strstr(lnk, "dropbear") != NULL)
            return SSH_FLAVOR_DROPBEAR;
    }

    /* 兜底二：系统里装了 dropbear 服务端，基本可以认定客户端也是它 */
    if (access("/usr/sbin/dropbear", X_OK) == 0)
        return SSH_FLAVOR_DROPBEAR;

    return SSH_FLAVOR_UNKNOWN;
}

/*
 * log_remote_output - 把远端命令的输出压成一行写进日志。
 * 远端报错（主机密钥未确认、认证失败、权限不足）都会在这里显形，
 * 不用再去路由器终端手动重跑一遍。
 */
static void log_remote_output(const char *level, const char *out)
{
    char flat[512];
    const char *p;
    size_t o = 0;

    if (out == NULL || out[0] == '\0')
        return;

    for (p = out; *p != '\0' && o + 2 < sizeof(flat); p++) {
        if (*p == '\r')
            continue;
        if (*p == '\n') {
            if (o > 0 && flat[o - 1] != ' ')
                flat[o++] = ' ';
            continue;
        }
        flat[o++] = *p;
    }

    flat[o] = '\0';

    log_write(level, "   远端输出：%s", flat);
}

/* 根据配置推导关机命令：自定义命令优先，其次按 host_os 选择。 */
static void build_shutdown_cmd(const cc_config_t *cfg, char *out, size_t outsz)
{
    if (cfg->shutdown_cmd[0] != '\0')
        cc_strlcpy(out, cfg->shutdown_cmd, outsz);
    else if (strcasecmp(cfg->host_os, "linux") == 0)
        cc_strlcpy(out, "shutdown -h now", outsz);
    else
        cc_strlcpy(out, "shutdown /s /t 10 /f", outsz);
}

/* 通过 SSH 执行关机，返回 0 表示 SSH 命令成功执行。 */
static int do_ssh_shutdown(const cc_config_t *cfg, int verbose)
{
    char target[256];
    char cmd[256];
    char ssh_bin[256];
    char sshpass_bin[256];
    char keypath[320];
    char ssh_out[4096];
    char *argv[20];
    int idx = 0;
    int rc;
    int flavor;
    const char *env_name = NULL;
    const char *env_val = NULL;

    g_ssh_inconclusive = 0;

    if (cfg == NULL)
        return -1;

    if (cfg->host[0] == '\0' || cfg->user[0] == '\0') {
        log_write("ERROR", "SSH 参数不完整：host=[%s] user=[%s]", cfg->host, cfg->user);
        return -1;
    }

    snprintf(target, sizeof(target), "%s@%s", cfg->user, cfg->host);
    build_shutdown_cmd(cfg, cmd, sizeof(cmd));

    /*
     * 用绝对路径定位外部命令。
     * 只写 "ssh"/"sshpass" 时，一旦 procd 给的环境 PATH 不全，就会以
     * exit=127 失败 —— 而 127 只表示「命令没找到」，很容易被误读成
     * 密码错误或网络不通，所以这里先解析路径并给出明确提示。
     */
    if (find_bin("ssh", ssh_bin, sizeof(ssh_bin)) != 0) {
        log_write("ERROR", "找不到 ssh 客户端（应为 /usr/bin/ssh，由 dropbear 提供）");
        return 127;
    }

    /*
     * 仅当配置了密码时才走 sshpass；
     * -e 表示从环境变量 SSHPASS 读取口令，避免密码出现在 ps 命令行里。
     */
    if (cfg->password[0] != '\0') {
        if (find_bin("sshpass", sshpass_bin, sizeof(sshpass_bin)) != 0) {
            log_write("ERROR", "找不到 sshpass —— 配置里填了密码，但路由器没有这个工具");
            log_write("ERROR", "  解决一：apk add sshpass（opkg 固件执行 opkg install sshpass）");
            log_write("ERROR", "  解决二：改用密钥登录，把公钥装到目标机，配置里密码留空即可");
            return 127;
        }

        argv[idx++] = sshpass_bin;
        argv[idx++] = (char *)"-e";
        env_name = "SSHPASS";
        env_val = cfg->password;
    }

    flavor = detect_ssh_flavor(ssh_bin);

    /*
     * 密码为空说明走密钥认证。此时必须显式用 -i 指定私钥路径，
     * 绝不能依赖 dbclient 自己去展开 ~/.ssh/id_dropbear ——
     * 它展开用的是 $HOME，而守护进程的 HOME 不可靠，
     * 展开失败时又不报错，只会静默退到密码认证，最后以
     * "exited: Interrupted." 收场，完全看不出是密钥没找到。
     */
    keypath[0] = '\0';

    if (cfg->password[0] == '\0') {
        if (flavor == SSH_FLAVOR_OPENSSH) {
            static const char *names[] = { "id_ed25519", "id_rsa", "id_ecdsa", NULL };
            int k;

            for (k = 0; names[k] != NULL; k++) {
                snprintf(keypath, sizeof(keypath), "%s/.ssh/%s", cc_home_dir(), names[k]);
                if (access(keypath, R_OK) == 0)
                    break;
                keypath[0] = '\0';
            }
        } else {
            snprintf(keypath, sizeof(keypath), "%s/.ssh/id_dropbear", cc_home_dir());
            if (access(keypath, R_OK) != 0)
                keypath[0] = '\0';
        }

        if (keypath[0] == '\0') {
            log_write("ERROR", "未找到可用的私钥（已在 %s/.ssh 下查找）", cc_home_dir());
            log_write("ERROR", "  生成密钥：dropbearkey -t ed25519 -f %s/.ssh/id_dropbear",
                      cc_home_dir());
            log_write("ERROR", "  再把公钥写入目标机的 administrators_authorized_keys");
            log_write("ERROR", "  或者：在页面里填 SSH 密码（需路由器已安装 sshpass）");
            return 1;
        }

        if (verbose)
            log_write("INFO", "使用私钥：%s", keypath);
    }

    argv[idx++] = ssh_bin;

    if (flavor == SSH_FLAVOR_OPENSSH) {
        argv[idx++] = (char *)"-p";
        argv[idx++] = (char *)cfg->ssh_port;
        argv[idx++] = (char *)"-o";
        argv[idx++] = (char *)"StrictHostKeyChecking=no";
        argv[idx++] = (char *)"-o";
        argv[idx++] = (char *)"UserKnownHostsFile=/dev/null";
        argv[idx++] = (char *)"-o";
        argv[idx++] = (char *)"ConnectTimeout=8";
        argv[idx++] = (char *)"-o";
        argv[idx++] = (char *)"LogLevel=ERROR";
    } else {
        /*
         * dropbear(dbclient)：只认 -y（自动接受「未知」主机密钥），
         * 主机密钥发生变化时它仍会拒绝，安全性不打折。
         * 千万不要传 -o，它会把选项当无效参数忽略后转去问交互问题。
         *
         * 也不要加 -t：那是排障时的一次误判。真正的原因是认证没通过，
         * -t 只是让人能在终端里手动输密码；加了反而会因本地没终端而卡住。
         */
        if (verbose)
            argv[idx++] = (char *)"-v";     /* 测试模式打开认证过程，便于定位 */

        argv[idx++] = (char *)"-y";
        argv[idx++] = (char *)"-p";
        argv[idx++] = (char *)cfg->ssh_port;
    }

    if (keypath[0] != '\0') {
        argv[idx++] = (char *)"-i";
        argv[idx++] = keypath;
    }

    argv[idx++] = target;
    argv[idx++] = cmd;      /* 整条关机命令作为单个参数，不再二次拆分 */
    argv[idx] = NULL;

    /*
     * 测试模式把「真正执行的命令行」「客户端类型」「运行环境」摊开。
     * 排障最怕的是「你以为它跑的是 A，其实跑的是 B」。
     */
    if (verbose) {
        char line[900];
        size_t used = 0;
        int i;

        line[0] = '\0';

        for (i = 0; argv[i] != NULL && used + 1 < sizeof(line); i++) {
            int w = snprintf(line + used, sizeof(line) - used, "%s%s",
                             (i > 0) ? " " : "", argv[i]);

            if (w <= 0 || (size_t)w >= sizeof(line) - used)
                break;

            used += (size_t)w;
        }

        log_write("INFO", "客户端：%s",
                  flavor == SSH_FLAVOR_OPENSSH ? "OpenSSH" :
                  (flavor == SSH_FLAVOR_DROPBEAR ? "dropbear(dbclient)" : "未知"));
        log_write("INFO", "实际执行：%s", line);
        log_write("INFO", "HOME：父进程=[%s] 子进程强制=[%s]",
                  getenv("HOME") ? getenv("HOME") : "未设置", cc_home_dir());
    }

    /* 捕获远端输出：失败时把原因写进日志，用户不用再手动重跑 */
    rc = run_argv_capture(argv, env_name, env_val, SSH_TIMEOUT_SEC,
                          ssh_out, sizeof(ssh_out));

    if (rc == 124) {
        log_write("ERROR", "SSH 执行超时（%d 秒未返回），已强制结束：%s@%s:%s",
                  SSH_TIMEOUT_SEC, cfg->user, cfg->host, cfg->ssh_port);
        log_remote_output("ERROR", ssh_out);
        return 124;
    }

    if (rc != 0) {
        log_write("ERROR", "SSH 关机失败（exit=%d）：%s@%s:%s，客户端=%s",
                  rc, cfg->user, cfg->host, cfg->ssh_port,
                  flavor == SSH_FLAVOR_OPENSSH ? "OpenSSH" :
                  (flavor == SSH_FLAVOR_DROPBEAR ? "dropbear" : "未知"));
        log_remote_output("ERROR", ssh_out);
    }
    else {
        log_write("INFO", "SSH 关机指令已下发：%s@%s -> %s",
                  cfg->user, cfg->host, cmd);

        /*
         * exit=0 只说明「ssh 连上了、远端命令返回 0」，
         * 不等于 Windows 真的会关机（可能被电源策略、组策略、
         * Shutdown Event Tracker 拦下，或连到了别的主机）。
         * 成功时也把远端回显打出来，让结论有据可查。
         */
        log_remote_output("INFO", ssh_out);

        /*
         * 连接被中断通常有两种含义，无法区分：
         *   1) 关机生效、sshd 被一起结束（正常，但我们拿不到真实退出码）
         *   2) 网络/认证中途掉线（实际没关）
         * /t 0 这种立即关机的命令必然落到这里，此时 exit=0 是假信号，
         * 不能据此断言「成功」。标记出来，由调用方如实展示。
         */
        if (strstr(ssh_out, "Interrupted") != NULL ||
            strstr(ssh_out, "interrupted") != NULL) {
            g_ssh_inconclusive = 1;
            log_write("WARN", "连接被中断，无法确认远端是否真的关机"
                              "（若用 /t 0，sshd 会先被关掉；建议改成 /t 10 之类留出回执时间）");
        }
    }

    return rc;
}

/* ------------------------------------------------------------------ */
/* 消息分发                                                           */
/* ------------------------------------------------------------------ */

/* 判断字符是否为 msg 字段值之后的分隔符。 */
static int is_value_delim(char c)
{
    return (c == '\0' || c == '&' || c == '\r' || c == '\n' ||
            c == '"' || c == '\'' || c == '}' || c == ',' ||
            c == ';' || c == '|' || c == ' ' || c == '\t');
}

/*
 * 提取 msg / message 字段的值（按字段边界解析）。
 *
 * 为什么不能只在字符层面找 "msg"：
 *   xmsg=on 会被误判成开机；更危险的是 topic=msg=on —— 若主题名恰好是
 *   msg=on，订阅回执 cmd=1&uid=xxx&topic=msg=on 会被当成开机指令，
 *   每次重连都发一次魔术包。所以必须「先切字段、再比字段名」。
 *
 * 做法：
 *   1. 按 '&' 和 ',' 切成 chunk（前者是 bemfa 参数分隔符，后者是 JSON）；
 *   2. 每个 chunk 里取第一个 '=' 或 ':'，左边是字段名，右边是字段值；
 *   3. 字段名去掉首尾的 { } " ' 和空白后，与 msg / message 比较；
 *   4. 命中则返回 1 并写出字段值（去引号空白，截到分隔符）。
 *
 * 兼容：msg=on / topic=PC001&msg=off / {"msg":"off"} / msg='on'
 * 返回 1 表示命中且值非空，0 表示没有可用字段。
 */
static int extract_msg_value(const char *line, char *out, size_t outsz)
{
    const char *p;

    if (line == NULL || out == NULL || outsz == 0)
        return 0;

    out[0] = '\0';
    p = line;

    while (*p != '\0') {
        const char *start = p;
        const char *end;
        const char *eq = NULL;
        const char *q;
        const char *ns;
        const char *ne;
        const char *vs;
        size_t namelen;
        size_t o;

        /* 1) 切出一个字段 chunk：[start, end) */
        while (*p != '\0' && *p != '&' && *p != ',')
            p++;
        end = p;
        if (*p == '&' || *p == ',')
            p++;                        /* 跳过分隔符，进入下一轮 */

        /* 2) chunk 内第一个 '=' 或 ':' 左边是字段名 */
        for (q = start; q < end; q++) {
            if (*q == '=' || *q == ':') {
                eq = q;
                break;
            }
        }

        /* 裸词（如 "PC001 on" 里的 PC001）没有字段名，跳过 */
        if (eq == NULL)
            continue;

        /* 3) 字段名去掉首尾的 { } " ' 和空白 */
        ns = start;
        while (ns < eq && (isspace((unsigned char)*ns) || *ns == '{' ||
                           *ns == '}' || *ns == '"' || *ns == '\''))
            ns++;

        ne = eq;
        while (ne > ns && (isspace((unsigned char)ne[-1]) || ne[-1] == '{' ||
                           ne[-1] == '}' || ne[-1] == '"' || ne[-1] == '\''))
            ne--;

        namelen = (size_t)(ne - ns);

        if (!((namelen == 3 && strncasecmp(ns, "msg", 3) == 0) ||
              (namelen == 7 && strncasecmp(ns, "message", 7) == 0)))
            continue;

        /* 4) 取字段值：去掉前导引号与空白，截到分隔符为止 */
        vs = eq + 1;
        while (vs < end && (isspace((unsigned char)*vs) || *vs == '"' ||
                            *vs == '\''))
            vs++;

        o = 0;
        while (vs < end && !is_value_delim(*vs)) {
            if (o + 1 < outsz)
                out[o++] = *vs;
            vs++;
        }
        out[o] = '\0';

        return (o > 0) ? 1 : 0;
    }

    return 0;
}

/*
 * 从一行报文里识别开/关命令：返回 1=on，2=off，0=无关报文。
 *
 * 安全约束（关键）：绝不允许对整行做「逐 token 匹配」。
 * 巴法云在订阅成功后会下发形如 cmd=1&uid=...&topic=... 的报文，其中裸 token
 * "1" 会被误判成开机 —— 每连一次就唤醒一次电脑；同理任何含 "0" 的报文都会
 * 误触发关机。这是一个能真正关机的插件，判定必须从严。
 *
 * 判定顺序：
 *   1. 优先只认 msg= / message= 字段的值；
 *   2. 取不到字段时，仅当整行 trim 后精确等于 on/off/open/close/1/0；
 *   3. off 优先于 on。
 */
static int parse_cmd(const char *line)
{
    char trimmed[LINE_MAX_LEN];
    char value[64];
    const char *src;
    size_t n;

    if (line == NULL)
        return 0;

    /* 1) 优先解析 msg / message 字段 */
    if (extract_msg_value(line, value, sizeof(value))) {
        if (strcasecmp(value, "off") == 0 || strcasecmp(value, "close") == 0 ||
            strcmp(value, "0") == 0)
            return 2;
        if (strcasecmp(value, "on") == 0 || strcasecmp(value, "open") == 0 ||
            strcmp(value, "1") == 0)
            return 1;
        /* 有字段但不是开关指令（如 msg=hello），一律忽略 */
        return 0;
    }

    /* 2) 无字段：整行 trim 后精确比较，不做任何切片 */
    src = line;
    while (*src != '\0' && isspace((unsigned char)*src))
        src++;

    n = strlen(src);
    if (n >= sizeof(trimmed))
        n = sizeof(trimmed) - 1;

    memcpy(trimmed, src, n);
    trimmed[n] = '\0';

    while (n > 0 && isspace((unsigned char)trimmed[n - 1]))
        trimmed[--n] = '\0';

    if (strcasecmp(trimmed, "off") == 0 || strcasecmp(trimmed, "close") == 0 ||
        strcmp(trimmed, "0") == 0)
        return 2;
    if (strcasecmp(trimmed, "on") == 0 || strcasecmp(trimmed, "open") == 0 ||
        strcmp(trimmed, "1") == 0)
        return 1;

    return 0;
}

/* 前置声明：关机确认阶段要靠心跳保活，避免长连接被判超时 */
static int bemfa_ping(int sock);

/*
 * probe_tcp - 静默探测目标端口：连上返回 0，连不上或超时返回 -1。
 * 与 tcp_connect 的区别是不打日志 —— 关机确认阶段会连续探测几十次，
 * 每次都记一条会把日志写满。
 */
static int probe_tcp(const char *host, int port, int timeout_sec)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai = NULL;
    char portstr[16];
    int ret;

    if (host == NULL || port <= 0)
        return -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(portstr, sizeof(portstr), "%d", port);

    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        int sock;
        int err = 0;
        socklen_t elen = sizeof(err);
        fd_set wfds;
        struct timeval tv;
        int flags;

        sock = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC,
                      ai->ai_protocol);
        if (sock < 0)
            continue;

        flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        ret = connect(sock, ai->ai_addr, ai->ai_addrlen);
        if (ret == 0) {
            close(sock);
            freeaddrinfo(res);
            return 0;
        }

        if (errno == EINPROGRESS) {
            FD_ZERO(&wfds);
            FD_SET(sock, &wfds);
            tv.tv_sec = timeout_sec;
            tv.tv_usec = 0;

            if (select(sock + 1, NULL, &wfds, NULL, &tv) > 0 &&
                getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 &&
                err == 0) {
                close(sock);
                freeaddrinfo(res);
                return 0;
            }
        }

        close(sock);
    }

    if (res != NULL)
        freeaddrinfo(res);

    return -1;
}

/*
 * wait_host_offline - 关机指令下发后，轮询确认目标机是否真的离线。
 *
 * 为什么不用 ping：Windows 防火墙默认丢弃 ICMP，机器开着也 ping 不通，
 * 会把「在线」误判成「已关机」——正好是最危险的误判方向。
 * 这里探测的是 SSH 端口，也就是刚才成功登录用的那条通道，判断最准。
 *
 * 连续 VERIFY_FAILS_NEEDED 次连不上才判定离线，避免一次网络抖动就下结论。
 * 等待期间保持巴法云心跳，否则长连接会被服务端判超时。
 *
 * 返回 1 确认离线，0 超时仍在线。
 */
static int wait_host_offline(int sock, const cc_config_t *cfg, int timeout_sec)
{
    int port = atoi(cfg->ssh_port);
    int fails = 0;
    time_t start = time(NULL);
    time_t last_hb = start;

    if (port <= 0)
        port = 22;

    log_write("INFO", "等待 %s 离线（最多 %d 秒，探测端口 %d）...",
              cfg->host, timeout_sec, port);

    while ((int)(time(NULL) - start) < timeout_sec) {
        if (probe_tcp(cfg->host, port, 2) != 0) {
            fails++;
            if (fails >= VERIFY_FAILS_NEEDED) {
                log_write("INFO", "目标机已离线（连续 %d 次无响应）", fails);
                return 1;
            }
        } else {
            fails = 0;
        }

        if (sock >= 0 && time(NULL) - last_hb >= 10) {
            bemfa_ping(sock);
            last_hb = time(NULL);
        }

        sleep(3);
    }

    return 0;
}

/*
 * 处理一条云端指令。
 * 关键：先执行动作，成功后才上报云端，保证云端状态与真实一致。
 *
 * 进执行前必须过两道闸，否则会出真机事故：
 *   闸 1 自回显去重：bemfa_push 往同一 topic 推的消息会被本连接收回来，
 *        不去重就变成正反馈（关机后立刻被自己唤醒，或反复唤醒）。
 *   闸 2 同类指令限流：云端重复推送时不连发关机/开机包。
 */
static void handle_cmd(const cc_config_t *cfg, int kind, int sock)
{
    const char *push_msg;
    time_t now;
    int min_gap;
    int rc;

    if (kind != 1 && kind != 2)
        return;

    now = time(NULL);
    push_msg = (kind == 1) ? "on" : "off";

    /* on 失败后用户可能立刻重试，窗口要短；off 要挡住延迟回显，窗口要长 */
    min_gap = (kind == 1) ? ON_MIN_INTERVAL_SEC : OFF_MIN_INTERVAL_SEC;

    /* 闸 1：本条指令与我们刚上报的内容相同且在去重窗口内 -> 是自身回显 */
    if (g_last_push_msg[0] != '\0' &&
        strcmp(g_last_push_msg, push_msg) == 0 &&
        g_last_push_time > 0 &&
        now - g_last_push_time < ECHO_GUARD_SEC) {
        log_write("INFO", "忽略自身回显：%s（%ld 秒前刚上报）",
                  push_msg, (long)(now - g_last_push_time));
        return;
    }

    /* 闸 2：同一种指令在最小间隔内重复到达 -> 丢弃 */
    if (g_last_kind_time[kind] > 0 &&
        now - g_last_kind_time[kind] < min_gap) {
        log_write("INFO", "忽略重复指令：%s（%ld 秒前刚执行过，最小间隔 %d 秒）",
                  push_msg, (long)(now - g_last_kind_time[kind]), min_gap);
        return;
    }

    /* 先占位再执行：SSH 可能耗时数秒，避免执行期间重入 */
    g_last_kind_time[kind] = now;

    if (kind == 1) {
        /* 开机 */
        rc = do_wol(cfg, 0);
        if (rc == 0) {
            log_write("INFO", "收到 on：已向 %s 发送唤醒魔术包", cfg->mac);
        } else {
            log_write("ERROR", "收到 on：唤醒包发送失败（exit=%d）", rc);
        }
        /* 无论成败都回报 on（反映"已执行开机动作"的意图） */
        bemfa_push(cfg, "on");
    } else {
        /* 关机 */
        rc = do_ssh_shutdown(cfg, 0);
        if (rc != 0) {
            log_write("ERROR", "收到 off：SSH 关机失败（exit=%d），云端状态回滚为 on", rc);
            bemfa_push(cfg, "on");
            return;
        }

        /*
         * SSH 返回 0 只代表「指令被接受」，不代表机器真的关了：
         * /t 0 会让会话被掐断拿不到退出码，Windows 也可能因策略拒绝执行。
         * 所以按配置再确认一次——探测 SSH 端口，连不上了才算真的关掉。
         */
        if (strcmp(cfg->verify_offline, "1") == 0 &&
            wait_host_offline(sock, cfg, VERIFY_TIMEOUT_SEC) == 0) {
            log_write("ERROR",
                      "收到 off：SSH 指令已被接受，但 %d 秒内 %s 仍在响应，判定关机失败，"
                      "云端状态回滚为 on",
                      VERIFY_TIMEOUT_SEC, cfg->host);
            bemfa_push(cfg, "on");
            return;
        }

        log_write("INFO", "收到 off：关机已确认，上报云端 off");
        bemfa_push(cfg, "off");
    }
}

/* ------------------------------------------------------------------ */
/* 网络                                                               */
/* ------------------------------------------------------------------ */

/* 带超时的 TCP 连接，返回 socket 描述符或 -1。 */
static int tcp_connect(const char *host, int port, int timeout_sec)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai = NULL;
    char portstr[16];
    int sock = -1;
    int ret;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(portstr, sizeof(portstr), "%d", port);

    ret = getaddrinfo(host, portstr, &hints, &res);
    if (ret != 0) {
        log_write("ERROR", "解析 %s 失败：%s", host, gai_strerror(ret));
        return -1;
    }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        int flags;
        int err = 0;
        socklen_t elen = sizeof(err);
        fd_set wfds;
        struct timeval tv;

        /*
         * SOCK_CLOEXEC：长连接 socket 不能被 run_argv 的子进程继承，
         * 否则 curl/ssh 存活期间父进程 close() 也无法真正断开。
         */
        sock = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC,
                      ai->ai_protocol);
        if (sock < 0) {
            /* 老 musl / 内核不支持 SOCK_CLOEXEC 时退回普通创建 */
            sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (sock < 0)
                continue;
        }
        /* 不论走哪条路径都补一次 FD_CLOEXEC，双保险 */
        fcntl(sock, F_SETFD, FD_CLOEXEC);

        flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        ret = connect(sock, ai->ai_addr, ai->ai_addrlen);
        if (ret < 0 && errno != EINPROGRESS) {
            close(sock);
            sock = -1;
            continue;
        }

        if (ret < 0) {
            FD_ZERO(&wfds);
            FD_SET(sock, &wfds);
            tv.tv_sec = timeout_sec;
            tv.tv_usec = 0;

            ret = select(sock + 1, NULL, &wfds, NULL, &tv);
            if (ret <= 0) {
                close(sock);
                sock = -1;
                continue;
            }

            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
                close(sock);
                sock = -1;
                continue;
            }
        }

        /* 恢复阻塞模式 */
        fcntl(sock, F_SETFL, flags);
        break;
    }

    if (res != NULL)
        freeaddrinfo(res);

    return sock;
}

/* 完整发送一段数据，返回 0 成功。 */
static int send_all(int sock, const char *data, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(sock, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)n;
    }

    return 0;
}

/* 发送订阅报文，返回 0 成功。 */
static int bemfa_subscribe(int sock, const cc_config_t *cfg)
{
    char pkt[512];
    int len;
    char masked[32];

    if (cfg->client_id[0] == '\0') {
        log_write("ERROR", "client_id 为空，无法订阅主题");
        return -1;
    }

    len = snprintf(pkt, sizeof(pkt), "cmd=1&uid=%s&topic=%s\r\n",
                   cfg->client_id, cfg->topic);
    if (len < 0 || (size_t)len >= sizeof(pkt)) {
        log_write("ERROR", "订阅报文过长，请缩短 topic");
        return -1;
    }

    if (send_all(sock, pkt, (size_t)len) != 0) {
        log_write("ERROR", "发送订阅报文失败：%s", strerror(errno));
        return -1;
    }

    mask_id(cfg->client_id, masked, sizeof(masked));
    log_write("INFO", "已发送订阅：uid=%s topic=%s", masked, cfg->topic);

    return 0;
}

/* 发送心跳包（巴法云要求 30s 内至少一次 ping）。 */
static int bemfa_ping(int sock)
{
    return send_all(sock, "ping\r\n", 6);
}

/* ------------------------------------------------------------------ */
/* 按行缓冲解析                                                       */
/* ------------------------------------------------------------------ */

/*
 * 从缓冲区切出一条完整报文（以 \n 结尾，可带 \r）。
 * 成功返回 1，并把剩余数据 memmove 到缓冲区头部；无完整行返回 0。
 */
static int extract_line(char *buf, size_t *blen, char *line, size_t linesz)
{
    size_t i;
    size_t linelen = 0;
    size_t skip = 0;
    int found = 0;

    if (buf == NULL || blen == NULL || line == NULL || linesz == 0)
        return 0;

    for (i = 0; i < *blen; i++) {
        if (buf[i] == '\n') {
            linelen = i;
            skip = i + 1;
            found = 1;
            break;
        }
    }

    if (!found)
        return 0;

    if (linelen > 0 && buf[linelen - 1] == '\r')
        linelen--;

    if (linelen >= linesz)
        linelen = linesz - 1;

    memcpy(line, buf, linelen);
    line[linelen] = '\0';

    memmove(buf, buf + skip, *blen - skip);
    *blen -= skip;
    buf[*blen] = '\0';

    return 1;
}

/* ------------------------------------------------------------------ */
/* 信号                                                               */
/* ------------------------------------------------------------------ */

/* SIGTERM / SIGINT：置退出标志，select 会因 EINTR 立即返回。 */
static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* 注册信号处理。 */
static void setup_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;             /* 不自动重启，保证 select 被打断 */

    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);    /* 对端关闭时不要被 SIGPIPE 杀掉 */
}

/* ------------------------------------------------------------------ */
/* 主循环                                                             */
/* ------------------------------------------------------------------ */

/* 守护进程主体：连接 - 订阅 - 收包 - 心跳 - 定期复查配置。 */
static int run_daemon(void)
{
    cc_config_t cfg;
    cc_config_t newcfg;
    char buf[RECV_BUF_SIZE];
    char line[LINE_MAX_LEN];
    char cur_uid[128];
    char cur_topic[128];
    char masked[32];
    size_t blen = 0;
    int sock = -1;
    time_t last_hb = 0;
    time_t last_check = 0;
    time_t now = 0;

    memset(buf, 0, sizeof(buf));
    memset(cur_uid, 0, sizeof(cur_uid));
    memset(cur_topic, 0, sizeof(cur_topic));
    memset(&cfg, 0, sizeof(cfg));

    g_running = 1;

    while (g_running) {
        if (cfg_load(&cfg) != 0) {
            log_write("WARN", "配置读取异常，将使用默认值重试");
        }

        if (strcmp(cfg.enabled, "1") != 0) {
            log_write("INFO", "服务未启用（enabled=0），30 秒后复查配置");
            sleep(30);
            continue;
        }

        if (cfg.client_id[0] == '\0' || cfg.topic[0] == '\0') {
            log_write("ERROR", "client_id 或 topic 为空，30 秒后重试");
            sleep(30);
            continue;
        }

        mask_id(cfg.client_id, masked, sizeof(masked));
        log_write("INFO", "正在连接 %s:%d（uid=%s topic=%s）",
                  BEMFA_HOST, BEMFA_PORT, masked, cfg.topic);

        sock = tcp_connect(BEMFA_HOST, BEMFA_PORT, CONN_TIMEOUT_SEC);
        if (sock < 0) {
            log_write("ERROR", "连接 %s:%d 失败，%d 秒后重试",
                      BEMFA_HOST, BEMFA_PORT, RETRY_DELAY_SEC);
            sleep(RETRY_DELAY_SEC);
            continue;
        }

        if (bemfa_subscribe(sock, &cfg) != 0) {
            close(sock);
            sock = -1;
            sleep(RETRY_DELAY_SEC);
            continue;
        }

        /* 记录当前订阅参数，供 60s 复查时对比 */
        cc_strlcpy(cur_uid, cfg.client_id, sizeof(cur_uid));
        cc_strlcpy(cur_topic, cfg.topic, sizeof(cur_topic));

        blen = 0;
        buf[0] = '\0';
        last_hb = time(NULL);
        last_check = last_hb;

        log_write("INFO", "订阅成功，进入监听循环");

        /* ---------------- 内层：单次连接的收发循环 ---------------- */
        while (g_running) {
            fd_set rfds;
            struct timeval tv;
            int n;

            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            tv.tv_sec = 1;
            tv.tv_usec = 0;

            n = select(sock + 1, &rfds, NULL, NULL, &tv);

            if (n < 0) {
                if (errno == EINTR)
                    continue;               /* 被信号打断，回到 while 判断 g_running */
                log_write("ERROR", "select 出错：%s", strerror(errno));
                break;
            }

            if (n > 0 && FD_ISSET(sock, &rfds)) {
                ssize_t got;

                /* 缓冲区快满仍无换行：丢弃并告警，避免死循环 */
                if (blen >= sizeof(buf) - 1) {
                    log_write("WARN", "接收缓冲区已满且无换行，丢弃 %u 字节异常数据",
                              (unsigned)blen);
                    blen = 0;
                    buf[0] = '\0';
                }

                got = recv(sock, buf + blen, sizeof(buf) - blen - 1, 0);
                if (got < 0) {
                    if (errno == EINTR)
                        continue;
                    log_write("ERROR", "recv 失败：%s", strerror(errno));
                    break;
                }
                if (got == 0) {
                    log_write("WARN", "服务器关闭了连接");
                    break;
                }

                blen += (size_t)got;
                buf[blen] = '\0';

                /* 逐条切分完整报文，处理剩余数据 */
                while (extract_line(buf, &blen, line, sizeof(line))) {
                    char *msg = cc_trim(line);
                    char safe[1024];
                    int kind;

                    if (msg[0] == '\0')
                        continue;

                    /*
                     * 日志一律打脱敏后的版本：报文里带 uid=<私钥>，
                     * 而 /var/log 会被 LuCI 页面读出来展示。
                     * 解析仍然用原文 msg。
                     */
                    /*
                     * cmd=0&res=N 是心跳（ping）的回执，不是业务报文。
                     * 实测巴法云对 ping 一律回 res=1（它并不认这种心跳写法），
                     * 但连接不受影响；不记它，免得日志被 20 秒一条刷屏，
                     * 也免得让人误以为收到了什么错误。
                     */
                    if (strncmp(msg, "cmd=0&res=", 10) != 0) {
                        redact_secrets(msg, safe, sizeof(safe));
                        log_write("INFO", "收到报文：%s", safe);
                    }

                    kind = parse_cmd(msg);
                    if (kind != 0)
                        handle_cmd(&cfg, kind, sock);
                }
            }

            now = time(NULL);

            /* 心跳 */
            if (now - last_hb >= HEARTBEAT_SEC) {
                if (bemfa_ping(sock) != 0) {
                    log_write("WARN", "心跳发送失败，准备重连");
                    break;
                }
                last_hb = now;
            }

            /* 每 60 秒复查配置：client_id / topic 变更需重连并重新订阅 */
            if (now - last_check >= RECHECK_SEC) {
                last_check = now;

                memset(&newcfg, 0, sizeof(newcfg));
                if (cfg_load(&newcfg) == 0) {
                    if (strcmp(newcfg.enabled, "1") != 0) {
                        log_write("INFO", "配置已关闭服务，断开连接并等待启用");
                        break;
                    }

                    if (strcmp(newcfg.client_id, cur_uid) != 0 ||
                        strcmp(newcfg.topic, cur_topic) != 0) {
                        mask_id(newcfg.client_id, masked, sizeof(masked));
                        log_write("INFO", "订阅参数变更，重新订阅：uid=%s topic=%s",
                                  masked, newcfg.topic);
                        break;      /* 跳出内层，外层会重连并重新订阅 */
                    }

                    /* 其余参数（host/mac/密码等）热更新即可，无需重连 */
                    memcpy(&cfg, &newcfg, sizeof(cfg));
                }
            }
        }
        /* ---------------- 内层结束 ---------------- */

        if (sock >= 0) {
            close(sock);
            sock = -1;
        }

        if (g_running) {
            log_write("INFO", "连接已断开，%d 秒后重连", RETRY_DELAY_SEC);
            sleep(RETRY_DELAY_SEC);
        }
    }

    if (sock >= 0)
        close(sock);

    log_write("INFO", "进程收到退出信号，已停止监听");
    return 0;
}

/* ------------------------------------------------------------------ */
/* 自守护化                                                           */
/* ------------------------------------------------------------------ */

/* double fork + setsid + 重定向 stdio，返回 0 表示已进入守护模式。 */
static int daemonize(void)
{
    pid_t pid;
    int fd;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(0);               /* 父进程退出 */

    if (setsid() < 0)
        return -1;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(0);               /* 第一个子进程退出，孙进程成为会话首进程且非组长 */

    umask(022);
    if (chdir("/") != 0) {
        /* 忽略：chdir 失败不影响主功能 */
    }

    fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO)
            close(fd);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* 测试模式                                                           */
/* ------------------------------------------------------------------ */

/*
 * bemfa_check_topic - 用 HTTP 推送接口验证 uid 与 topic 是否真的存在。
 *
 * 为什么必须做这一步（实测结论）：
 *   TCP 订阅接口对任何 uid/topic 都返回 "cmd=1&res=0"，
 *   连随便编的 UID 和主题都能过 —— 它只证明「服务器连得上」，
 *   证明不了主题填对了。HTTP 接口才会真正校验：
 *   主题不存在时返回 {"code":"40212","status":"topic error"}。
 *
 * 推送内容固定用 "test"：守护进程只认 on/off，不会误触发开关机。
 *
 * 返回：0 校验通过 / 1 校验失败 / 2 无法校验（没装 curl 等）
 */
static int bemfa_check_topic(const cc_config_t *cfg)
{
    char uid_enc[384];
    char topic_enc[384];
    char url[1024];
    char resp[512];
    char *argv[8];
    char curl_bin[256];
    long code = -1;
    int rc;

    if (cfg->client_id[0] == '\0' || cfg->topic[0] == '\0')
        return 1;

    url_encode(cfg->client_id, uid_enc, sizeof(uid_enc));
    url_encode(cfg->topic, topic_enc, sizeof(topic_enc));

    snprintf(url, sizeof(url), BEMFA_PUSH_URL, uid_enc, topic_enc, "test");

    if (find_bin("curl", curl_bin, sizeof(curl_bin)) != 0) {
        log_write("WARN", "未安装 curl，跳过主题有效性校验");
        return 2;
    }

    argv[0] = curl_bin;
    argv[1] = (char *)"-s";
    argv[2] = (char *)"--max-time";
    argv[3] = (char *)"8";
    argv[4] = url;
    argv[5] = NULL;

    rc = run_argv_capture(argv, NULL, NULL, 15, resp, sizeof(resp));

    if (rc == 127) {
        log_write("WARN", "未安装 curl，跳过主题有效性校验（只能验证服务器连通性）");
        return 2;
    }

    if (rc != 0) {
        log_write("WARN", "HTTP 校验请求失败（curl exit=%d），跳过主题校验", rc);
        return 2;
    }

    if (!json_get_code(resp, &code)) {
        log_write("WARN", "无法解析巴法云返回，跳过主题校验：%s", resp);
        return 2;
    }

    if (code != 0) {
        char safe[512];

        redact_secrets(resp, safe, sizeof(safe));
        log_write("ERROR", "巴法云拒绝了这个主题：code=%ld，返回：%s", code, safe);
        log_write("ERROR", "请核对：1) 主题名与控制台完全一致（大小写、空格都算）"
                          " 2) 该主题确实建在这个 UID 下");
        return 1;
    }

    log_write("OK", "uid 与 topic 校验通过");
    return 0;
}

/* -t cloud：先验证服务器连通，再验证 uid/topic 真实存在。 */
static int test_cloud(const cc_config_t *cfg)
{
    char buf[512];
    int sock;
    int n;
    fd_set rfds;
    struct timeval tv;

    if (cfg->client_id[0] == '\0') {
        log_write("ERROR", "client_id 未配置，无法测试");
        return 1;
    }

    sock = tcp_connect(BEMFA_HOST, BEMFA_PORT, CONN_TIMEOUT_SEC);
    if (sock < 0) {
        log_write("ERROR", "连接 %s:%d 失败", BEMFA_HOST, BEMFA_PORT);
        return 1;
    }

    if (bemfa_subscribe(sock, cfg) != 0) {
        close(sock);
        return 1;
    }

    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);
    tv.tv_sec = CONN_TIMEOUT_SEC;
    tv.tv_usec = 0;

    n = select(sock + 1, &rfds, NULL, NULL, &tv);
    if (n <= 0) {
        log_write("ERROR", "等待巴法云响应超时（%d 秒）", CONN_TIMEOUT_SEC);
        close(sock);
        return 1;
    }

    n = (int)recv(sock, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        log_write("ERROR", "巴法云未返回数据（可能 uid/topic 有误）");
        close(sock);
        return 1;
    }

    buf[n] = '\0';

    /*
     * 解析返回码：巴法云用 res=0 表示订阅成功，其它值表示被拒绝
     * （常见原因：uid 与 topic 不匹配、topic 没在控制台建过）。
     * 之前是「收到任何响应都算成功」，订阅被拒也会报成成功，误导性很大。
     */
    {
        char *r = strstr(buf, "res=");

        if (r != NULL) {
            long code = strtol(r + 4, NULL, 10);

            if (code != 0) {
                char safe[1024];

                redact_secrets(cc_trim(buf), safe, sizeof(safe));
                log_write("ERROR", "巴法云拒绝了订阅：res=%ld，响应：%s", code, safe);
                close(sock);
                return 1;
            }
        }
    }

    {
        /*
         * 测试模式（-t cloud）的日志直接走 stdout，会被 LuCI 的 run_capture
         * 捕获并完整渲染在管理员浏览器里 —— 响应里带 uid=<私钥>，
         * 必须脱敏，否则等于把账号密钥显示在网页上。
         */
        char safe[1024];
        redact_secrets(cc_trim(buf), safe, sizeof(safe));
        log_write("OK", "巴法云连接成功，响应：%s", safe);
    }

    close(sock);

    /*
     * 前面的 TCP 订阅只能证明服务器连得上；主题是否真实存在必须再走
     * 一次 HTTP 接口，否则填错主题也会显示「成功」。
     */
    return (bemfa_check_topic(cfg) == 1) ? 1 : 0;
}

/* 打印用法。 */
static void usage(const char *prog)
{
    fprintf(stdout,
            "用法：%s [选项]\n"
            "  （无参数）   前台运行，交由 procd 托管\n"
            "  -d           自守护化后后台运行\n"
            "  -t wol       执行一次 Wake-on-LAN 测试\n"
            "  -t ssh       执行一次 SSH 关机测试\n"
            "  -t cloud     测试巴法云连接（8 秒超时）\n"
            "  -h           显示本帮助\n",
            prog);
}

/* ------------------------------------------------------------------ */
/* 入口                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    cc_config_t cfg;
    int opt;
    int daemon_mode = 0;
    const char *test_mode = NULL;
    int rc = 0;

    /*
     * 必须最先执行：之后任何 log_write() 都会取 localtime_r() 生成时间戳，
     * 而 localtime_r() 依赖 TZ。放在 getopt 之前，保证包括 -h/-t 在内的
     * 所有路径在任何日志输出前都已完成时区初始化。
     */
    cc_init_timezone();

    while ((opt = getopt(argc, argv, "dt:h")) != -1) {
        switch (opt) {
        case 'd':
            daemon_mode = 1;
            break;
        case 't':
            test_mode = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* ---------------- 测试模式 ---------------- */
    if (test_mode != NULL) {
        g_verbose = 1;
        g_log_to_stdout = 1;
        g_log_to_file = 0;

        /*
         * 三种测试模式都先报一行时区：日志时间对不对，取决于这一行。
         * 「实际偏移」取自 tm_gmtoff，是 libc 真正在用的值，不是猜的：
         * TZ 没拿到时真正生效的时区由 libc 决定，只有读出来才说得准。
         * 取不到就如实写「未知」，绝不拿 UTC+00:00 冒充。
         */
        {
            int off = cc_tz_offset_sec();

            if (off == CC_TZ_OFFSET_UNKNOWN) {
                fprintf(stdout, "时区：TZ=%s（来源：%s）  实际偏移 未知\n",
                        (g_tz_value[0] != '\0') ? g_tz_value : "(未设置)",
                        g_tz_source);
            } else {
                int mag = (off < 0) ? -off : off;
                const char *sign = (off < 0) ? "-" : "+";

                fprintf(stdout,
                        "时区：TZ=%s（来源：%s）  实际偏移 UTC%s%02d:%02d\n",
                        (g_tz_value[0] != '\0') ? g_tz_value : "(未设置)",
                        g_tz_source, sign, mag / 3600, (mag % 3600) / 60);
            }
        }

        memset(&cfg, 0, sizeof(cfg));
        if (cfg_load(&cfg) != 0) {
            fprintf(stdout, "结果：失败 - 无法读取配置 %s\n", CFG_PATH);
            return 1;
        }

        if (strcmp(test_mode, "wol") == 0) {
            if (cfg.mac[0] == '\0') {
                fprintf(stdout, "结果：失败 - 未配置 MAC 地址\n");
                return 1;
            }
            rc = do_wol(&cfg, 1);
            fprintf(stdout, "结果：%s - 网卡 %s，目标 MAC %s\n",
                    rc == 0 ? "成功" : "失败", cfg.nic, cfg.mac);
        } else if (strcmp(test_mode, "ssh") == 0) {
            fprintf(stdout, "版本：cloud_control %s\n", CC_VERSION);

            if (cfg.host[0] == '\0' || cfg.user[0] == '\0') {
                fprintf(stdout, "结果：失败 - 未配置目标主机 IP 或用户名\n");
                return 1;
            }
            rc = do_ssh_shutdown(&cfg, 1);

            if (rc == 0 && g_ssh_inconclusive)
                fprintf(stdout, "结果：已下发但无法确认 - %s@%s:%s"
                                "（连接被中断，建议把关机命令改成 shutdown /s /t 10 /f）\n",
                        cfg.user, cfg.host, cfg.ssh_port);
            else
                fprintf(stdout, "结果：%s - %s@%s:%s\n",
                        rc == 0 ? "成功" : "失败", cfg.user, cfg.host, cfg.ssh_port);
        } else if (strcmp(test_mode, "cloud") == 0) {
            rc = test_cloud(&cfg);

            /*
             * 这里最容易误解：本测试是「连上即断开」的一次性验证，
             * 它成功只说明 uid/topic 能被订阅，不代表设备会上线。
             * 巴法云控制台的在线状态取决于有没有常驻长连接。
             */
            if (strcmp(cfg.enabled, "1") != 0) {
                fprintf(stdout,
                    "\n注意：配置里「启用服务」未勾选（enabled=0）。\n"
                    "      本测试只验证 uid/topic 能否订阅成功，测完立即断开；\n"
                    "      守护进程不会常驻连接，巴法云控制台会一直显示设备离线。\n"
                    "      勾选「启用服务」并点「保存并应用」后才会常驻。\n");
            }

            fprintf(stdout, "结果：%s - 巴法云 %s:%d\n",
                    rc == 0 ? "成功" : "失败", BEMFA_HOST, BEMFA_PORT);
        } else {
            fprintf(stdout, "结果：失败 - 未知测试类型 [%s]\n", test_mode);
            usage(argv[0]);
            return 1;
        }

        return rc == 0 ? 0 : 1;
    }

    /* ---------------- 守护模式 ---------------- */

    if (daemon_mode) {
        if (daemonize() != 0) {
            fprintf(stderr, "守护化失败：%s\n", strerror(errno));
            return 1;
        }
        /* 守护化后 stdio 已指向 /dev/null，仅写文件日志 */
    }

    g_log_to_file = 1;
    g_log_to_stdout = 0;

    setup_signals();
    log_open();

    log_write("INFO", "cloud_control %s 启动（pid=%d，模式=%s）",
              CC_VERSION, (int)getpid(), daemon_mode ? "后台" : "前台/procd");

    rc = run_daemon();

    log_write("INFO", "cloud_control 退出（rc=%d）", rc);

    if (g_logfp != NULL) {
        fclose(g_logfp);
        g_logfp = NULL;
    }

    return rc;
}

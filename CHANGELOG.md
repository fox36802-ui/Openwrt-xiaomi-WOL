# 变更日志

本项目的所有重要变更都记录在这里。
格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [2.1.8] - 2026-11-05

### 修复
- **日志时间与路由器本地时间不符（显示为 UTC，差 8 小时）。**
  `log_write()` 用 `localtime_r()` 生成时间戳，而它依赖 `TZ` 环境变量；
  守护进程由 procd 拉起，环境里没有 `TZ`（OpenWrt 只在 `/etc/profile` 里给登录 shell 设置），
  于是按 UTC 处理。现在程序启动时自行读取 `/etc/config/system` 的 `option timezone`
  并 `setenv("TZ")` + `tzset()`，一处修复同时覆盖守护进程和 `-t` 三种测试模式。
  环境变量已有 `TZ` 时不覆盖；配置缺失时行为与修复前一致。
- 时区值首尾空白自动裁剪。`option timezone 'CST-8 '` 这类写法原本会得到非法的
  `"CST-8 "`，libc 静默回落 UTC，修复等于失效且没有任何报错。

### 新增
- 三种测试模式（`-t wol|ssh|cloud`）首行输出当前时区与**实测** UTC 偏移，例如
  `时区：TZ=CST-8（来源：/etc/config/system）  实际偏移 UTC+08:00`。
  偏移取自 `tm_gmtoff`，`+12:45` / `+13` / `+14` 这类超出 ±12 小时的时区也能正确显示；
  无法计算时明确打印「实际偏移 未知」，不冒充 `UTC+00:00`。

## [2.1.7] - 2026-11-05

首个公开发布版本。

### 新增

**云端接入**
- TCP 长连接订阅巴法云主题，20 秒心跳、断线自动重连
- 每 60 秒复查配置：地址、密码、MAC 即时生效；Client ID / Topic 变更会自动断开重订阅
- 报文按字段解析（`msg=` / `message=`），避免订阅回执里的 `cmd=1` 被误判成开机指令
- 自身回显去重（10 秒）与同类指令限流（on 10s / off 30s），避免「关机后被自己唤醒」

**关机（SSH）**
- 执行成功**之后才**回报 `off`；失败回报 `on` 并记 ERROR 日志
- 自动识别 dropbear(`dbclient`) / OpenSSH 客户端，两者参数不兼容，分别下参数
- 显式 `-i` 指定私钥，不依赖 `$HOME` 展开；找不到私钥时直接报路径与生成命令
- `sshpass -e` + 仅子进程可见的环境变量传递口令，密码不出现在 `ps`
- 单次执行 15 秒超时看门狗，卡住不会拖死 Web 请求
- 可选「关机确认」：探测目标机 SSH 端口，连续 3 次无响应才回报 `off`（`verify_offline`，默认开启）

**开机（Wake-on-LAN）**
- 程序自己构造 UDP 魔术包，不依赖 `etherwake`
- `SO_BINDTODEVICE` 绑定出口网卡，多网口软路由不会把广播发到 WAN 侧

**运行保障**
- procd 托管，崩溃自动拉起；`SIGTERM` 优雅退出
- 日志轮转（`log_max_kb`，默认 256 KB）、`client_id` 脱敏、密码不落日志
- 4 KB 行缓冲，TCP 半包不会丢指令

**界面（LuCI）**
- 全中文 JS 视图，OpenWrt 19.07 ~ 25.12+ 通用，不需要 `luci-compat`
- 分组说明、IP / MAC / 端口校验、一键启停、日志面板（逐行转义）
- 三个连通性测试按钮：巴法云（含 uid / topic 有效性校验）、开机封包、SSH 关机

**工具链**
- `-t wol|ssh|cloud` 自检模式，`-d` 自守护化模式
- `install.sh` / `uninstall.sh` 一键装卸，不走包管理器，apk / opkg 固件通用
- `build.sh` 支持 x86_64 / aarch64 交叉编译，产物必须静态零依赖，否则构建失败
- GitHub Actions：语法与编译检查、双架构构建、打 tag 自动发布

### 说明
- 预编译产物为静态链接，无 `INTERP` / `NEEDED` 段，与固件 libc（musl / glibc）版本无关
- 配置文件含明文 SSH 密码时权限为 `600`；更推荐用密钥登录，配置里密码留空即可

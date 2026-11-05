# 变更日志

本项目的所有重要变更都记录在这里。
格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## 未发布

- 仓库化：补齐 README、变更日志、GitHub Actions 自动构建、Issue 模板

## [2.1.7] - 2026-11-05

### 修复
- **SSH 关机在守护进程里必然失败（`exited: Interrupted.`）**。根因在 dropbear 源码
  `cli-auth.c:355`：这句报错出自 `getpass()` 失败分支，说明公钥认证没成功、客户端退到了
  密码认证，而没有终端可输密码。再往上追，`cli-runopts.c:492` 里
  `loadidentityfile(expand_homedir_path("~/.ssh/id_dropbear"), 0)` 的 `warnfail` 是 0——
  `~` 展开依赖 `$HOME`，procd 环境的 HOME 不可靠时私钥会被**静默跳过**，一句提示都没有。
  现在改为显式 `-i <私钥路径>`，彻底不依赖 HOME。

### 新增
- 子进程强制把 `HOME` 设成 `getpwuid()` 解析出的真实 home 目录
- 找不到私钥时直接报错并给出路径与生成命令，不再静默降级
- 「测试 SSH 关机」输出中增加：版本号、客户端类型、实际执行的完整命令行、父/子进程 HOME

## [2.1.6] - 2026-11-05

### 修复
- 子进程 stdin 改为「父进程持有写端」的管道，避免 stdin 为 `/dev/null` 时 EOF 被提前
  推给远端通道

## [2.1.5] - 2026-11-05

### 文档
- 更正 README 中「加 `-t` 已修」的错误结论，改为正确的判断方法
  （`ssh -y -p 22 用户@IP "echo OK"` 不输密码返回 OK 才算密钥生效）

## [2.1.4] - 2026-11-05

### 变更
- **撤销 2.1.1/2.1.2 的 `-t` 与伪终端方案**，恢复为最朴素的调用：
  `ssh -y -p 22 用户@IP "命令"`。该方案基于误判，实际会让进程卡住

## [2.1.3] - 2026-11-05

### 修复
- SSH 执行增加 15 秒超时看门狗，超时后 `SIGTERM` → `SIGKILL`，返回 124
- 超时也能带出卡住之前的远端输出，不再把 LuCI 的 XHR 请求拖死

## [2.1.2] - 2026-11-05

### 变更（已于 2.1.4 回退）
- 为 ssh 子进程 `openpty()` 分配本地伪终端

## [2.1.1] - 2026-11-05

### 变更（已于 2.1.4 回退）
- dropbear 客户端加 `-t`。事后确认这是误判：真正原因是认证没通过，
  `-t` 只是让人能在终端里手动输密码

## [2.1.0] - 2026-11-05

### 新增
- **关机后探测确认**：下发 `off` 后轮询目标机 SSH 端口，连续 3 次连不上才回报 `off`，
  60 秒内仍在响应则回报 `on` 并记 ERROR
- 配置项 `verify_offline`（默认开启）与对应的界面开关
- 等待期间保持巴法云心跳，避免长连接被判超时

### 说明
- 用端口探测而不是 `ping`：Windows 防火墙默认丢弃 ICMP，用 `ping` 会把「开着」
  误判成「已关机」，正好是最危险的误判方向

## [2.0.7] - 2026-11-05

### 修复
- 连接被中断时不再谎报「成功」，改为「已下发但无法确认」
- 成功时也回显远端输出（此前只有失败才显示，等于黑盒）

## [2.0.6] - 2026-11-05

### 修复
- 成功路径也输出远端回显，`exit=0` 不再等于「什么都没说」

## [2.0.5] - 2026-11-05

### 修复
- **安装脚本补上重启服务的步骤**。只替换 `/usr/bin/cloud_control` 不会影响正在运行的
  旧进程，导致装了新版本却仍跑旧逻辑
- 心跳回执 `cmd=0&res=N` 不再写日志，日志里只留真正的业务报文

## [2.0.4] - 2026-11-05

### 修复
- **自动识别 `ssh` 客户端实现**。OpenWrt 的 `/usr/bin/ssh` 通常是 dropbear 的
  `dbclient`，它不认 OpenSSH 的 `-o` 参数（会被静默忽略后转去问交互问题）；
  OpenSSH 与 dropbear 分别下参数
- 修复 `SSHPASS` 环境变量未传给子进程（密码登录会失效）
- 失败时把远端原话写进日志

## [2.0.3] - 2026-11-05

### 修复
- 外部命令改用绝对路径定位（`find_bin`），避免 procd 环境下 PATH 不全导致 `exit=127`
- `127` 时明确报出「是哪个程序没找到」，不再只甩一个错误码

## [2.0.2] - 2026-11-05

### 修复
- **「测试巴法云连接」接入 HTTP 接口校验 uid/topic**。TCP 订阅接口对任何
  uid/topic 都返回 `cmd=1&res=0`，填错主题也会显示成功；改为再走一次 HTTP 推送接口，
  主题不存在时返回 `code=40212 topic error`

## [2.0.1] - 2026-11-05

### 修复
- 启动脚本按「节类型」而不是「节名」读取 `enabled`，避免配置文件被写成匿名节时服务永远起不来
- 界面状态栏在「未勾选启用」或「已启用但进程未运行」时直接红字说明

## [2.0.0] - 2026-11-04

首个重写版本。相对上游 `sudo-bai/luci-app-cloud-control`：

### 新增
- 全中文 LuCI 界面：分组说明、IP/MAC/端口校验、启停按钮、三个连通性测试按钮、日志面板
- 界面改为 JS 视图（`htdocs/luci-static/resources/view/`），19.07–25.12+ 通用，
  不依赖 `luci-compat`
- `-t wol|ssh|cloud` 三种自检模式与 `-d` 自守护化模式
- 日志轮转（`log_max_kb`）、`client_id` 脱敏
- procd 托管，崩溃自动拉起

### 修复
- 去掉 `system()` 拼接，全部改为 `fork + execvp`
- SSH 密码改用 `sshpass -e` + 环境变量，不再出现在 `ps` 命令行
- **SSH 成功后才回报 `off`**，失败回报 `on`（原版先回报再执行）
- 关机命令补 `/f`（原版缺，Windows 有未保存文档会拦住关机）
- 去掉多余的 `-A` / `-g`，补 `UserKnownHostsFile=/dev/null`
- 报文按字段解析，不再逐 token 匹配（原版会被订阅回执里的 `cmd=1` 误触发唤醒）
- 自身回显去重 + 同类指令限流，避免「关机后被自己唤醒」
- 日志面板逐行 `pcdata` 转义（原版直接拼 HTML，可被主题消息注入脚本）

[2.1.7]: https://github.com/YOUR_NAME/luci-app-cloud-control/compare/v2.1.6...v2.1.7
[2.1.6]: https://github.com/YOUR_NAME/luci-app-cloud-control/compare/v2.1.5...v2.1.6
[2.1.5]: https://github.com/YOUR_NAME/luci-app-cloud-control/compare/v2.1.4...v2.1.5
[2.1.4]: https://github.com/YOUR_NAME/luci-app-cloud-control/compare/v2.1.3...v2.1.4
[2.1.3]: https://github.com/YOUR_NAME/luci-app-cloud-control/compare/v2.1.2...v2.1.3
[2.1.2]: https://github.com/YOUR_NAME/luci-app-cloud-control/compare/v2.1.1...v2.1.2
[2.1.1]: https://github.com/YOUR_NAME/luci-app-cloud-control/compare/v2.1.0...v2.1.1
[2.1.0]: https://github.com/YOUR_NAME/luci-app-cloud-control/compare/v2.0.7...v2.1.0
[2.0.7]: https://github.com/YOUR_NAME/luci-app-cloud-control/compare/v2.0.6...v2.0.7
[2.0.6]: https://github.com/YOUR_NAME/luci-app-cloud-control/compare/v2.0.5...v2.0.6
[2.0.5]: https://github.com/YOUR_NAME/luci-app-cloud-control/compare/v2.0.4...v2.0.5
[2.0.4]: https://github.com/YOUR_NAME/luci-app-cloud-control/compare/v2.0.3...v2.0.4
[2.0.3]: https://github.com/YOUR_NAME/luci-app-cloud-control/compare/v2.0.2...v2.0.3
[2.0.2]: https://github.com/YOUR_NAME/luci-app-cloud-control/compare/v2.0.1...v2.0.2
[2.0.1]: https://github.com/YOUR_NAME/luci-app-cloud-control/compare/v2.0.0...v2.0.1
[2.0.0]: https://github.com/YOUR_NAME/luci-app-cloud-control/releases/tag/v2.0.0

========================================================================
 云端开关机插件（cloud-control） OpenWrt 安装包
 适配：OpenWrt 19.07 ~ 25.12+ 及各家用改版固件（x86_64 / aarch64）
========================================================================

一句话：路由器订阅巴法云主题，收到 "on" 发网络唤醒包开机，
收到 "off" 通过 SSH 关机；同时持续探测目标机真实状态，
网页和云端都能查询「电脑现在是开是关」。

开机和关机是两条独立链路：
  · 开机 = 局域网广播魔术包，目标机不用装任何软件
  · 关机 = SSH 登录目标机执行 shutdown，必须配 IP / 用户名 / 密码


------------------------------------------------------------------------
一、为什么不是 .ipk
------------------------------------------------------------------------

OpenWrt 官方从 25.12 起把包管理器从 opkg 换成了 apk，.ipk 已经装不上；
而 apk 包需要签名，离线自制容易被拒。
（注意：第三方魔改固件不一定跟着换，例如 BleachWrt 虽然基于 25.12 分支，
仍是 opkg。用 which apk 判断即可，本安装方式两种都适用。）

所以这个包不走包管理器，直接放文件 + 注册服务 —— 对 apk / opkg 固件
都适用，也不占包数据库。

另外，LuCI 25.12 已经不再提供 Lua CBI 兼容层，旧式的 Lua 配置页在你
的固件上根本不会显示。本包的界面是 JS 视图，19.07 到 25.12+ 通用。

二、为什么是静态二进制
------------------------------------------------------------------------

/usr/bin/cloud_control 是 96–108KB 的静态链接 ELF，不依赖 libuci、libubox、
musl/glibc 版本，也不依赖 etherwake（魔术包由程序自己发 UDP）。

也就是说：换固件版本、换 libc 都不会出现「装上了但跑不起来」。

运行期仍需要的外部命令（固件一般自带）：
  ssh       关机用（dropbear 自带）
  sshpass   仅当用「密码」登录时需要，没有就改用密钥
  curl      仅用于向巴法云回报状态，没有只影响状态同步


------------------------------------------------------------------------
三、安装
------------------------------------------------------------------------

1) 把整个压缩包传到路由器：

   scp luci-app-cloud-control-<版本>-<架构>.tar.gz root@192.168.1.1:/tmp/

2) 登录路由器解压并安装：

   ssh root@192.168.1.1
   cd /tmp
   tar -xzf luci-app-cloud-control-<版本>-<架构>.tar.gz
   cd luci-app-cloud-control
   sh install.sh

3) 装完打开 LuCI → 服务 → 云端开关机

   如果菜单里没有，执行：
     rm -f /tmp/luci-indexcache*
     /etc/init.d/rpcd restart
   然后按 Ctrl+F5 强制刷新浏览器。


------------------------------------------------------------------------
四、配置
------------------------------------------------------------------------

必填项：
  Client ID    巴法云控制台 →「私钥」里的 32 位 UID（不是账号密码）
  Topic        主题名，默认 PC001
  主机 IP      目标电脑的局域网地址，建议在路由器上绑定静态 DHCP
  SSH 用户名    Windows 下用 whoami 查实际用户名，别填微软账号邮箱
  SSH 密码      留空则改用 SSH 密钥登录
  目标机 MAC    有线网卡的 MAC，格式 AA:BB:CC:DD:EE:FF
  出口网卡      x86 软路由一般是 br-lan

可选：
  关机后确认离线   默认开启。下发关机命令后探测目标机 22 端口，
                   连续 3 次连不上才回报 off，避免「说关了其实还开着」
  周期同步状态     默认开启。每 30 秒探测一次目标机，状态变化且云端
                   显示不一致时，主动把真实状态推给巴法云
  状态探测间隔     默认 30 秒，可设 10 ~ 300
  自动重试次数     默认 0（不重试）。动作后状态未达预期时，
                   按这个次数重复执行该动作，可设 1 ~ 3

最后勾选「启用服务」→「保存并应用」。


------------------------------------------------------------------------
四之二、状态查询与重复操作（2.2.0 起）
------------------------------------------------------------------------

以前是单向发指令，现在每一步都先查真实状态：

  查状态 → 已经是目标状态？ → 是：只回报，不重复发动作
                            → 否：执行动作 → 回读状态 → 回报云端

状态来自对目标机 SSH 端口的探测（不用 ping，Windows 防火墙默认不回 ICMP）。
探测结果写在 /var/run/cloud_control.state，脚本可以直接读：

  cat /var/run/cloud_control.state

里面是 key=value：state（on/off/unknown）、updated、detail、last_cmd、
last_result、cloud（云端已上报状态）等。

LuCI 页面「服务 → 云端开关机 → 设备状态（实时）」里可以：
  · 看当前是「开机中 / 已关机 / 未知」以及最后探测时间、判定依据
  · 点「重新探测」只查不动
  · 点「立即开机 / 立即关机」按当前状态决定是否真发指令
  · 点「重试上次动作」重做状态文件里记录的最后一条指令
  · 点「同步状态到云端」把真实状态推给巴法云

云端查询：在巴法云控制台往本主题发 msg=status（也认 query / get / ?），
插件会回推当前真实状态，不会触发开关机。

重复下发相同指令不再是盲发：电脑已经关着时再说「关机」，
只会回报 off，不会再跑一次必然失败的 SSH。

命令行等价操作：
  cloud_control -t status    重新探测
  cloud_control -t on        立即开机（幂等）
  cloud_control -t off       立即关机（幂等）
  cloud_control -t retry     重试上次动作
  cloud_control -t push      同步状态到云端


------------------------------------------------------------------------
五、目标 Windows 电脑必须先做的事
------------------------------------------------------------------------

不装 OpenSSH 服务器，SSH 一定连不上 —— 这是最常见的失败原因。

用管理员身份打开 PowerShell，逐条执行：

  # 1. 安装 OpenSSH 服务器
  Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0

  # 2. 启动并设为开机自启
  Start-Service sshd
  Set-Service -Name sshd -StartupType Automatic

  # 3. 放行防火墙 22 端口
  New-NetFirewallRule -Name sshd -DisplayName 'OpenSSH Server' `
    -Enabled True -Direction Inbound -Protocol TCP -Action Allow -LocalPort 22

另外两件事，不做的话「开机」会失败：

  # 4. 关闭快速启动（否则关机后网卡断电，收不到唤醒包）
  powercfg /h off

  # 5. 网卡属性 → 电源管理 → 勾选「允许此设备唤醒计算机」
     并在「高级」里把「唤醒魔数包 / Wake on Magic Packet」设为启用
     BIOS 里也要打开 Wake on LAN / PCIE 设备唤醒


------------------------------------------------------------------------
六、验证（配完一定要按顺序点一遍）
------------------------------------------------------------------------

页面「连通性测试」里有三个按钮，从上到下依次点：

  1. 测试巴法云连接  → 应显示 uid / topic 响应
  2. 测试开机封包    → 显示「已向 xx 广播唤醒包（出口：br-lan）」
                       此时电脑应该开始启动
  3. 测试 SSH 关机   → 显示「SSH 关机指令已下发」
                       电脑应弹窗提示 10 秒后关机

哪个失败就点开弹窗看输出，里面是程序原话。


------------------------------------------------------------------------
七、常见问题
------------------------------------------------------------------------

菜单里看不到「云端开关机」
  → 清缓存：rm -f /tmp/luci-indexcache*  &&  /etc/init.d/rpcd restart
  → 确认 /usr/share/luci/menu.d/luci-app-cloud-control.json 存在

SSH 关机失败
  → 先看页面测试按钮的输出，里面是程序原话
  → 日志出现 exit=127：这不是密码错，是「要执行的程序没找到」
      · 配置里填了密码就必须有 sshpass：apk add sshpass
        （opkg 固件：opkg update && opkg install sshpass）
      · 或者改用密钥登录：把公钥装到目标机，配置里密码留空
  → exit=255：ssh 跑起来了但连不上或被拒
      · 目标机没装 OpenSSH 服务器 / sshd 没启动 / 防火墙没放行 22
      · 用户名不对（微软账户登录时用 whoami 查，不要填邮箱）
  → exit=1：sshpass 密码错误
  → 日志里出现 "Connection ... exited: Interrupted." 且命令没执行：
    这句话出自 dbclient 的 getpass() 失败分支，含义是「需要密码，但没有终端可输」。
    也就是说公钥认证没成功，客户端退到了密码认证。
    本版已改为显式 -i 指定私钥，并在找不到私钥时直接报路径，不再静默降级。
    自检：在路由器上直接跑
        ssh -y -p 22 用户@IP "echo OK"
    不用输密码就返回 OK，才说明密钥认证已生效。
  → 日志里出现 "Ignoring unknown configuration option" 或
    "Didn't validate host key"：
      · 原因：固件的 /usr/bin/ssh 是 dropbear 的 dbclient，
        它不认 OpenSSH 的 -o 参数，转而去问交互问题
      · 本版会自动识别 dropbear / OpenSSH，分别下参数
  → 其他：没装 OpenSSH 服务器 / 目标机 IP 变了

提示「未找到 sshpass」
  → apk add sshpass   或   opkg update && opkg install sshpass
  → 或者改用密钥：把公钥放到目标机，配置里密码留空

开机没反应
  → BIOS 的 Wake on LAN 没开
  → Windows 快速启动没关（关机后网卡彻底断电）
  → 目标机和「出口网卡」不在同一个二层网络（跨 VLAN / 跨路由不行）
  → MAC 填成了无线网卡的

页面显示「? 未知」或状态不刷新
  → 守护进程没启动，或配置里主机 IP 为空，状态文件还没生成
  → 点「重新探测」可手动生成一次（服务未启用时也能用）
  → 确认 /usr/share/rpcd/acl.d/luci-app-cloud-control.json
    里有 /var/run/cloud_control.state 的 read 权限

云端状态和电脑实际状态不一致
  → 确认「周期同步状态」已开启，点「同步状态到云端」立即推一次
  → 或在巴法云控制台发 msg=status 让插件回推真实状态
  → 主动同步有 60 秒节流，短时间内连续变化不会每条都推

巴法云控制台里设备一直显示「离线」
  → 最常见：主题名和控制台里的对不上（大小写、空格、全半角都算）
  → 「测试巴法云连接」现在会用 HTTP 接口校验，填错会直接报
    code=40212 topic error，按提示改主题即可
  → 其次：配置里「启用服务」没勾选，守护进程不会常驻连接
  → 再其次：服务没启动，看页面状态和日志

小爱说「已关闭」但电脑还开着
  → 本版已修：SSH 执行成功才回报 off，失败会回报 on 并记日志

日志在哪
  → 页面底部「运行日志」，或 tail -f /var/log/cloud_control.log
  → client_id 只显示前 4 位，密码不写日志


------------------------------------------------------------------------
八、卸载
------------------------------------------------------------------------

  cd /tmp/luci-app-cloud-control
  sh uninstall.sh

配置会备份到 /etc/config/cloud_control.bak（含明文密码，注意保管）。


------------------------------------------------------------------------
九、文件清单
------------------------------------------------------------------------

files/usr/bin/cloud_control                              静态二进制 96–108KB
files/ARCH                                               该包的架构标记（装错架构会被拦下）
（运行时生成，不在包内）  /var/run/cloud_control.state    状态快照
files/etc/config/cloud_control                           默认配置
files/etc/init.d/cloud_control                           procd 启动脚本
files/www/luci-static/resources/view/cloud_control/settings.js   JS 界面
files/usr/share/luci/menu.d/luci-app-cloud-control.json  菜单项
files/usr/share/rpcd/acl.d/luci-app-cloud-control.json   权限白名单
install.sh / uninstall.sh                                安装与卸载

源码在同级目录的 cloud-control/src/cloud_control.c，
Makefile 也一并提供，需要的话可以用你的 SDK 自行编译。

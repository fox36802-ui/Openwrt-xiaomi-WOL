#!/bin/sh
#
# install.sh —— 云端开关机插件 一键安装
#
# 用法（在路由器上执行）：
#   sh install.sh
#
# 设计说明：
#   25.12 起 OpenWrt 的包管理器从 opkg 换成了 apk，.ipk 已经装不上；
#   apk 包还需要签名，离线自制容易因为签名/格式问题被拒。
#   所以这里不走包管理器，直接把文件放到对应位置并注册服务 ——
#   对 apk 和 opkg 固件都适用，也不占用包数据库。
#
# License: MIT

set -u

SRC_DIR=$(cd "$(dirname "$0")" && pwd)
FILES="$SRC_DIR/files"

say()  { printf '\033[32m[+] \033[0m%s\n' "$1"; }
warn() { printf '\033[33m[!] \033[0m%s\n' "$1"; }
die()  { printf '\033[31m[x] \033[0m%s\n' "$1" >&2; exit 1; }

# ---------------------------------------------------------------- 前置检查

[ "$(id -u)" = "0" ] || die "请用 root 运行（路由器上默认就是 root）"

[ -d "$FILES" ] || die "找不到 files 目录，请把整个压缩包解压后在本目录执行"

# 架构检查：安装包里带一个 ARCH 标记文件，写着这份二进制是给哪个架构编的。
# 直接 uname 对比，避免「装完才发现二进制跑不起来」。
ARCH=$(uname -m)
case "$ARCH" in
	arm64|armv8l) ARCH=aarch64 ;;
esac

EXPECT=$(cat "$FILES/ARCH" 2>/dev/null || echo "")

if [ -n "$EXPECT" ] && [ "$EXPECT" != "$ARCH" ]; then
	die "本安装包的二进制是 $EXPECT 架构，当前设备是 $ARCH，请下载对应架构的包"
fi

say "架构检查通过：$ARCH${EXPECT:+（安装包：$EXPECT）}"

# LuCI 版本检查：25.12 的菜单在 /usr/share/luci/menu.d，老版本没有这个目录
if [ ! -d /usr/share/luci/menu.d ]; then
	warn "这台设备的 LuCI 没有 /usr/share/luci/menu.d，可能是 19.07 以前的老版本。"
	warn "本界面用的是 JS 视图，老版本可能不显示菜单元件。仍可继续安装，"
	warn "配置项可用 uci 命令行修改：uci set cloud_control.main.host=..."
fi

# ---------------------------------------------------------------- 安装文件

say "安装程序与配置文件..."

mkdir -p /usr/bin /etc/config /etc/init.d \
         /usr/share/luci/menu.d /usr/share/rpcd/acl.d \
         /www/luci-static/resources/view/cloud_control

cp -f "$FILES/usr/bin/cloud_control"                 /usr/bin/cloud_control || die "复制二进制失败"
chmod 755 /usr/bin/cloud_control                                || die "设置权限失败"

# 配置文件：已存在就保留，避免覆盖用户已有设置
if [ -f /etc/config/cloud_control ]; then
	warn "检测到已有 /etc/config/cloud_control，保留原配置不覆盖"
else
	cp -f "$FILES/etc/config/cloud_control" /etc/config/cloud_control
	chmod 600 /etc/config/cloud_control
	say "已写入默认配置（含明文 SSH 密码，权限设为 600）"
fi

cp -f "$FILES/etc/init.d/cloud_control" /etc/init.d/cloud_control || die "复制启动脚本失败"
chmod 755 /etc/init.d/cloud_control

cp -f "$FILES/www/luci-static/resources/view/cloud_control/settings.js" \
      /www/luci-static/resources/view/cloud_control/settings.js || die "复制界面文件失败"

cp -f "$FILES/usr/share/luci/menu.d/luci-app-cloud-control.json" \
      /usr/share/luci/menu.d/ 2>/dev/null || warn "菜单文件复制失败"
cp -f "$FILES/usr/share/rpcd/acl.d/luci-app-cloud-control.json" \
      /usr/share/rpcd/acl.d/ 2>/dev/null || warn "权限文件复制失败"

say "文件安装完成"

# ---------------------------------------------------------------- 注册服务

say "注册开机自启..."
/etc/init.d/cloud_control enable 2>/dev/null || warn "enable 失败，可稍后手动执行"

# 覆盖 /usr/bin/cloud_control 只换了磁盘上的文件，正在跑的仍是旧进程，
# 必须重启才会用上新版本。这一步漏了，升级后所有修复都不会生效。
if [ -x /etc/init.d/cloud_control ]; then
	say "重启服务以加载新版本..."
	if /etc/init.d/cloud_control restart >/dev/null 2>&1; then
		sleep 1
		if pidof cloud_control >/dev/null 2>&1; then
			say "服务已重启并正在运行"
		else
			warn "服务未运行（若「启用服务」未勾选，属于正常）"
		fi
	else
		warn "重启失败，请手动执行 /etc/init.d/cloud_control restart"
	fi
fi

say "刷新 rpcd 权限与 LuCI 缓存..."
rm -f /tmp/luci-indexcache /tmp/luci-indexcache.* 2>/dev/null
/etc/init.d/rpcd restart 2>/dev/null || warn "rpcd 重启失败，请手动执行 /etc/init.d/rpcd restart"

# ---------------------------------------------------------------- 依赖提示

echo
say "检查运行期依赖..."
MISSING=""
for c in ssh curl; do
	if ! command -v "$c" >/dev/null 2>&1; then
		MISSING="$MISSING $c"
	fi
done
# sshpass 只在用密码登录时需要
if ! command -v sshpass >/dev/null 2>&1; then
	warn "未找到 sshpass：如果目标机用「密码」登录，SSH 关机会失败。"
	if command -v apk >/dev/null 2>&1; then
		warn "  安装方法：apk add sshpass"
	elif command -v opkg >/dev/null 2>&1; then
		warn "  安装方法：opkg update && opkg install sshpass"
	fi
	warn "  或者改用 SSH 密钥登录（配置里密码留空即可）。"
fi

if [ -n "$MISSING" ]; then
	warn "缺少命令:$MISSING —— 状态上报/关机功能会受影响"
fi

# ---------------------------------------------------------------- 完成

echo
say "安装完成"
echo
echo "下一步："
echo "  1. 打开 LuCI → 服务 → 云端开关机"
echo "  2. 填 Client ID / Topic / 目标机 IP / 用户名 / 密码 / MAC / 网卡"
echo "  3. 勾选「启用服务」，点「保存并应用」"
echo "  4. 用页面里的三个「测试」按钮逐个验证"
echo
echo "命令行查看日志：  tail -f /var/log/cloud_control.log"
echo "手动启停：        /etc/init.d/cloud_control start|stop|restart"
echo

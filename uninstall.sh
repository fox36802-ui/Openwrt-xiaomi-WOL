#!/bin/sh
#
# uninstall.sh —— 云端开关机插件 卸载
#
# 用法（在路由器上执行）：
#   sh uninstall.sh
#
# License: MIT

set -u

say()  { printf '\033[32m[+] \033[0m%s\n' "$1"; }
warn() { printf '\033[33m[!] \033[0m%s\n' "$1"; }
die()  { printf '\033[31m[x] \033[0m%s\n' "$1" >&2; exit 1; }

[ "$(id -u)" = "0" ] || die "请用 root 运行"

say "停止并取消开机自启..."
/etc/init.d/cloud_control stop    2>/dev/null
/etc/init.d/cloud_control disable 2>/dev/null

say "删除程序与界面文件..."
rm -f /usr/bin/cloud_control
rm -f /etc/init.d/cloud_control
rm -f /www/luci-static/resources/view/cloud_control/settings.js
rmdir /www/luci-static/resources/view/cloud_control 2>/dev/null
rm -f /usr/share/luci/menu.d/luci-app-cloud-control.json
rm -f /usr/share/rpcd/acl.d/luci-app-cloud-control.json

# 配置里有 SSH 密码，删除前先留一份备份
if [ -f /etc/config/cloud_control ]; then
	mv -f /etc/config/cloud_control /etc/config/cloud_control.bak
	say "配置已备份到 /etc/config/cloud_control.bak（含明文密码，注意保管）"
fi

say "清理缓存与日志..."
rm -f /var/log/cloud_control.log /var/log/cloud_control.log.1
rm -f /tmp/luci-indexcache /tmp/luci-indexcache.* 2>/dev/null
/etc/init.d/rpcd restart 2>/dev/null

echo
say "卸载完成"

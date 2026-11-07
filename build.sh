#!/usr/bin/env bash
#
# build.sh —— 编译静态二进制并打成可直装的 tar.gz
#
# 用法：
#   ./build.sh                                   # 用 prebuilt/<arch> 里已有的二进制直接打包
#   ./build.sh --toolchain <路径>                 # 用 musl 交叉工具链重新编译后再打包
#   ./build.sh --arch aarch64 --toolchain <路径>  # 指定目标架构（默认 x86_64）
#   ./build.sh --cc gcc                          # 直接用本机编译器（在 Alpine 等 musl 环境里用）
#
# 工具链获取（约 110MB，解压后即可用）：
#   curl -LO https://musl.cc/x86_64-linux-musl-cross.tgz
#   tar -xzf x86_64-linux-musl-cross.tgz
#   ./build.sh --toolchain $PWD/x86_64-linux-musl-cross
#
# 为什么用 musl 交叉工具链静态编译，而不是 OpenWrt SDK：
#   本程序不依赖 libuci / libubox，静态链接后与固件的 libc 版本完全无关，
#   在 24.10 / 25.12 / 各家用魔改固件上都能直接跑，省掉几百 MB 的 SDK。
#   （musl.cc 经常连不上，CI 里改用 Alpine 容器 + --cc gcc 原生编译）
#
# License: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOOLCHAIN=""
CC_OVERRIDE=""
ARCH="x86_64"
VERSION="2.2.5"

while [ $# -gt 0 ]; do
	case "$1" in
		--toolchain) TOOLCHAIN="${2:-}"; shift 2 ;;
		--cc)        CC_OVERRIDE="${2:-}"; shift 2 ;;
		--arch)      ARCH="${2:-}"; shift 2 ;;
		-h|--help)
			sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
			exit 0 ;;
		*) echo "未知参数：$1（-h 查看用法）"; exit 1 ;;
	esac
done

info() { printf '\033[32m[+]\033[0m %s\n' "$*"; }
die()  { printf '\033[31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

case "$ARCH" in
	x86_64|aarch64) ;;
	*) die "暂不支持的架构：$ARCH（可选 x86_64 / aarch64）" ;;
esac

# ---------------------------------------------------------------- 编译

BIN="$ROOT/prebuilt/$ARCH/cloud_control"

if [ -n "$CC_OVERRIDE" ]; then
	# 直接用指定的编译器（例如在 Alpine 容器里就是原生 gcc，
	# 目标本身就是 musl，静态链接后同样零依赖）
	CC="$CC_OVERRIDE"
	STRIP_CMD="strip"
	READELF_CMD="readelf"
	DO_COMPILE=1
elif [ -n "$TOOLCHAIN" ]; then
	# 允许两种布局：<toolchain>/ 或 <toolchain>/<arch>-linux-musl-cross/
	CC=""
	PREFIX=""
	for d in "$TOOLCHAIN" "$TOOLCHAIN/$ARCH-linux-musl-cross"; do
		if [ -x "$d/bin/$ARCH-linux-musl-gcc" ]; then
			CC="$d/bin/$ARCH-linux-musl-gcc"
			PREFIX="$d/bin/$ARCH-linux-musl-"
			break
		fi
	done
	[ -n "$CC" ] || die "在 $TOOLCHAIN 下找不到 $ARCH-linux-musl-gcc"
	STRIP_CMD="${PREFIX}strip"
	READELF_CMD="${PREFIX}readelf"
	DO_COMPILE=1
else
	DO_COMPILE=0
fi

if [ "$DO_COMPILE" = "1" ]; then
	info "编译 cloud_control.c（$ARCH，静态、-Os，编译器 $CC）"
	mkdir -p "$(dirname "$BIN")"
	"$CC" -static -no-pie -Os -Wall -Wextra \
		-DCC_VERSION=\"$VERSION\" \
		-o "$BIN" "$ROOT/cloud-control/src/cloud_control.c"

	if command -v "$STRIP_CMD" >/dev/null 2>&1; then "$STRIP_CMD" "$BIN"; fi

	info "校验产物"
	"$READELF_CMD" -h "$BIN" | grep -q 'EXEC' || die "产物不是静态可执行文件"
	if "$READELF_CMD" -d "$BIN" 2>/dev/null | grep -q NEEDED; then
		die "产物仍有动态库依赖，请检查编译参数"
	fi
else
	[ -x "$BIN" ] || die "prebuilt/$ARCH 里没有二进制，请用 --toolchain 或 --cc 指定编译器"
	info "使用已有二进制：$BIN"
fi

info "大小：$(du -h "$BIN" | cut -f1)"

# ---------------------------------------------------------------- 组装

info "组装发布目录 dist/"

DIST="$ROOT/dist"
rm -rf "$DIST"

mkdir -p "$DIST/files/usr/bin" \
         "$DIST/files/etc/config" \
         "$DIST/files/etc/init.d" \
         "$DIST/files/www/luci-static/resources/view/cloud_control" \
         "$DIST/files/usr/share/luci/menu.d" \
         "$DIST/files/usr/share/rpcd/acl.d"

cp "$BIN"                                                              "$DIST/files/usr/bin/cloud_control"
cp "$ROOT/luci-app-cloud-control/root/etc/config/cloud_control"        "$DIST/files/etc/config/cloud_control"
cp "$ROOT/luci-app-cloud-control/root/etc/init.d/cloud_control"        "$DIST/files/etc/init.d/cloud_control"
cp "$ROOT/luci-app-cloud-control/htdocs/luci-static/resources/view/cloud_control/settings.js" \
                                                                        "$DIST/files/www/luci-static/resources/view/cloud_control/settings.js"
cp "$ROOT/luci-app-cloud-control/root/usr/share/luci/menu.d/luci-app-cloud-control.json" \
                                                                        "$DIST/files/usr/share/luci/menu.d/"
cp "$ROOT/luci-app-cloud-control/root/usr/share/rpcd/acl.d/luci-app-cloud-control.json" \
                                                                        "$DIST/files/usr/share/rpcd/acl.d/"
cp "$ROOT/install.sh" "$ROOT/uninstall.sh"                             "$DIST/"

# 架构标记：install.sh 靠它判断这个包能不能装到当前设备
printf '%s\n' "$ARCH" > "$DIST/files/ARCH"

if [ -f "$ROOT/README-dist.txt" ]; then cp "$ROOT/README-dist.txt" "$DIST/README.txt"; fi
if [ -f "$ROOT/LICENSE" ];        then cp "$ROOT/LICENSE"        "$DIST/LICENSE"; fi

chmod 755 "$DIST/install.sh" "$DIST/uninstall.sh" \
          "$DIST/files/usr/bin/cloud_control" "$DIST/files/etc/init.d/cloud_control"
chmod 644 "$DIST/files/etc/config/cloud_control" \
          "$DIST/files/ARCH" \
          "$DIST/files/www/luci-static/resources/view/cloud_control/settings.js"

# ---------------------------------------------------------------- 打包

TGZ="$ROOT/luci-app-cloud-control-${VERSION}-${ARCH}.tar.gz"

info "打包 $TGZ"
rm -rf /tmp/cc-pkg && mkdir -p /tmp/cc-pkg
cp -r "$DIST" /tmp/cc-pkg/luci-app-cloud-control

# 在 ext4 上重新设权限：Windows 挂载盘上 chmod 不生效，权限必须在打包前落定
P=/tmp/cc-pkg/luci-app-cloud-control
chmod 755 "$P/install.sh" "$P/uninstall.sh" \
          "$P/files/usr/bin/cloud_control" "$P/files/etc/init.d/cloud_control"
chmod 644 "$P/files/etc/config/cloud_control" \
          "$P/files/ARCH" \
          "$P/files/www/luci-static/resources/view/cloud_control/settings.js" \
          "$P/README.txt" 2>/dev/null || true
chmod 644 "$P/LICENSE" 2>/dev/null || true
find "$P" -type d -exec chmod 755 {} +
find "$P" -name '*.json' -exec chmod 644 {} +

( cd /tmp/cc-pkg && tar -czf "$TGZ" luci-app-cloud-control )

info "完成：$(du -h "$TGZ" | cut -f1)  $TGZ"
echo
echo "传到路由器后："
echo "  tar -xzf $(basename "$TGZ") && cd luci-app-cloud-control && sh install.sh"

#
# luci-app-cloud-control —— 云端开关机 LuCI 配置界面
#
# 界面是 JS 视图（htdocs/luci-static/resources/view/...），
# 从 LuCI 19.07 一直支持到 25.12+，不需要 luci-compat。
#
# License: MIT
#

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-cloud-control
PKG_VERSION:=2.2.3
PKG_RELEASE:=1

PKG_LICENSE:=MIT
PKG_MAINTAINER:=fox36802-ui <fox36802-ui@users.noreply.github.com>

LUCI_TITLE:=LuCI support for cloud-control (Bemfa power on/off)
LUCI_DESCRIPTION:=Web UI to configure the cloud-control daemon.
LUCI_DEPENDS:=+cloud-control +luci-base
LUCI_PKGARCH:=all

# 兼容两种目录布局：
#   1) 作为 luci feed 的一部分：applications/luci-app-xxx/  -> ../../luci.mk
#   2) 直接丢进 SDK 的 package/ 目录                        -> $(TOPDIR)/feeds/luci/luci.mk
LUCI_MK:=$(firstword $(wildcard ../../luci.mk $(TOPDIR)/feeds/luci/luci.mk))

ifeq ($(strip $(LUCI_MK)),)
  $(error 找不到 luci.mk，请先在 SDK 中执行 ./scripts/feeds update -a && ./scripts/feeds install -a)
endif

include $(LUCI_MK)

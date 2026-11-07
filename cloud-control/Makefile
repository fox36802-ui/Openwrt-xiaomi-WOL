#
# cloud-control —— 巴法云云端开关机守护进程（OpenWrt 包定义）
#
# 本程序不链接 libuci / libubox，也不依赖 etherwake：
#   配置由程序自行解析，Wake-on-LAN 魔术包由程序直接发送。
#   因此既可以静态链接成零依赖二进制（推荐，见 install.sh），
#   也可以按常规方式在 SDK 里编成动态包。
#
# License: MIT
#

include $(TOPDIR)/rules.mk

PKG_NAME:=cloud-control
PKG_VERSION:=2.2.3
PKG_RELEASE:=1

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

PKG_LICENSE:=MIT
PKG_LICENSE_FILES:=LICENSE
PKG_MAINTAINER:=fox36802-ui <fox36802-ui@users.noreply.github.com>

include $(INCLUDE_DIR)/package.mk

define Package/cloud-control
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Cloud control daemon for Bemfa (WoL power-on / SSH shutdown)
  URL:=https://github.com/fox36802-ui/Openwrt-xiaomi-WOL
  DEPENDS:=+curl +sshpass
endef

define Package/cloud-control/description
  Subscribe a Bemfa (bemfa.com) topic over a TCP long connection and control
  a LAN PC: "on" broadcasts a Wake-on-LAN magic packet (sent by this program
  itself, no etherwake needed), "off" shuts the PC down over SSH and only
  reports "off" to the cloud after the SSH command actually succeeded.

  No libuci dependency: the UCI config file is parsed directly, so the binary
  can be built statically and run on any OpenWrt build regardless of libc ABI.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
	$(CP) ../LICENSE $(PKG_BUILD_DIR)/
endef

define Build/Configure
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -Wall -Wextra -Os \
		-o $(PKG_BUILD_DIR)/cloud_control $(PKG_BUILD_DIR)/cloud_control.c \
		$(TARGET_LDFLAGS)
endef

define Package/cloud-control/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/cloud_control $(1)/usr/bin/cloud_control
endef

$(eval $(call BuildPackage,cloud-control))

-- -*- coding: utf-8 -*-
-- 云端开关机 —— LuCI 路由入口
--
-- License: MIT

module("luci.controller.cloud_control", package.seeall)

local fs = require "nixio.fs"
local i18n = require "luci.i18n"

function index()
	if not fs.access("/etc/config/cloud_control") then
		return
	end

	entry({"admin", "services", "cloud_control"},
		cbi("cloud_control_setting"),
		i18n.translate("云端开关机"), 60).dependent = true
end

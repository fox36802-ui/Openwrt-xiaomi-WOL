-- -*- coding: utf-8 -*-
--
-- 云端开关机 —— LuCI 配置界面（Lua CBI）
--
-- 兼容性说明：
--   * 官方 OpenWrt 21.02+ 需要安装 luci-compat 才能运行本 Lua CBI 模型；
--   * Lean 版 LEDE 自带 Lua CBI，无需额外组件。
--
-- 安全说明：
--   所有来自日志文件 / 命令输出的文本在写入 HTML 之前都会经过
--   luci.util.pcdata() 转义，防止存储型 XSS（原版就是把日志原文拼进
--   HTML，任何能给该 bemfa 主题发消息的人都能在管理页注入脚本）。
--
-- 交互说明：
--   动作按钮使用 cbi-button 样式的链接 + cc_action 查询参数实现，
--   在模型文件末尾统一处理。这样不依赖 SimpleSection 内部按钮的分发
--   细节，在 Lean LEDE 与官方 OpenWrt + luci-compat 上都稳定可用。
--
-- License: MIT

local fs   = require "nixio.fs"
local sys  = require "luci.sys"
local util = require "luci.util"
local i18n = require "luci.i18n"
local http = require "luci.http"
local disp = require "luci.dispatcher"

local _ = i18n.translate

-- 当前会话 token：启停/重启/测试/清日志这些写操作都必须带上它。
--
-- 注意字段来源（LuCI dispatcher）：
--   context.authsession 是「会话 ID 字符串」，不是表
--   context.authtoken   才是 CSRF token（模板里 <%=token%> 用的就是它）
-- 早期某些分支把带 token 的表放在 authsession 里，故保留一层兜底。
local SESSION_TOKEN = ""
do
	local ctx = disp.context
	if ctx then
		if type(ctx.authtoken) == "string" then
			SESSION_TOKEN = ctx.authtoken
		elseif type(ctx.authsession) == "table" and ctx.authsession.token then
			SESSION_TOKEN = tostring(ctx.authsession.token)
		end
	end
end

local DAEMON  = "/usr/bin/cloud_control"
local INITD   = "/etc/init.d/cloud_control"
local LOGFILE = "/var/log/cloud_control.log"
local LOGROT  = "/var/log/cloud_control.log.1"
local PACKAGE = "cloud_control"
local SECTION = "main"

local ucicursor = require "luci.model.uci".cursor()

-- ------------------------------------------------------------------
-- 通用工具
-- ------------------------------------------------------------------

-- 跳回本页面（用于动作执行后刷新状态）
local function redirect_self()
	http.redirect(disp.build_url("admin", "services", "cloud_control"))
end

-- 执行命令并丢弃输出，返回退出码
local function run_quiet(cmd)
	return sys.call(cmd .. " >/dev/null 2>&1")
end

-- 判断守护进程是否在跑。
-- 优先用 pidof：部分精简固件的 BusyBox 没编进 pgrep -f。
local function service_running()
	if run_quiet("command -v pidof") == 0 then
		return run_quiet("pidof cloud_control") == 0
	end
	return run_quiet("pgrep -f " .. DAEMON) == 0
end

-- 执行命令并同时拿到「退出码 + stdout/stderr 文本」
-- 注意：cmd 全部由本文件内的常量拼接而成，不含任何用户输入。
local function run_capture(cmd)
	local out = nil
	local rc = 1

	if io and io.popen then
		local fp = io.popen(cmd .. " 2>&1; echo __RC__$?")
		if fp then
			out = fp:read("*a") or ""
			fp:close()

			local s = out:match("__RC__(%d+)%s*$")
			if s then
				rc = tonumber(s) or 1
				out = out:gsub("__RC__%d+%s*$", "")
			end
		end
	end

	if out == nil then
		-- 兜底：popen 不可用时只取输出，无法判断退出码
		out = sys.exec(cmd .. " 2>&1") or ""
		rc = (out ~= "") and 0 or 1
	end

	return rc, out
end

-- 读取文件末尾 n 行，返回表；文件不存在返回 nil
local function read_tail(path, n)
	local fp = io.open(path, "r")
	if not fp then
		return nil
	end

	local lines = {}
	local total = 0

	for line in fp:lines() do
		total = total + 1
		lines[total] = line

		-- 控制内存占用：超过 3 倍目标行数就裁剪一次
		if total > n * 3 then
			local kept = {}
			local start = total - n + 1
			if start < 1 then start = 1 end
			for i = start, total do
				kept[#kept + 1] = lines[i]
			end
			lines = kept
			total = #kept
		end
	end

	fp:close()

	local result = {}
	local start = total - n + 1
	if start < 1 then start = 1 end
	for i = start, total do
		result[#result + 1] = lines[i]
	end

	return result
end

-- 把测试命令的输出转成安全的 HTML 片段
local function build_test_html(rc, output)
	local head
	if rc == 0 then
		head = '<span style="color:#00a65a;font-weight:bold;">&#10004; ' ..
			util.pcdata(_("成功")) .. '</span>'
	else
		head = '<span style="color:#dd4b39;font-weight:bold;">&#10008; ' ..
			util.pcdata(_("失败")) .. '</span>'
	end

	local text = (output or ""):gsub("%s+$", "")
	local body = util.pcdata(text)
	if body == "" then
		body = util.pcdata(_("（无输出）"))
	end

	return head ..
		'<pre style="max-height:200px;overflow:auto;margin:6px 0 0 0;' ..
		'font-size:12px;line-height:1.5;white-space:pre-wrap;">' .. body .. '</pre>'
end

-- 生成运行日志的 HTML 片段（必须逐行 pcdata 转义）
local function build_log_html()
	local lines = read_tail(LOGFILE, 60)

	if lines == nil then
		return '<em>' ..
			util.pcdata(_("暂无日志：服务可能尚未启动，或日志文件尚未创建。")) ..
			'</em>'
	end

	if #lines == 0 then
		return '<em>' .. util.pcdata(_("日志文件为空。")) .. '</em>'
	end

	local safe = {}
	for i = 1, #lines do
		-- 逐行转义：日志里出现 <script> 也只会被当成文本显示
		safe[#safe + 1] = util.pcdata(lines[i])
	end

	return '<pre style="max-height:320px;overflow:auto;margin:0;' ..
		'font-size:12px;line-height:1.5;white-space:pre-wrap;">' ..
		table.concat(safe, "\n") .. '</pre>'
end

-- 生成分组标题 HTML（SimpleSection 自身不渲染标题，故显式输出）
local function header_html(title, desc)
	local h = '<h3 style="margin:18px 0 6px 0;padding-bottom:6px;' ..
		'border-bottom:1px solid rgba(128,128,128,.35);font-size:15px;' ..
		'font-weight:bold;">' .. util.pcdata(title) .. '</h3>'

	if desc then
		h = h .. '<p class="cbi-section-descr" style="margin:0 0 8px 0;">' ..
			util.pcdata(desc) .. '</p>'
	end

	return h
end

-- 生成一个动作按钮。
--
-- 为什么用带 token 的链接，而不是 POST 表单 / 提交按钮：
--   1. 不能嵌套 <form>：CBI 整页本身已经在一个 <form> 里，HTML 解析器会
--      直接忽略嵌套的表单标签，按钮会彻底失效（点了没反应）。
--   2. 也不能用 type="submit" 按钮：那样会连同 CBI 主表单一起提交，触发
--      一次隐式的「保存并应用」，on_after_commit 随之重启服务 —— 点
--      「停止服务」会被立刻重新拉起来，按钮等于坏的。
--   3. CSRF 防护靠 token 而不是靠方法：攻击者读不到当前会话 token，
--      因此无法构造出可用的链接。
local function action_button(action, label, style)
	return '<a class="cbi-button cbi-button-' .. style .. '" ' ..
		'href="?cc_action=' .. action .. '&amp;token=' ..
		util.pcdata(SESSION_TOKEN) .. '" ' ..
		'style="margin:0 6px 6px 0;display:inline-block;">' ..
		util.pcdata(label) .. '</a>'
end


-- MAC 地址校验：AA:BB:CC:DD:EE:FF
-- CBI 约定：校验通过返回原值，失败返回 nil 加错误信息
local function validate_mac(self, value)
	if value == nil or value == "" then
		return value
	end

	if not value:match("^%x%x:%x%x:%x%x:%x%x:%x%x:%x%x$") then
		return nil, _("MAC 地址格式不正确，示例：AA:BB:CC:DD:EE:FF")
	end

	return value
end

-- ------------------------------------------------------------------
-- 配置节存在性兜底（用户误删配置文件时不至于整页报错）
-- ------------------------------------------------------------------

if not ucicursor:get(PACKAGE, SECTION) then
	ucicursor:section(PACKAGE, "main", SECTION, { enabled = "0" })
	ucicursor:commit(PACKAGE)
end

-- ------------------------------------------------------------------
-- Map
-- ------------------------------------------------------------------

local m = Map(PACKAGE,
	_("云端开关机"),
	_("路由器订阅巴法云主题：收到 on 发送网络唤醒封包开机，" ..
	  "收到 off 通过 SSH 关机，关机成功后才会向云端回报 off。"))

local test_html = nil   -- 连通性测试结果（已转义的 HTML）

-- 创建一个「纯展示」分组（无 UCI 配置节）
local function vsection(title, desc)
	local s = m:section(SimpleSection)
	local d = s:option(DummyValue, "_hdr")
	d.rawhtml = true
	d.cfgvalue = function()
		return header_html(title, desc)
	end
	return s
end

-- ==================================================================
-- 0. 服务状态
-- ==================================================================

local svc = vsection(_("服务状态"),
	_("守护进程由 procd 托管，异常退出会自动拉起。"))

local running = service_running()

local st = svc:option(DummyValue, "_status")
st.rawhtml = true
st.cfgvalue = function()
	local html
	if running then
		html = '<span style="color:#00a65a;font-weight:bold;font-size:15px;">' ..
			'&#9679; ' .. util.pcdata(_("正在运行")) .. '</span>'
	else
		html = '<span style="color:#dd4b39;font-weight:bold;font-size:15px;">' ..
			'&#9679; ' .. util.pcdata(_("已停止")) .. '</span>'
	end

	return html
end

local act = svc:option(DummyValue, "_actions")
act.rawhtml = true
act.cfgvalue = function()
	return action_button("start", _("启动服务"), "apply") ..
		action_button("stop", _("停止服务"), "reset") ..
		action_button("restart", _("重启服务"), "action")
end

-- ==================================================================
-- 1. 巴法云连接
-- ==================================================================

local s1 = m:section(NamedSection, SECTION, "main", _("巴法云连接"))
s1.addremove = false
s1.description = _("巴法云（bemfa.com）负责把「小爱同学 / 米家 / 巴法云 App」的指令" ..
	"通过 TCP 长连接推送到路由器。")

local o = s1:option(Flag, "enabled", _("启用服务"),
	_("关闭后守护进程不会连接巴法云，也不会响应任何指令。"))
o.rmempty = false
o.default = "0"

o = s1:option(Value, "client_id", _("Client ID（私钥）"))
o.description = _("填巴法云控制台 → 设备云 → 「私钥」里的 UID（32 位），" ..
	"不是登录账号或密码。留空则无法订阅主题。日志中只显示前 4 位。")
o.placeholder = "0123456789abcdef0123456789abcdef"
o.rmempty = true

o = s1:option(Value, "topic", _("Topic（主题名）"))
o.description = _("巴法云控制台里为该设备设置的主题名，默认 PC001。" ..
	"小爱同学 / 米家场景通过这个名字下发指令（on 或 off）。")
o.placeholder = "PC001"
o.default = "PC001"
o.rmempty = true

-- ==================================================================
-- 2. 目标主机
-- ==================================================================

local s2 = m:section(NamedSection, SECTION, "main", _("目标主机"))
s2.addremove = false
s2.description = _("被控电脑的信息。关机走 SSH，因此目标机必须开机并运行 SSH 服务端。")

o = s2:option(Value, "host", _("主机 IP 地址"))
o.description = _("局域网内被控电脑的 IPv4 地址，例如 192.168.1.100。" ..
	"建议在路由器上为它绑定静态 DHCP，避免 IP 变化后关机失效。" ..
	"（旧版配置里的 ip 字段会被自动读取。）")
o.datatype = "ip4addr"
o.placeholder = "192.168.1.100"
o.rmempty = true

o = s2:option(Value, "ssh_port", _("SSH 端口"))
o.description = _("目标机 SSH 服务端口，Windows OpenSSH Server 默认 22。")
o.datatype = "port"
o.default = "22"
o.placeholder = "22"
o.rmempty = true

o = s2:option(Value, "user", _("SSH 用户名"))
o.description = _("Windows 下通常是本机账户名。用微软账户登录时请执行 whoami " ..
	"查看实际用户名（可能形如 机器名\\用户名），不要填邮箱。")
o.rmempty = true

o = s2:option(Value, "password", _("SSH 密码"))
o.password = true
o.description = _("通过环境变量 SSHPASS 传给 sshpass，不会出现在进程命令行里。" ..
	"留空则改用 SSH 密钥登录（更安全，推荐）。")
o.rmempty = true

o = s2:option(ListValue, "host_os", _("操作系统类型"))
o.description = _("决定默认关机命令：Windows 用 shutdown /s /t 10 /f，" ..
	"Linux 用 shutdown -h now。")
o:value("windows", _("Windows"))
o:value("linux", _("Linux / 类 Unix"))
o.default = "windows"
o.rmempty = false

-- ==================================================================
-- 3. 远程开机（Wake-on-LAN）
-- ==================================================================

local s3 = m:section(NamedSection, SECTION, "main", _("远程开机（Wake-on-LAN）"))
s3.addremove = false
s3.description = _("开机走 WoL 广播魔术包，目标机不需要安装任何软件，" ..
	"但需要在 BIOS 和网卡里开启网络唤醒。")

-- 动态获取网卡列表；取不到就退回文本框
local niclist = {}
local ok, devs = pcall(sys.net.devices)
if ok and type(devs) == "table" then
	for _, dev in ipairs(devs) do
		if dev ~= "lo" then
			niclist[#niclist + 1] = dev
		end
	end
end

local cur_nic = ucicursor:get(PACKAGE, SECTION, "nic") or "br-lan"

if #niclist == 0 then
	o = s3:option(Value, "nic", _("出口网卡"))
	o.description = _("魔术包从该网卡以广播方式发出，x86 软路由一般是 br-lan。")
	o.default = "br-lan"
	o.placeholder = "br-lan"
	o.rmempty = false
else
	-- 当前配置值若不在设备列表里，补进去，避免下拉框显示为空
	local found = false
	for _, d in ipairs(niclist) do
		if d == cur_nic then found = true end
	end
	if not found and cur_nic ~= "" then
		niclist[#niclist + 1] = cur_nic
	end

	o = s3:option(ListValue, "nic", _("出口网卡"))
	o.description = _("魔术包从该网卡以广播方式发出。目标机必须和这个网卡在" ..
		"同一个二层网络（同网段 / 同 VLAN），否则广播包到不了。")
	for _, d in ipairs(niclist) do
		o:value(d, d)
	end
	o.default = "br-lan"
	o.rmempty = false
end

o = s3:option(Value, "mac", _("目标机 MAC 地址"))
o.description = _("格式 AA:BB:CC:DD:EE:FF。必须是有线网卡的 MAC 地址，" ..
	"无线网卡基本不支持 WoL。")
o.placeholder = "AA:BB:CC:DD:EE:FF"
o.validate = validate_mac
o.rmempty = true

-- ==================================================================
-- 4. 高级
-- ==================================================================

local s4 = m:section(NamedSection, SECTION, "main", _("高级"))
s4.addremove = false

o = s4:option(Value, "shutdown_cmd", _("自定义关机命令"))
o.description = _("留空则按「操作系统类型」自动选择关机命令。填写后整条命令作为" ..
	"单个参数交给 SSH 执行，例如 sudo shutdown -h now 或 shutdown /s /t 0 /f。")
o.placeholder = _("留空表示自动选择")
o.rmempty = true

o = s4:option(Value, "log_max_kb", _("日志大小上限（KB）"))
o.description = _("超过该大小后自动轮转为 cloud_control.log.1，" ..
	"防止 /var/log（tmpfs）被写满。默认 256，最小 16。")
o.datatype = "uinteger"
o.default = "256"
o.placeholder = "256"
o.rmempty = true

-- ==================================================================
-- 5. 连通性测试
-- ==================================================================

local s5 = vsection(_("连通性测试"),
	_("点击按钮立即执行一次对应动作，结果直接显示在下面。" ..
	  "注意：「测试 SSH 关机」会真的让目标机关机，请谨慎使用。"))

local tbtns = s5:option(DummyValue, "_test_buttons")
tbtns.rawhtml = true
tbtns.cfgvalue = function()
	return action_button("test_wol", _("测试开机封包"), "action") ..
		action_button("test_ssh", _("测试 SSH 关机"), "action") ..
		action_button("test_cloud", _("测试巴法云连接"), "action")
end

local tres = s5:option(DummyValue, "_test_result")
tres.rawhtml = true
tres.cfgvalue = function()
	if test_html then
		return test_html
	end
	return '<em>' .. util.pcdata(_("尚未执行测试。")) .. '</em>'
end

-- ==================================================================
-- 6. 运行日志
-- ==================================================================

local s6 = vsection(_("运行日志"),
	_("显示 " .. LOGFILE .. " 的最后 60 行，内容已做 HTML 转义。" ..
	  "client_id 只保留前 4 位，密码不会写入日志。"))

local lbtns = s6:option(DummyValue, "_log_buttons")
lbtns.rawhtml = true
lbtns.cfgvalue = function()
	return action_button("log_refresh", _("刷新"), "action") ..
		action_button("log_clear", _("清空日志"), "reset")
end

local lbody = s6:option(DummyValue, "_log_body")
lbody.rawhtml = true
lbody.cfgvalue = function()
	return build_log_html()
end

-- ==================================================================
-- 动作处理（必须放在 return m 之前：此时页面已构建完毕，
-- 测试类动作的结果由上面的 cfgvalue 在渲染时输出）
--
-- 安全：动作值先过白名单，写操作（启停/重启/测试/清日志）必须携带
-- 与当前会话一致的 token，否则 403 且不执行 —— 防止被外部页面用
-- 链接或跨站表单诱导触发（CSRF）。
-- ==================================================================

local ACTION_ALLOWED = {
	start       = true,
	stop        = true,
	restart     = true,
	test_wol    = true,
	test_ssh    = true,
	test_cloud  = true,
	log_refresh = true,
	log_clear   = true,
}

-- 只读动作不需要 token
local ACTION_READONLY = {
	log_refresh = true,
}

local action = http.formvalue("cc_action") or ""

if action ~= "" and not ACTION_ALLOWED[action] then
	-- 白名单外的一律当作没有动作，避免奇怪的值进入下面的分支
	action = ""
end

if action ~= "" and not ACTION_READONLY[action] then
	local got = http.formvalue("token")

	if SESSION_TOKEN ~= "" then
		-- 有 token 就必须匹配，不匹配一律拒绝
		if got ~= SESSION_TOKEN then
			http.status(403, "Forbidden")
			if sys and sys.log then
				sys.log("warn", "cloud_control: 拒绝缺少有效 token 的写操作")
			end
			action = ""
		end
	else
		-- 取不到会话 token（极旧固件）时放行，但要留下痕迹方便排查。
		-- 这个页面本身已在 LuCI 鉴权之后，让按钮整体失效的代价更大。
		if sys and sys.log then
			sys.log("warn", "cloud_control: 会话无 token，写操作未做 CSRF 校验")
		end
	end
end

if action == "start" then
	run_quiet(INITD .. " start")
	redirect_self()
elseif action == "stop" then
	run_quiet(INITD .. " stop")
	redirect_self()
elseif action == "restart" then
	run_quiet(INITD .. " restart")
	redirect_self()
elseif action == "test_wol" then
	local rc, out = run_capture(DAEMON .. " -t wol")
	test_html = build_test_html(rc, out)
elseif action == "test_ssh" then
	local rc, out = run_capture(DAEMON .. " -t ssh")
	test_html = build_test_html(rc, out)
elseif action == "test_cloud" then
	local rc, out = run_capture(DAEMON .. " -t cloud")
	test_html = build_test_html(rc, out)
elseif action == "log_refresh" then
	redirect_self()
elseif action == "log_clear" then
	-- 守护进程以 O_APPEND 方式写日志，截断后下一次写入会自动回到文件开头
	local fp = io.open(LOGFILE, "w")
	if fp then
		fp:close()
	end
	if fs.access(LOGROT) then
		os.remove(LOGROT)
	end
	redirect_self()
end

-- ==================================================================
-- 保存后联动：启用则启用服务并重启，禁用则停止服务
-- ==================================================================

function m.on_after_commit(self)
	local enabled = self.uci:get(PACKAGE, SECTION, "enabled")

	if enabled == "1" then
		run_quiet(INITD .. " enable")
		run_quiet(INITD .. " restart")
	else
		run_quiet(INITD .. " stop")
		run_quiet(INITD .. " disable")
	end
end

return m

'use strict';
'require view';
'require form';
'require fs';
'require rpc';
'require uci';
'require ui';

/*
 * 云端开关机 —— LuCI 配置界面（JS 视图）
 *
 * 为什么用 JS 视图而不是 Lua CBI：
 *   LuCI 自 21.02 起主推 JS 视图，25.12 分支已不再提供 Lua CBI 兼容层，
 *   旧的 luci.model.cbi 写法在 25.12 上不会显示。JS 视图从 19.07 一直
 *   支持到 25.12+，是唯一能跨版本通用的形态。
 *
 * 权限：所有文件读写 / 命令执行都走 rpcd，具体白名单见
 *   /usr/share/rpcd/acl.d/luci-app-cloud-control.json
 */

var DAEMON  = '/usr/bin/cloud_control';
var INITD   = '/etc/init.d/cloud_control';
var LOGFILE = '/var/log/cloud_control.log';
var STATEFILE = '/var/run/cloud_control.state';   /* 守护进程写、页面读 */

/* 查询 procd 服务状态：比 ps | grep 稳，且不受进程名截断影响 */
var callServiceList = rpc.declare({
	object: 'service',
	method: 'list',
	params: [ 'name' ],
	expect: { '': {} }
});

/* 渲染前要准备好的运行时信息 */
var RT = {
	running: false,
	logtext: '',
	state: {}
};

function loadStatus() {
	return callServiceList('cloud_control').then(function(res) {
		var inst = res && res.cloud_control && res.cloud_control.instances;
		var running = false;

		if (inst) {
			Object.keys(inst).forEach(function(k) {
				if (inst[k] && inst[k].running)
					running = true;
			});
		}

		RT.running = running;
	}).catch(function() {
		RT.running = false;
	});
}

function loadLog() {
	return fs.read(LOGFILE, 65536).then(function(data) {
		var lines = String(data || '').replace(/\r/g, '').split('\n');

		while (lines.length && lines[lines.length - 1] === '')
			lines.pop();

		RT.logtext = lines.slice(-60).join('\n');
	}).catch(function() {
		RT.logtext = '';
	});
}

/* 读取守护进程写下的状态快照（key=value 纯文本，无需 JSON 解析） */
function loadState() {
	return fs.read(STATEFILE, 8192).then(function(data) {
		var obj = {};

		String(data || '').replace(/\r/g, '').split('\n').forEach(function(l) {
			var i = l.indexOf('=');
			if (i > 0)
				obj[l.slice(0, i)] = l.slice(i + 1);
		});

		RT.state = obj;
	}).catch(function() {
		RT.state = {};
	});
}

/*
 * 执行一条命令并用弹窗展示结果。
 * 失败不抛异常，避免一次命令失败把整页交互卡住。
 */
function runAction(argv, title, reloadAfter) {
	return fs.exec(argv[0], argv.slice(1)).then(function(res) {
		var ok = res && res.code === 0;
		var out = '';

		if (res && res.stdout)
			out += res.stdout;
		if (res && res.stderr)
			out += res.stderr;

		ui.showModal(title, [
			E('p', {
				'style': 'margin:0 0 10px 0;font-weight:bold;color:%s'.format(ok ? '#00a65a' : '#dd4b39')
			}, ok ? _('执行成功（退出码 0）') : _('执行失败（退出码 %s）').format(res ? res.code : '?')),
			E('pre', {
				'style': 'max-height:320px;overflow:auto;margin:0;padding:8px;' +
				         'background:rgba(128,128,128,.12);font-size:12px;' +
				         'line-height:1.5;white-space:pre-wrap;word-break:break-all'
			}, out || _('（无输出）')),
			E('div', { 'style': 'margin-top:14px;text-align:right' }, [
				E('button', {
					'class': 'btn cbi-button',
					'click': function() {
					ui.hideModal();
					if (reloadAfter)
						window.location.reload();
				}
				}, _('关闭'))
			])
		]);
	}).catch(function(err) {
		ui.addNotification(null, E('p', {}, _('执行失败：%s').format(err)), 'danger');
	});
}

return view.extend({
	load: function() {
		return Promise.all([
			uci.load('cloud_control'),
			loadStatus(),
			loadLog(),
			loadState()
		]);
	},

	render: function() {
		var m, s, o;

		m = new form.Map('cloud_control', _('云端开关机'),
			_('路由器订阅巴法云主题：收到 on 发送网络唤醒封包开机，' +
			  '收到 off 通过 SSH 关机。开机走局域网广播，关机走 SSH，两条链路互相独立。'));

		/* ============================================================
		 * 服务状态
		 * ============================================================ */
		s = m.section(form.NamedSection, 'main', 'main', _('服务状态'));

		o = s.option(form.DummyValue, '_status', _('当前状态'));
		o.cfgvalue = function() {
			var enabled = (uci.get('cloud_control', 'main', 'enabled') == '1');
			var box = E('span', {}, [
				E('span', {
					'style': 'font-weight:bold;color:%s'.format(RT.running ? '#00a65a' : '#dd4b39')
				}, RT.running ? _('● 正在运行') : _('● 已停止'))
			]);

			if (!enabled) {
				box.appendChild(E('span', {
					'style': 'margin-left:10px;color:#dd4b39'
				}, _('配置未启用，不会连接巴法云，设备会一直显示离线')));
			}
			else if (!RT.running) {
				box.appendChild(E('span', {
					'style': 'margin-left:10px;color:#dd4b39'
				}, _('已启用但进程未运行，点「重启」或看下方日志')));
			}

			return box;
		};

		o = s.option(form.Button, '_start', _('启动服务'),
			_('需先在下方勾选「启用服务」并保存，否则启动会被跳过。'));
		o.inputtitle = _('启动');
		o.inputstyle = 'apply';
		o.onclick = function() {
			return runAction([ INITD, 'start' ], _('启动服务'));
		};

		o = s.option(form.Button, '_stop', _('停止服务'));
		o.inputtitle = _('停止');
		o.inputstyle = 'reset';
		o.onclick = function() {
			return runAction([ INITD, 'stop' ], _('停止服务'));
		};

		o = s.option(form.Button, '_restart', _('重启服务'));
		o.inputtitle = _('重启');
		o.inputstyle = 'action';
		o.onclick = function() {
			return runAction([ INITD, 'restart' ], _('重启服务'));
		};

		/* ============================================================
		 * 设备状态（实时）—— 不再是「只发指令、不知结果」
		 * ============================================================ */
	s = m.section(form.NamedSection, 'main', 'main', _('设备状态（实时）'));
	s.description = _('状态来自对目标机 SSH 端口的真实探测，不是「上次下发了什么指令」。' +
	                  '守护进程每 30 秒探测一次，手动开机 / 关机后需连续两次一致才确认（约 1 分钟）；' +
	                  '随后若与云端记录不一致，会在 60 秒同步窗口过后自动补发，上报失败则每 30 秒重试。' +
	                  '断网、配置不全或服务未启用时，本地探测与页面状态仍会照常更新（只是不上报云端）。' +
	                  '服务没启动时可以点「重新探测」手动查一次。' +
	                  '另外可以在巴法云控制台往本主题发 msg=status，插件会把当前状态回推到云端。');

		o = s.option(form.DummyValue, '_host_state', _('目标机当前状态'));
		o.cfgvalue = function() {
			var st = RT.state.state || 'unknown';
			var color = (st === 'on') ? '#00a65a' : ((st === 'off') ? '#888888' : '#dd4b39');
			var label = (st === 'on') ? _('● 开机中') :
			            ((st === 'off') ? _('○ 已关机') : _('? 未知'));
			var box = E('span', {}, [
				E('span', { 'style': 'font-size:15px;font-weight:bold;color:' + color }, label)
			]);

			if (!RT.state.state) {
				box.appendChild(E('div', {
					'style': 'margin-top:6px;color:#dd4b39;font-size:12px'
				}, _('尚未读到状态：服务可能未启动、状态文件还没生成，或配置里没填主机 IP')));
			} else {
				box.appendChild(E('div', {
					'style': 'margin-top:6px;color:#666;font-size:12px'
				}, _('最后探测：%s ｜ 依据：%s').format(
					RT.state.updated || '-', RT.state.detail || '-')));
			}

			return box;
		};

		o = s.option(form.DummyValue, '_last_action', _('最后一次动作 / 云端状态'));
		o.cfgvalue = function() {
			var st = RT.state;
			var cmd = st.last_cmd && st.last_cmd !== '-' ? st.last_cmd : '-';
			var cloud = st.cloud || 'unknown';
			/* cloud= 只是插件本地记录的「最后成功上报值」，巴法云没有查询接口，
			   不能假装在向云端查询。如实标注，未确认过就直说尚未成功上报。 */
			var confirmed = (st.cloud_confirmed === '1');
			var pending = (st.cloud_pending === '1');
			var cloudLabel = (cloud === 'on') ? _('开机') :
			                 ((cloud === 'off') ? _('关机') : _('未知'));
			var cloudText;

			if (!confirmed || cloud === 'unknown')
				cloudText = _('尚未成功上报（本地记录，未向云端查询）');
			else
				cloudText = _('云端已上报：%s（本地记录，未向云端查询）').format(cloudLabel);

			if (pending)
				cloudText += _(' ｜ 有新状态待同步，插件会自动补发');

			return E('div', { 'style': 'font-size:12px;line-height:1.8;color:#666' }, [
				E('div', {}, _('指令：%s ｜ 时间：%s').format(cmd, st.last_cmd_at || '-')),
				E('div', {}, _('结果：%s').format(st.last_result || '-')),
				E('div', {}, cloudText),
				E('div', {}, _('状态快照：%s ｜ 版本 %s').format(st.source || '-', st.version || '-'))
			]);
		};

		o = s.option(form.Button, '_st_reload', _('刷新显示'));
		o.inputtitle = _('刷新');
		o.inputstyle = 'action';
		o.onclick = function() { window.location.reload(); };

		o = s.option(form.Button, '_st_probe', _('重新探测'),
			_('立即探测一次目标机状态并写入状态快照，不改变电脑开关状态。'));
		o.inputtitle = _('重新探测');
		o.inputstyle = 'action';
		o.onclick = function() {
			return runAction([ DAEMON, '-t', 'status' ], _('重新探测目标机状态'), true);
		};

		o = s.option(form.Button, '_st_on', _('立即开机'),
			_('先查状态：已经开机就只回报、不重复发唤醒包。'));
		o.inputtitle = _('立即开机');
		o.inputstyle = 'apply';
		o.onclick = function() {
			return runAction([ DAEMON, '-t', 'on' ], _('立即开机'), true);
		};

		o = s.option(form.Button, '_st_off', _('立即关机'),
			_('先查状态：已经关机就只回报、不重复下发 SSH 关机。注意本操作会真的关机。'));
		o.inputtitle = _('立即关机');
		o.inputstyle = 'reset';
		o.onclick = function() {
			return runAction([ DAEMON, '-t', 'off' ], _('立即关机'), true);
		};

		o = s.option(form.Button, '_st_retry', _('重试上次动作'),
			_('按状态文件记录的最后一次指令重做一次；若当前状态已经符合，则只回报不重发。'));
		o.inputtitle = _('重试');
		o.inputstyle = 'action';
		o.onclick = function() {
			return runAction([ DAEMON, '-t', 'retry' ], _('重试上次动作'), true);
		};

		o = s.option(form.Button, '_st_push', _('同步状态到云端'),
			_('把当前探测到的真实状态主动推给巴法云，用于纠正云端显示不一致。' +
			  '弹窗会明确显示推送成功或失败；巴法云只有上报接口，插件不会去查询云端当前显示。'));
		o.inputtitle = _('同步');
		o.inputstyle = 'action';
		o.onclick = function() {
			return runAction([ DAEMON, '-t', 'push' ], _('同步状态到云端'), true);
		};

		/* ============================================================
		 * 巴法云连接
		 * ============================================================ */
		s = m.section(form.NamedSection, 'main', 'main', _('巴法云连接'));
		s.description = _('巴法云（bemfa.com）把「小爱同学 / 米家」的指令通过 TCP 长连接推送到路由器。');

		o = s.option(form.Flag, 'enabled', _('启用服务'),
			_('关闭后守护进程不会连接巴法云，也不会响应任何指令；但仍会继续探测目标机状态并更新本页面，只是不主动上报云端。'));
		o.rmempty = false;
		o.default = '0';

		o = s.option(form.Value, 'client_id', _('Client ID（私钥）'),
			_('巴法云控制台 → 设备云 →「私钥」里的 UID（32 位）。' +
			  '不是登录账号或密码。留空无法订阅。日志中只显示前 4 位。'));
		o.placeholder = '0123456789abcdef0123456789abcdef';
		o.rmempty = true;

		o = s.option(form.Value, 'topic', _('Topic（主题名）'),
			_('巴法云控制台里给该设备设置的主题名，默认 PC001。' +
			  '小爱 / 米家场景通过这个名字下发 on 或 off。'));
		o.placeholder = 'PC001';
		o.default = 'PC001';
		o.rmempty = true;

		/* ============================================================
		 * 目标主机
		 * ============================================================ */
		s = m.section(form.NamedSection, 'main', 'main', _('目标主机'));
		s.description = _('被控电脑的信息。关机走 SSH，所以目标机必须开机并运行 SSH 服务端' +
		                  '（Windows 需在「可选功能」里安装 OpenSSH 服务器）。');

		o = s.option(form.Value, 'host', _('主机 IP 地址'),
			_('局域网内被控电脑的 IPv4 地址。建议在路由器上给它绑定静态 DHCP，' +
			  '避免 IP 变化后关机失效。'));
		o.datatype = 'ip4addr';
		o.placeholder = '192.168.1.100';
		o.rmempty = true;

		o = s.option(form.Value, 'ssh_port', _('SSH 端口'),
			_('Windows OpenSSH Server 默认 22。'));
		o.datatype = 'port';
		o.default = '22';
		o.placeholder = '22';
		o.rmempty = true;

		o = s.option(form.Value, 'user', _('SSH 用户名'),
			_('Windows 下通常是本机账户名。用微软账户登录时请执行 whoami 查看实际用户名，' +
			  '不要填邮箱。'));
		o.rmempty = true;

		o = s.option(form.Value, 'password', _('SSH 密码'),
			_('通过环境变量 SSHPASS 传给 sshpass，不会出现在进程命令行里。' +
			  '留空则改用 SSH 密钥登录（更安全）。'));
		o.password = true;
		o.rmempty = true;

		o = s.option(form.ListValue, 'host_os', _('操作系统类型'),
			_('决定默认关机命令：Windows 用 shutdown /s /t 10 /f，Linux 用 shutdown -h now。'));
		o.value('windows', _('Windows'));
		o.value('linux', _('Linux / 类 Unix'));
		o.default = 'windows';
		o.rmempty = false;

		/* ============================================================
		 * 远程开机
		 * ============================================================ */
		s = m.section(form.NamedSection, 'main', 'main', _('远程开机（Wake-on-LAN）'));
		s.description = _('开机走局域网广播魔术包，目标机不需要装任何软件，' +
		                  '但要在 BIOS 和网卡属性里开启网络唤醒，并关闭 Windows 快速启动。');

		o = s.option(form.Value, 'nic', _('出口网卡'),
			_('魔术包从该网卡以广播方式发出。x86 软路由一般是 br-lan；' +
			  '不确定的话在路由器上执行 ip link 查看。目标机必须和这个网卡在同一二层网络。'));
		o.default = 'br-lan';
		o.placeholder = 'br-lan';
		o.rmempty = false;

		o = s.option(form.Value, 'mac', _('目标机 MAC 地址'),
			_('格式 AA:BB:CC:DD:EE:FF。必须是有线网卡的 MAC，无线网卡基本不支持 WoL。'));
		o.placeholder = 'AA:BB:CC:DD:EE:FF';
		o.rmempty = true;
		o.validate = function(section_id, value) {
			if (!value)
				return true;
			if (!/^[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}$/.test(value))
				return _('MAC 地址格式不正确，示例：AA:BB:CC:DD:EE:FF');
			return true;
		};

		/* ============================================================
		 * 高级
		 * ============================================================ */
		s = m.section(form.NamedSection, 'main', 'main', _('高级'));

		o = s.option(form.Value, 'shutdown_cmd', _('自定义关机命令'),
			_('留空则按「操作系统类型」自动选择。填写后整条命令作为单个参数交给 SSH 执行，' +
			  '例如 sudo shutdown -h now 或 shutdown /s /t 0 /f。'));
		o.placeholder = _('留空表示自动选择');
		o.rmempty = true;

		o = s.option(form.Value, 'log_max_kb', _('日志大小上限（KB）'),
			_('超过该大小后自动轮转为 cloud_control.log.1，防止 /var/log（内存盘）被写满。' +
			  '默认 256，最小 16。'));
		o.datatype = 'uinteger';
		o.default = '256';
		o.placeholder = '256';
		o.rmempty = true;

		o = s.option(form.Flag, 'state_sync', _('周期探测并同步真实状态到云端'),
			_('守护进程定时探测目标机 SSH 端口；状态变化且云端记录不一致时，主动把真实状态推给巴法云。' +
			  '60 秒最小间隔内被拦下的变化不会丢弃，会在窗口过后自动补发；上报失败每 30 秒重试一次。' +
			  '关闭后本地探测和状态文件照常更新、动作执行后的结果回报也保留，只是不再主动同步云端。' +
			  '关闭期间发生的状态变化不会追溯补发；重新启用只对此后新发生的变化生效。需要立即纠偏，请点「同步状态到云端」或往主题发 msg=status。'));
		o.default = '1';
		o.rmempty = false;

		o = s.option(form.Value, 'state_interval', _('状态探测间隔（秒）'),
			_('默认 30 秒，允许 10 ~ 300。状态变化需要连续两次探测一致才会提交，避免开机过程中的抖动。'));
		o.datatype = 'range(10, 300)';
		o.default = '30';
		o.placeholder = '30';
		o.rmempty = true;

		o = s.option(form.ListValue, 'retry_times', _('状态未达预期时自动重试次数'),
			_('动作执行后重新探测状态，若没达到目标状态就重复执行该动作。' +
			  '默认 0（不重试）；开机 / 关机不稳定时可设为 1 ~ 2。注意主机会被重复操作。'));
		o.value('0', _('0 次（不重试）'));
		o.value('1', _('1 次'));
		o.value('2', _('2 次'));
		o.value('3', _('3 次'));
		o.default = '0';
		o.rmempty = false;

		o = s.option(form.Flag, 'verify_offline', _('关机后确认是否真的离线'),
			_('下发关机命令后持续探测目标机的 SSH 端口，连续 3 次连不上才认为关机成功，' +
			  '然后才向云端回报 off。用端口探测而不是 ping —— Windows 防火墙默认不回 ICMP，' +
			  '用 ping 会把「开着」误判成「已关机」。关闭后只依据 SSH 返回码。'));
		o.default = '1';
		o.rmempty = false;

		/* ============================================================
		 * 连通性测试
		 * ============================================================ */
		s = m.section(form.NamedSection, 'main', 'main', _('连通性测试'));
		s.description = _('测试读取的是已保存的配置，改完参数请先「保存并应用」再测。' +
		                  '「测试巴法云连接」除了连服务器，还会用 HTTP 接口校验主题是否真的存在' +
		                  '——TCP 订阅对任何主题都返回成功，光看它靠不住。' +
		                  '注意：「测试 SSH 关机」会真的让目标机关机。');

		o = s.option(form.Button, '_test_wol', _('测试开机封包'));
		o.inputtitle = _('发送魔术包');
		o.inputstyle = 'action';
		o.onclick = function() {
			return runAction([ DAEMON, '-t', 'wol' ], _('测试开机封包'));
		};

		o = s.option(form.Button, '_test_ssh', _('测试 SSH 关机'));
		o.inputtitle = _('执行关机');
		o.inputstyle = 'reset';
		o.onclick = function() {
			return runAction([ DAEMON, '-t', 'ssh' ], _('测试 SSH 关机'));
		};

		o = s.option(form.Button, '_test_cloud', _('测试巴法云连接'));
		o.inputtitle = _('测试连接');
		o.inputstyle = 'action';
		o.onclick = function() {
			return runAction([ DAEMON, '-t', 'cloud' ], _('测试巴法云连接'));
		};

		/* ============================================================
		 * 运行日志
		 * ============================================================ */
		s = m.section(form.NamedSection, 'main', 'main', _('运行日志'));
		s.description = _('显示 /var/log/cloud_control.log 的最后 60 行。' +
		                  'client_id 只保留前 4 位，密码不会写入日志。');

		o = s.option(form.DummyValue, '_log', _('最近 60 行'));
		o.cfgvalue = function() {
			return E('pre', {
				'style': 'max-height:340px;overflow:auto;margin:0;padding:8px;' +
				         'background:rgba(128,128,128,.12);font-size:12px;' +
				         'line-height:1.5;white-space:pre-wrap;word-break:break-all'
			}, RT.logtext || _('暂无日志：服务可能尚未启动，或日志文件尚未生成。'));
		};

		o = s.option(form.Button, '_log_refresh', _('刷新'));
		o.inputtitle = _('刷新日志');
		o.inputstyle = 'action';
		o.onclick = function() {
			window.location.reload();
		};

		o = s.option(form.Button, '_log_clear', _('清空日志'));
		o.inputtitle = _('清空');
		o.inputstyle = 'reset';
		o.onclick = function() {
			return fs.write(LOGFILE, '').then(function() {
				ui.addNotification(null, E('p', {}, _('日志已清空')), 'info');
				window.setTimeout(function() { window.location.reload(); }, 600);
			}).catch(function(err) {
				ui.addNotification(null, E('p', {}, _('清空失败：%s').format(err)), 'danger');
			});
		};

		return m.render();
	}
});

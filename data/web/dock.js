// Camdrop supervision dock: one card per camera with network, phone state and alerts.
'use strict';

const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);
const code = params.get('code') || '';

const dock = {
	ws: null,
	attempt: 0,
	knownAlerts: new Map(), // camera name -> Set of alert keys
	audio: null,
	sound: (() => {
		try {
			return localStorage.getItem('camdrop.dockSound') === '1';
		} catch {
			return false;
		}
	})(),
};

function el(tag, cls, text) {
	const e = document.createElement(tag);
	if (cls) e.className = cls;
	if (text !== undefined) e.textContent = text;
	return e;
}

function fmt(n, digits = 0) {
	if (n === undefined || n === null || Number.isNaN(n)) return '–';
	return Number(n).toFixed(digits);
}

function computeAlerts(cam) {
	const alerts = []; // {key, text, major}
	const tel = cam.telemetry || {};
	if (cam.state === 'disconnected' || cam.state === 'failed' || cam.state === 'closed') {
		alerts.push({ key: 'disconnected', text: t('alertDisconnected'), major: true });
		return alerts;
	}
	if (typeof tel.battery === 'number' && tel.battery < 20 && !tel.charging)
		alerts.push({ key: 'battery', text: t('alertBattery', { level: tel.battery }), major: true });
	if (cam.hidden || tel.visible === false) alerts.push({ key: 'hidden', text: t('alertHidden'), major: true });
	if (cam.state === 'live' && cam.uptime > 4) {
		if (!cam.fps || cam.fps < 1) alerts.push({ key: 'novideo', text: t('alertNoVideo'), major: true });
		else if (tel.fps && cam.fps < 0.6 * Math.max(tel.fps, 15))
			alerts.push({ key: 'fps', text: t('alertFps', { fps: fmt(cam.fps) }), major: false });
		else if (cam.fps < 12) alerts.push({ key: 'fps', text: t('alertFps', { fps: fmt(cam.fps) }), major: false });
	}
	if (cam.lossPct > 5) alerts.push({ key: 'loss', text: t('alertLoss', { loss: fmt(cam.lossPct, 1) }), major: cam.lossPct > 10 });
	if (tel.qualityLimitation === 'cpu' || tel.cpuLimited) alerts.push({ key: 'cpu', text: t('alertCpu'), major: false });
	if (tel.pressure === 'serious' || tel.pressure === 'critical')
		alerts.push({ key: 'pressure', text: t('alertPressure', { p: tel.pressure }), major: tel.pressure === 'critical' });
	if (cam.telemetryAge > 15) alerts.push({ key: 'stale', text: t('alertStale'), major: false });
	return alerts;
}

function beep() {
	if (!dock.sound) return;
	try {
		dock.audio = dock.audio || new (window.AudioContext || window.webkitAudioContext)();
		const ctx = dock.audio;
		const osc = ctx.createOscillator();
		const gain = ctx.createGain();
		osc.frequency.value = 880;
		gain.gain.setValueAtTime(0.0001, ctx.currentTime);
		gain.gain.exponentialRampToValueAtTime(0.2, ctx.currentTime + 0.02);
		gain.gain.exponentialRampToValueAtTime(0.0001, ctx.currentTime + 0.35);
		osc.connect(gain).connect(ctx.destination);
		osc.start();
		osc.stop(ctx.currentTime + 0.4);
	} catch {
		/* no audio */
	}
}

function action(label, onClick) {
	const b = el('button', '', label);
	b.addEventListener('click', onClick);
	return b;
}

function send(msg) {
	if (dock.ws && dock.ws.readyState === WebSocket.OPEN) dock.ws.send(JSON.stringify(msg));
}

function renderCamera(cam, gone) {
	const alerts = computeAlerts(cam);
	const card = el('section', 'cam');
	if (gone) card.classList.add('gone');
	else if (alerts.some((a) => a.major)) card.classList.add('alert');
	else if (alerts.length) card.classList.add('warn');

	const head = el('div', 'head');
	head.appendChild(el('span', 'name', cam.name));
	const stateKey = 'dockState_' + (cam.state || 'connecting');
	const badge = el('span', 'badge', t(stateKey));
	if (cam.state === 'live') badge.classList.add('live');
	if (gone || cam.state === 'failed' || cam.state === 'unstable') badge.classList.add('bad');
	head.appendChild(badge);
	card.appendChild(head);

	const tel = cam.telemetry || {};
	if (gone) {
		card.appendChild(el('div', 'line', t('dockAgo', { s: Math.round(cam.ago || 0) }) + (cam.reason ? ' · ' + cam.reason : '')));
	} else {
		const video = el('div', 'line');
		video.innerHTML = '';
		video.append(
			el('b', '', `${cam.width || '?'}×${cam.height || '?'}`),
			` · ${fmt(cam.fps)} fps · ${fmt((cam.videoKbps || 0) + (cam.audioKbps || 0))} kbps · ${cam.codec || '–'}${cam.hardware ? ' (HW)' : ''}`,
		);
		card.appendChild(video);

		const net = el('div', 'line');
		net.append(
			el('b', '', t('dockNetwork')),
			` RTT ${fmt(cam.rttMs)} ms · ${fmt(cam.lossPct, 1)} % · NACK ${cam.nacks || 0}/${cam.recoveredPackets || 0}`,
		);
		card.appendChild(net);

		const delay = el('div', 'line');
		delay.append(el('b', '', t('dockDelay')), ` ${fmt(cam.delayMs)} ms · buffer ${cam.bufferMs ?? '–'} ms${cam.avSynced ? ' · A/V sync' : ''}`);
		card.appendChild(delay);

		const phone = [];
		if (typeof tel.battery === 'number') phone.push(`${t('dockBattery')} ${tel.battery} %${tel.charging ? ' (' + t('dockCharging') + ')' : ''}`);
		if (tel.pressure) phone.push(`CPU ${tel.pressure}`);
		if (tel.qualityLimitation && tel.qualityLimitation !== 'none') phone.push(`limit: ${tel.qualityLimitation}`);
		if (tel.encoder) phone.push(tel.encoder);
		if (phone.length) card.appendChild(el('div', 'line', phone.join(' · ')));
		card.appendChild(el('div', 'line', `${cam.ip || ''}${cam.watchers !== undefined ? ' · ' + cam.watchers + ' source(s)' : ''}`));
	}

	if (alerts.length) {
		const box = el('div', 'alerts');
		alerts.forEach((a) => box.appendChild(el('span', a.major ? '' : 'minor', a.text)));
		card.appendChild(box);
	}

	const actions = el('div', 'actions');
	if (gone) {
		actions.appendChild(action(t('dockForget'), () => send({ type: 'forget', name: cam.name })));
	} else {
		actions.appendChild(action(t('dockKeyframe'), () => send({ type: 'keyframe', id: cam.id })));
		actions.appendChild(action(t('dockIdentify'), () => send({ type: 'identify', id: cam.id })));
		actions.appendChild(action(t('dockKick'), () => send({ type: 'kick', id: cam.id })));
	}
	card.appendChild(actions);

	// Sound on new major alerts.
	const keys = new Set(alerts.filter((a) => a.major).map((a) => a.key));
	const known = dock.knownAlerts.get(cam.name) || new Set();
	for (const k of keys) if (!known.has(k)) beep();
	dock.knownAlerts.set(cam.name, keys);
	return card;
}

function render(status) {
	const root = $('cameras');
	root.innerHTML = '';
	const cams = status.cameras || [];
	const departed = status.departed || [];
	cams.forEach((c) => root.appendChild(renderCamera(c, false)));
	departed.forEach((c) => root.appendChild(renderCamera(c, true)));
	$('summary').textContent = `${cams.length}/${status.maxClients || '?'}`;
	const empty = $('empty');
	empty.hidden = cams.length + departed.length > 0;
	empty.textContent = t('dockEmptyShort');
}

function connect() {
	const proto = location.protocol === 'https:' ? 'wss' : 'ws';
	const ws = new WebSocket(`${proto}://${location.host}/ws/dock?code=${encodeURIComponent(code)}`);
	dock.ws = ws;
	let opened = false;
	ws.onopen = () => {
		opened = true;
		dock.attempt = 0;
		$('connection').hidden = true;
	};
	ws.onmessage = (e) => {
		try {
			const msg = JSON.parse(e.data);
			if (msg.type === 'status') render(msg);
		} catch {
			/* ignore */
		}
	};
	ws.onclose = () => {
		const banner = $('connection');
		banner.hidden = false;
		banner.textContent = opened || dock.attempt < 2 ? t('dockOffline') : t('dockAuthError') + ' / ' + t('dockOffline');
		dock.attempt++;
		setTimeout(connect, Math.min(10000, 1000 * dock.attempt));
	};
}

applyTranslations();
$('sound').checked = dock.sound;
$('sound').addEventListener('change', () => {
	dock.sound = $('sound').checked;
	try {
		localStorage.setItem('camdrop.dockSound', dock.sound ? '1' : '0');
	} catch {
		/* ignore */
	}
	if (dock.sound) beep();
});
connect();

// Camdrop supervision dock: one card per camera with network, phone state, alerts and remote controls.
'use strict';

const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);
const code = params.get('code') || '';

const dock = {
	ws: null,
	attempt: 0,
	cards: new Map(), // "live:<name>" / "gone:<name>" -> card
	knownAlerts: new Map(), // camera name -> Set of major alert keys
	orientation: 'any',
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

function send(msg) {
	if (dock.ws && dock.ws.readyState === WebSocket.OPEN) dock.ws.send(JSON.stringify(msg));
}

function computeAlerts(cam) {
	const alerts = []; // {key, text, major}
	const tel = cam.telemetry || {};
	if (cam.state === 'disconnected' || cam.state === 'failed' || cam.state === 'closed') {
		alerts.push({ key: 'disconnected', text: t('alertDisconnected'), major: true });
		return alerts;
	}
	const videoOn = (cam.video || tel.video || 'camera') !== 'none';
	if (typeof tel.battery === 'number' && tel.battery < 20 && !tel.charging) alerts.push({ key: 'battery', text: t('alertBattery', { level: tel.battery }), major: true });
	if (cam.hidden || tel.visible === false) alerts.push({ key: 'hidden', text: t('alertHidden'), major: true });
	if (videoOn && cam.state === 'live' && cam.uptime > 4) {
		if (!cam.fps || cam.fps < 1) alerts.push({ key: 'novideo', text: t('alertNoVideo'), major: true });
		else if (tel.fps && cam.fps < 0.6 * Math.max(tel.fps, 15)) alerts.push({ key: 'fps', text: t('alertFps', { fps: fmt(cam.fps) }), major: false });
		else if (cam.fps < 12) alerts.push({ key: 'fps', text: t('alertFps', { fps: fmt(cam.fps) }), major: false });
	}
	if (cam.lossPct > 5) alerts.push({ key: 'loss', text: t('alertLoss', { loss: fmt(cam.lossPct, 1) }), major: cam.lossPct > 10 });
	if (tel.qualityLimitation === 'cpu' || tel.cpuLimited) alerts.push({ key: 'cpu', text: t('alertCpu'), major: false });
	if (tel.pressure === 'serious' || tel.pressure === 'critical') alerts.push({ key: 'pressure', text: t('alertPressure', { p: tel.pressure }), major: tel.pressure === 'critical' });
	if (videoOn && dock.orientation !== 'any' && tel.orientation && tel.orientation !== dock.orientation && (cam.video || tel.video) !== 'screen')
		alerts.push({ key: 'orientation', text: t(tel.orientation === 'portrait' ? 'alertPortrait' : 'alertLandscape'), major: false });
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

function button(label, onClick, cls) {
	const b = el('button', cls || '', label);
	b.type = 'button';
	b.addEventListener('click', onClick);
	return b;
}

function select(options) {
	const s = el('select');
	options.forEach(([value, label]) => {
		const o = el('option', '', label);
		o.value = String(value);
		s.appendChild(o);
	});
	return s;
}

// Do not overwrite a control the user is touching.
function busy(input) {
	return document.activeElement === input || input.dataset.dragging === '1';
}

// ---------------------------------------------------------------------------------------------
// Remote controls of one camera

function createControls(card) {
	const box = el('details', 'controls');
	box.appendChild(el('summary', '', t('dockControls')));
	const control = (set) => send({ type: 'control', id: card.cam.id, set });

	const rowCam = el('div', 'crow');
	const camera = el('select');
	camera.addEventListener('change', () => control({ deviceId: camera.value }));
	const flip = button(t('dockFlip'), () => control({ facing: 'toggle' }));
	rowCam.append(el('span', 'clabel', t('dockCamera')), camera, flip);

	const rowZoom = el('div', 'crow');
	const zoom = el('input');
	zoom.type = 'range';
	const zoomValue = el('span', 'cvalue');
	let zoomTimer = null;
	const sendZoom = () => {
		zoomTimer = null;
		control({ zoom: Number(zoom.value) });
	};
	zoom.addEventListener('pointerdown', () => (zoom.dataset.dragging = '1'));
	zoom.addEventListener('pointerup', () => (zoom.dataset.dragging = ''));
	zoom.addEventListener('input', () => {
		zoomValue.textContent = `×${Number(zoom.value).toFixed(1)}`;
		if (!zoomTimer) zoomTimer = setTimeout(sendZoom, 120);
	});
	zoom.addEventListener('change', () => {
		clearTimeout(zoomTimer);
		sendZoom();
	});
	rowZoom.append(el('span', 'clabel', t('dockZoom')), zoom, zoomValue);

	const rowToggles = el('div', 'crow');
	const torch = button(t('dockTorch'), () => control({ torch: !card.cam.caps.torch }), 'toggle');
	const mute = button(t('dockMute'), () => control({ muted: !card.cam.caps.muted }), 'toggle');
	const video = button(t('dockVideoOff'), () => control({ video: card.cam.video === 'none' ? 'camera' : 'none' }), 'toggle');
	rowToggles.append(torch, mute, video);

	const rowQuality = el('div', 'crow');
	const quality = select([
		[480, '480p'],
		[720, '720p'],
		[1080, '1080p'],
	]);
	quality.addEventListener('change', () => control({ height: Number(quality.value) }));
	const fps = select([
		[30, '30 fps'],
		[60, '60 fps'],
	]);
	fps.addEventListener('change', () => control({ fps: Number(fps.value) }));
	rowQuality.append(el('span', 'clabel', t('dockQuality')), quality, fps);

	box.append(rowCam, rowZoom, rowToggles, rowQuality);
	card.controls = { box, camera, flip, rowCam, zoom, zoomValue, rowZoom, torch, mute, video, quality, fps, devicesKey: '' };
	return box;
}

function updateControls(card) {
	const c = card.controls;
	const caps = card.cam.caps || {};
	const cameraMode = (card.cam.video || caps.video) === 'camera';

	const devices = caps.devices || [];
	const key = JSON.stringify(devices);
	if (key !== c.devicesKey) {
		c.devicesKey = key;
		c.camera.innerHTML = '';
		devices.forEach((d) => {
			const o = el('option', '', d.label);
			o.value = d.id;
			c.camera.appendChild(o);
		});
	}
	if (!busy(c.camera) && caps.deviceId) c.camera.value = caps.deviceId;
	c.rowCam.hidden = !cameraMode || devices.length === 0;
	c.flip.disabled = devices.length < 2 && !caps.facing;

	const z = caps.zoom;
	c.rowZoom.hidden = !cameraMode || !z;
	if (z && !busy(c.zoom)) {
		c.zoom.min = z.min;
		c.zoom.max = z.max;
		c.zoom.step = z.step || 0.1;
		c.zoom.value = z.value;
		c.zoomValue.textContent = `×${Number(z.value).toFixed(1)}`;
	}

	c.torch.hidden = !cameraMode || typeof caps.torch !== 'boolean';
	c.torch.classList.toggle('on', !!caps.torch);
	c.mute.hidden = !caps.mic;
	c.mute.textContent = t(caps.muted ? 'dockUnmute' : 'dockMute');
	c.mute.classList.toggle('on', !!caps.muted);
	c.video.textContent = t(card.cam.video === 'none' ? 'dockVideoOn' : 'dockVideoOff');
	c.video.hidden = (card.cam.video || caps.video) === 'screen';
	if (!busy(c.quality) && caps.height) c.quality.value = String(caps.height);
	if (!busy(c.fps) && caps.fps) c.fps.value = String(caps.fps);
}

// ---------------------------------------------------------------------------------------------
// Cards

function createCard(gone) {
	const card = { cam: {}, gone };
	card.root = el('section', 'cam');
	const head = el('div', 'head');
	card.name = el('span', 'name');
	card.tally = el('span', 'badge tally');
	card.badge = el('span', 'badge');
	head.append(card.name, card.tally, card.badge);
	card.info = el('div', 'info');
	card.alerts = el('div', 'alerts');
	const actions = el('div', 'actions');
	if (gone) {
		actions.appendChild(button(t('dockForget'), () => send({ type: 'forget', name: card.cam.name })));
	} else {
		actions.append(
			button(t('dockKeyframe'), () => send({ type: 'keyframe', id: card.cam.id })),
			button(t('dockIdentify'), () => send({ type: 'identify', id: card.cam.id })),
			button(t('dockKick'), () => send({ type: 'kick', id: card.cam.id })),
		);
	}
	card.root.append(head, card.info, card.alerts, actions);
	if (!gone) card.root.appendChild(createControls(card));
	return card;
}

function line(label, text) {
	const d = el('div', 'line');
	if (label) d.appendChild(el('b', '', label));
	d.append(text);
	return d;
}

function updateCard(card, cam) {
	card.cam = cam;
	const gone = card.gone;
	const alerts = computeAlerts(cam);
	card.root.classList.toggle('gone', gone);
	card.root.classList.toggle('alert', !gone && alerts.some((a) => a.major));
	card.root.classList.toggle('warn', !gone && alerts.length > 0 && !alerts.some((a) => a.major));
	card.root.classList.toggle('program', !gone && cam.tally === 'program');

	card.name.textContent = cam.name;
	card.badge.textContent = t('dockState_' + (cam.state || 'connecting'));
	card.badge.classList.toggle('live', cam.state === 'live');
	card.badge.classList.toggle('bad', gone || cam.state === 'failed' || cam.state === 'unstable');
	const tally = !gone && (cam.tally === 'program' || cam.tally === 'preview') ? cam.tally : '';
	card.tally.hidden = !tally;
	card.tally.textContent = tally ? t('dockTally_' + tally) : '';
	card.tally.classList.toggle('program', tally === 'program');
	card.tally.classList.toggle('preview', tally === 'preview');

	const tel = cam.telemetry || {};
	const lines = [];
	if (gone) {
		lines.push(line('', t('dockAgo', { s: Math.round(cam.ago || 0) }) + (cam.reason ? ' · ' + cam.reason : '')));
	} else {
		const mode = cam.video || tel.video || 'camera';
		if (mode === 'none') lines.push(line(t('dockVideo'), ' ' + t('dockAudioOnly')));
		else
			lines.push(
				line(
					`${cam.width || '?'}×${cam.height || '?'}`,
					` · ${fmt(cam.fps)} fps · ${fmt((cam.videoKbps || 0) + (cam.audioKbps || 0))} kbps · ${cam.codec || '–'}${cam.hardware ? ' (HW)' : ''}${mode === 'screen' ? ' · ' + t('dockScreen') : ''}`,
				),
			);
		lines.push(line(t('dockNetwork'), ` RTT ${fmt(cam.rttMs)} ms · ${fmt(cam.lossPct, 1)} % · NACK ${cam.nacks || 0}/${cam.recoveredPackets || 0}`));
		lines.push(line(t('dockDelay'), ` ${fmt(cam.delayMs)} ms · buffer ${cam.bufferMs ?? '–'} ms${cam.avSynced ? ' · A/V sync' : ''}`));
		const phone = [];
		if (typeof tel.battery === 'number') phone.push(`${t('dockBattery')} ${tel.battery} %${tel.charging ? ' (' + t('dockCharging') + ')' : ''}`);
		if (tel.pressure) phone.push(`CPU ${tel.pressure}`);
		if (tel.qualityLimitation && tel.qualityLimitation !== 'none') phone.push(`limit: ${tel.qualityLimitation}`);
		if (tel.encoder) phone.push(tel.encoder);
		if (phone.length) lines.push(line('', phone.join(' · ')));
		lines.push(line('', `${cam.ip || ''}${cam.watchers !== undefined ? ' · ' + cam.watchers + ' source(s)' : ''}`));
	}
	card.info.replaceChildren(...lines);

	card.alerts.replaceChildren(...alerts.map((a) => el('span', a.major ? '' : 'minor', a.text)));
	card.alerts.hidden = alerts.length === 0;

	if (card.controls) updateControls(card);

	// Sound on new major alerts.
	const keys = new Set(alerts.filter((a) => a.major).map((a) => a.key));
	const known = dock.knownAlerts.get(cam.name) || new Set();
	for (const k of keys) if (!known.has(k)) beep();
	dock.knownAlerts.set(cam.name, keys);
}

function render(status) {
	dock.orientation = status.orientation || 'any';
	const root = $('cameras');
	const items = [...(status.cameras || []).map((cam) => ({ cam, gone: false })), ...(status.departed || []).map((cam) => ({ cam, gone: true }))];
	const seen = new Set();
	items.forEach(({ cam, gone }, index) => {
		const key = (gone ? 'gone:' : 'live:') + cam.name + (gone ? '' : ':' + cam.id);
		seen.add(key);
		let card = dock.cards.get(key);
		if (!card) {
			card = createCard(gone);
			dock.cards.set(key, card);
		}
		updateCard(card, cam);
		if (root.children[index] !== card.root) root.insertBefore(card.root, root.children[index] || null);
	});
	for (const [key, card] of dock.cards) {
		if (!seen.has(key)) {
			card.root.remove();
			dock.cards.delete(key);
		}
	}
	const live = (status.cameras || []).length;
	$('summary').textContent = `${live}/${status.maxClients || '?'}`;
	const empty = $('empty');
	empty.hidden = items.length > 0;
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
		} catch (err) {
			console.warn(err);
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

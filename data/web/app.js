// Camdrop camera page: captures the camera and microphone and sends them to OBS over WebRTC.
'use strict';

const $ = (id) => document.getElementById(id);

const store = {
	get(key, fallback) {
		try {
			const v = localStorage.getItem('camdrop.' + key);
			return v === null ? fallback : v;
		} catch {
			return fallback;
		}
	},
	set(key, value) {
		try {
			localStorage.setItem('camdrop.' + key, String(value));
		} catch {
			/* private mode */
		}
	},
};

const FATAL_ERRORS = {
	bad_code: 'errBadCode',
	rate_limited: 'errRateLimited',
	full: 'errFull',
	name_taken: 'errNameTaken',
	bad_name: 'errBadName',
};

const BYE_REASONS = {
	kicked: 'kicked',
	code_changed: 'codeChanged',
	replaced: 'replaced',
	server_stopped: 'serverStopped',
};

const state = {
	stream: null,
	videoDevices: [],
	facing: store.get('facing', 'environment'),
	deviceId: store.get('deviceId', ''),
	running: false,
	ws: null,
	pc: null,
	videoSender: null,
	audioSender: null,
	welcome: null,
	reconnectAttempt: 0,
	reconnectTimer: null,
	peerTimer: null,
	telemetryTimer: null,
	wakeLock: null,
	noSleep: null,
	battery: null,
	pressure: null,
	lastOutbound: null,
	muted: false,
	economy: false,
	serverStats: null,
	localStats: {},
	clientId: store.get('clientId', ''),
};

if (!state.clientId) {
	state.clientId = (crypto.randomUUID ? crypto.randomUUID() : String(Math.random()).slice(2)).replace(/[^a-zA-Z0-9-]/g, '');
	store.set('clientId', state.clientId);
}

// ---------------------------------------------------------------------------------------------
// Setup screen

function showSetupError(message) {
	const el = $('setup-error');
	el.textContent = message || '';
	el.hidden = !message;
}

function videoConstraints() {
	const height = Number($('quality').value) || 720;
	const fps = Number($('fps').value) || 30;
	const width = Math.round((height * 16) / 9);
	const c = {
		width: { ideal: width },
		height: { ideal: height },
		frameRate: { ideal: fps, max: fps },
	};
	if (state.deviceId) c.deviceId = { exact: state.deviceId };
	else c.facingMode = { ideal: state.facing };
	return c;
}

function audioConstraints() {
	if (!$('mic').checked) return false;
	const processing = $('processing').checked;
	return {
		echoCancellation: processing,
		noiseSuppression: processing,
		autoGainControl: processing,
	};
}

// Test mode (?test=clock): replaces the camera by a picture of the current time encoded as 32
// black/white blocks, so that the latency up to OBS can be measured on the same machine.
function clockStream() {
	const height = Number($('quality').value) || 720;
	const canvas = document.createElement('canvas');
	canvas.width = Math.round((height * 16) / 9);
	canvas.height = height;
	const ctx = canvas.getContext('2d');
	const draw = () => {
		const now = Date.now();
		const value = now % 4294967296;
		ctx.fillStyle = '#404040';
		ctx.fillRect(0, 0, canvas.width, canvas.height);
		const block = Math.floor(canvas.width / 32);
		for (let i = 0; i < 32; i++) {
			const bit = Math.floor(value / 2 ** (31 - i)) % 2;
			ctx.fillStyle = bit ? '#ffffff' : '#000000';
			ctx.fillRect(i * block, 0, block, Math.floor(canvas.height / 6));
		}
		ctx.fillStyle = '#ffffff';
		ctx.font = `${Math.floor(canvas.height / 8)}px monospace`;
		ctx.fillText(String(now), 20, canvas.height / 2);
	};
	draw();
	setInterval(draw, 1000 / (Number($('fps').value) || 30));
	return canvas.captureStream(Number($('fps').value) || 30);
}

async function openCamera() {
	if (state.stream) state.stream.getTracks().forEach((tr) => tr.stop());
	state.stream = null;
	let stream;
	try {
		if (new URLSearchParams(location.search).get('test') === 'clock') {
			stream = clockStream();
			const mic = audioConstraints();
			if (mic) (await navigator.mediaDevices.getUserMedia({ audio: mic })).getAudioTracks().forEach((tr) => stream.addTrack(tr));
		} else {
			stream = await navigator.mediaDevices.getUserMedia({ video: videoConstraints(), audio: audioConstraints() });
		}
	} catch (err) {
		if (state.deviceId && err && err.name === 'OverconstrainedError') {
			state.deviceId = '';
			return openCamera();
		}
		if (err && (err.name === 'NotAllowedError' || err.name === 'SecurityError')) throw new Error(t('errPermission'));
		throw new Error(t('errCamera', { msg: (err && (err.message || err.name)) || err }));
	}
	state.stream = stream;
	const video = stream.getVideoTracks()[0];
	if (video) {
		try {
			video.contentHint = 'motion';
		} catch {
			/* not supported */
		}
		const settings = video.getSettings();
		if (settings.facingMode) state.facing = settings.facingMode;
	}
	const audio = stream.getAudioTracks()[0];
	if (audio) audio.enabled = !state.muted;

	$('preview').srcObject = stream;
	$('live-preview').srcObject = stream;
	$('preview-empty').hidden = true;
	await refreshDevices();
	await replaceSenderTracks();
}

async function refreshDevices() {
	let devices = [];
	try {
		devices = (await navigator.mediaDevices.enumerateDevices()).filter((d) => d.kind === 'videoinput');
	} catch {
		/* ignore */
	}
	state.videoDevices = devices;
	const select = $('camera');
	const current = state.stream && state.stream.getVideoTracks()[0] ? state.stream.getVideoTracks()[0].getSettings().deviceId : '';
	select.innerHTML = '';
	devices.forEach((d, i) => {
		const opt = document.createElement('option');
		opt.value = d.deviceId;
		let label = d.label || t('cameraN', { n: i + 1 });
		if (/front|user|avant/i.test(label) && !/camera/i.test(label)) label = t('front');
		opt.textContent = label;
		select.appendChild(opt);
	});
	if (current) select.value = current;
	$('flip').disabled = devices.length < 2 && !('facingMode' in (navigator.mediaDevices.getSupportedConstraints() || {}));
}

async function replaceSenderTracks() {
	if (!state.stream) return;
	const video = state.stream.getVideoTracks()[0] || null;
	const audio = state.stream.getAudioTracks()[0] || null;
	try {
		if (state.videoSender) await state.videoSender.replaceTrack(video);
		if (state.audioSender) await state.audioSender.replaceTrack(audio);
	} catch {
		// Track kind changed (microphone toggled): renegotiate.
		if (state.running) restartPeer(0);
	}
	if (state.running && ((audio && !state.audioSender) || (!audio && state.audioSender))) restartPeer(0);
}

async function reopenCamera() {
	try {
		showSetupError('');
		await openCamera();
	} catch (err) {
		showSetupError(err.message);
	}
}

// ---------------------------------------------------------------------------------------------
// Live session

function send(message) {
	if (state.ws && state.ws.readyState === WebSocket.OPEN) state.ws.send(JSON.stringify(message));
}

function setStatus(key, kind) {
	$('state-text').textContent = t(key);
	const pill = $('state-pill');
	pill.classList.toggle('live', kind === 'live');
	pill.classList.toggle('bad', kind === 'bad');
	$('economy-status').textContent = t(key);
}

async function start(event) {
	if (event) event.preventDefault();
	showSetupError('');
	const code = $('code').value.trim();
	const name = $('name').value.trim();
	if (!name) return showSetupError(t('errBadName'));
	if (!code) return showSetupError(t('errBadCode'));
	store.set('code', code);
	store.set('name', name);
	store.set('quality', $('quality').value);
	store.set('fps', $('fps').value);
	store.set('mic', $('mic').checked ? '1' : '0');
	store.set('processing', $('processing').checked ? '1' : '0');

	if (!state.stream) {
		try {
			await openCamera();
		} catch (err) {
			return showSetupError(err.message);
		}
	}

	state.running = true;
	state.reconnectAttempt = 0;
	$('setup').hidden = true;
	$('live').hidden = false;
	$('live-name').textContent = name;
	setStatus('stConnecting');
	requestWakeLock(); // needs the user gesture of the Start button
	startTelemetry();
	connect();
}

function stop(messageKey, vars) {
	state.running = false;
	clearTimeout(state.reconnectTimer);
	clearTimeout(state.peerTimer);
	stopTelemetry();
	send({ type: 'bye' });
	closePeer();
	if (state.ws) {
		state.ws.onclose = null;
		try {
			state.ws.close();
		} catch {
			/* ignore */
		}
	}
	state.ws = null;
	releaseWakeLock();
	setEconomy(false);
	$('live').hidden = true;
	$('setup').hidden = false;
	showSetupError(messageKey ? t(messageKey, vars) : '');
}

function connect() {
	if (!state.running) return;
	clearTimeout(state.reconnectTimer);
	const ws = new WebSocket(`wss://${location.host}/ws`);
	state.ws = ws;
	let welcomed = false;

	ws.onopen = () => {
		ws.send(
			JSON.stringify({
				type: 'hello',
				code: store.get('code', ''),
				name: store.get('name', ''),
				clientId: state.clientId,
			}),
		);
	};

	ws.onmessage = async (event) => {
		let msg;
		try {
			msg = JSON.parse(event.data);
		} catch {
			return;
		}
		switch (msg.type) {
			case 'welcome':
				welcomed = true;
				state.welcome = msg;
				state.reconnectAttempt = 0;
				await applyServerVideoSettings(msg.video || {});
				await createPeer();
				break;
			case 'answer':
				if (state.pc) {
					try {
						await state.pc.setRemoteDescription({ type: 'answer', sdp: msg.sdp });
					} catch (err) {
						console.warn('answer', err);
						restartPeer(1000);
					}
				}
				break;
			case 'candidate':
				if (state.pc && msg.candidate && msg.candidate.candidate) {
					try {
						await state.pc.addIceCandidate(msg.candidate);
					} catch (err) {
						console.warn('candidate', err);
					}
				}
				break;
			case 'peer-state':
				if (msg.state === 'failed') {
					setStatus('errWebrtc', 'bad');
					restartPeer(1500);
				}
				break;
			case 'stats':
				state.serverStats = msg;
				renderStats();
				break;
			case 'identify':
				flash();
				break;
			case 'error':
				if (FATAL_ERRORS[msg.code]) {
					stop(FATAL_ERRORS[msg.code], { s: msg.retryAfter || 60 });
				}
				break;
			case 'bye':
				if (BYE_REASONS[msg.reason]) stop(BYE_REASONS[msg.reason]);
				break;
		}
	};

	ws.onclose = () => {
		if (state.ws !== ws) return;
		state.ws = null;
		closePeer();
		if (!state.running) return;
		setStatus(welcomed ? 'stReconnecting' : 'errServer', 'bad');
		scheduleReconnect();
	};
}

// OBS decides the default resolution and frame rate; a choice made on the phone wins.
async function applyServerVideoSettings(video) {
	let changed = false;
	const apply = (id, value) => {
		if (!value || store.get(id + '.user', '') === '1') return;
		const select = $(id);
		if (select.value === String(value) || ![...select.options].some((o) => o.value === String(value))) return;
		select.value = String(value);
		changed = true;
	};
	apply('quality', video.height);
	apply('fps', video.fps);
	if (changed) {
		try {
			await openCamera();
		} catch (err) {
			console.warn('camera', err);
		}
	}
}

function scheduleReconnect() {
	clearTimeout(state.reconnectTimer);
	const delay = Math.min(8000, 500 * 2 ** state.reconnectAttempt);
	state.reconnectAttempt++;
	state.reconnectTimer = setTimeout(connect, delay);
}

function closePeer() {
	clearTimeout(state.peerTimer);
	if (state.pc) {
		state.pc.onconnectionstatechange = null;
		state.pc.onicecandidate = null;
		try {
			state.pc.close();
		} catch {
			/* ignore */
		}
	}
	state.pc = null;
	state.videoSender = null;
	state.audioSender = null;
	state.lastOutbound = null;
}

function restartPeer(delay) {
	closePeer();
	clearTimeout(state.peerTimer);
	state.peerTimer = setTimeout(() => {
		if (state.running && state.ws && state.ws.readyState === WebSocket.OPEN) createPeer();
	}, delay);
}

function orderedCodecs(kind, preferred) {
	let caps = null;
	try {
		caps = RTCRtpSender.getCapabilities(kind);
	} catch {
		/* old browser */
	}
	if (!caps || !caps.codecs) return null;
	if (kind === 'audio') return caps.codecs.filter((c) => /opus/i.test(c.mimeType));
	const h264 = caps.codecs
		.filter((c) => /h264/i.test(c.mimeType) && /packetization-mode=1/.test(c.sdpFmtpLine || ''))
		.sort((a, b) => {
			const score = (c) => (/profile-level-id=42e0/i.test(c.sdpFmtpLine) ? 0 : /profile-level-id=420/i.test(c.sdpFmtpLine) ? 1 : 2);
			return score(a) - score(b);
		});
	const vp8 = caps.codecs.filter((c) => /vp8/i.test(c.mimeType));
	const rtx = caps.codecs.filter((c) => /rtx/i.test(c.mimeType));
	return preferred === 'vp8' ? [...vp8, ...h264, ...rtx] : [...h264, ...vp8, ...rtx];
}

function setPreferences(transceiver, kind) {
	if (!transceiver.setCodecPreferences) return;
	const preferred = state.welcome && state.welcome.video ? state.welcome.video.codec : 'h264';
	const codecs = orderedCodecs(kind, preferred);
	if (!codecs || !codecs.length) return;
	try {
		transceiver.setCodecPreferences(codecs);
	} catch (err) {
		console.warn('setCodecPreferences', err);
	}
}

async function createPeer() {
	closePeer();
	if (!state.stream) return;
	const video = state.welcome && state.welcome.video ? state.welcome.video : {};
	const maxBitrate = (video.maxBitrateKbps || 5000) * 1000;
	const fps = Number($('fps').value) || video.fps || 30;

	const pc = new RTCPeerConnection({ iceServers: [], bundlePolicy: 'max-bundle', rtcpMuxPolicy: 'require' });
	state.pc = pc;

	const videoTrack = state.stream.getVideoTracks()[0];
	const audioTrack = state.stream.getAudioTracks()[0];
	if (videoTrack) {
		const tr = pc.addTransceiver(videoTrack, {
			direction: 'sendonly',
			streams: [state.stream],
			sendEncodings: [{ maxBitrate, maxFramerate: fps, priority: 'high', networkPriority: 'high' }],
		});
		setPreferences(tr, 'video');
		state.videoSender = tr.sender;
	}
	if (audioTrack) {
		const tr = pc.addTransceiver(audioTrack, { direction: 'sendonly', streams: [state.stream] });
		setPreferences(tr, 'audio');
		state.audioSender = tr.sender;
	}

	pc.onicecandidate = (e) => {
		if (e.candidate) send({ type: 'candidate', candidate: e.candidate.toJSON() });
	};
	pc.onconnectionstatechange = () => {
		if (state.pc !== pc) return;
		switch (pc.connectionState) {
			case 'connected':
				setStatus('stLive', 'live');
				tuneSender(maxBitrate, fps);
				break;
			case 'disconnected':
				setStatus('stUnstable', 'bad');
				break;
			case 'failed':
				setStatus('errWebrtc', 'bad');
				restartPeer(1500);
				break;
		}
	};

	const offer = await pc.createOffer();
	await pc.setLocalDescription(offer);
	send({ type: 'offer', sdp: pc.localDescription.sdp });
	setStatus('stConnecting');
}

async function tuneSender(maxBitrate, fps) {
	const sender = state.videoSender;
	if (!sender || !sender.getParameters) return;
	try {
		const p = sender.getParameters();
		if (!p.encodings || !p.encodings.length) p.encodings = [{}];
		p.encodings[0].maxBitrate = maxBitrate;
		p.encodings[0].maxFramerate = fps;
		// 'balanced' starts at full resolution; 'maintain-framerate' starts low and needs ~10 s to climb.
		p.degradationPreference = 'balanced';
		await sender.setParameters(p);
	} catch (err) {
		console.warn('setParameters', err);
	}
}

// ---------------------------------------------------------------------------------------------
// Screen wake lock (with a fallback for browsers without the API)

async function requestWakeLock() {
	if (!state.running) return;
	if ('wakeLock' in navigator) {
		try {
			state.wakeLock = await navigator.wakeLock.request('screen');
			state.wakeLock.addEventListener('release', () => {
				state.wakeLock = null;
			});
			return;
		} catch (err) {
			console.warn('wakeLock', err);
		}
	}
	enableNoSleep();
}

function enableNoSleep() {
	// "NoSleep" technique: a tiny muted video playing in a loop keeps the screen on.
	if (state.noSleep) {
		state.noSleep.play().catch(() => {});
		return;
	}
	const canvas = document.createElement('canvas');
	canvas.width = canvas.height = 2;
	const ctx = canvas.getContext('2d');
	let tick = 0;
	setInterval(() => {
		ctx.fillStyle = tick++ % 2 ? '#000' : '#010101';
		ctx.fillRect(0, 0, 2, 2);
	}, 1000);
	const v = document.createElement('video');
	v.muted = true;
	v.playsInline = true;
	v.setAttribute('playsinline', '');
	v.loop = true;
	v.style.cssText = 'position:fixed;width:1px;height:1px;opacity:0.01;pointer-events:none;bottom:0;left:0';
	v.srcObject = canvas.captureStream ? canvas.captureStream(1) : null;
	document.body.appendChild(v);
	v.play().catch(() => {});
	state.noSleep = v;
}

function releaseWakeLock() {
	if (state.wakeLock) state.wakeLock.release().catch(() => {});
	state.wakeLock = null;
	if (state.noSleep) state.noSleep.pause();
}

document.addEventListener('visibilitychange', () => {
	const hidden = document.visibilityState !== 'visible';
	send({ type: 'visibility', hidden });
	if (!hidden && state.running) {
		// The browser drops the wake lock when the page is hidden: take it again.
		requestWakeLock();
		if (!state.ws) connect();
	}
	updateWarnings();
});

// ---------------------------------------------------------------------------------------------
// Telemetry (every 5 s through the signaling WebSocket)

function startTelemetry() {
	stopTelemetry();
	if (navigator.getBattery && !state.battery) {
		navigator.getBattery().then((b) => {
			state.battery = b;
			b.addEventListener('levelchange', updateWarnings);
			b.addEventListener('chargingchange', updateWarnings);
		}).catch(() => {});
	}
	if ('PressureObserver' in window && !state.pressureObserver) {
		try {
			state.pressureObserver = new PressureObserver((records) => {
				state.pressure = records[records.length - 1].state;
				updateWarnings();
			});
			state.pressureObserver.observe('cpu', { sampleInterval: 2000 }).catch(() => {});
		} catch {
			/* not available */
		}
	}
	state.telemetryTimer = setInterval(sendTelemetry, 5000);
	setTimeout(sendTelemetry, 1500);
}

function stopTelemetry() {
	clearInterval(state.telemetryTimer);
	state.telemetryTimer = null;
}

async function collectStats() {
	const out = {};
	if (!state.pc) return out;
	let report;
	try {
		report = await state.pc.getStats();
	} catch {
		return out;
	}
	const byId = new Map();
	report.forEach((s) => byId.set(s.id, s));
	report.forEach((s) => {
		if (s.type === 'outbound-rtp' && s.kind === 'video') {
			out.fps = s.framesPerSecond || 0;
			out.width = s.frameWidth || 0;
			out.height = s.frameHeight || 0;
			out.framesEncoded = s.framesEncoded || 0;
			out.qualityLimitation = s.qualityLimitationReason || 'none';
			out.cpuLimited = s.qualityLimitationReason === 'cpu' ? 1 : 0;
			if (s.encoderImplementation) out.encoder = s.encoderImplementation;
			if (s.targetBitrate) out.targetKbps = Math.round(s.targetBitrate / 1000);
			const codec = byId.get(s.codecId);
			if (codec && codec.mimeType) out.codec = codec.mimeType.replace(/^video\//i, '');
			if (state.lastOutbound) {
				const dt = (s.timestamp - state.lastOutbound.timestamp) / 1000;
				if (dt > 0) out.kbps = Math.round(((s.bytesSent - state.lastOutbound.bytesSent) * 8) / 1000 / dt);
			}
			state.lastOutbound = { timestamp: s.timestamp, bytesSent: s.bytesSent };
		}
		if (s.type === 'remote-inbound-rtp' && s.kind === 'video') {
			if (s.roundTripTime !== undefined) out.rtt = Math.round(s.roundTripTime * 1000);
			if (s.jitter !== undefined) out.jitter = Math.round(s.jitter * 1000);
			if (s.fractionLost !== undefined) out.lossPct = Math.round(s.fractionLost * 1000) / 10;
		}
	});
	return out;
}

async function sendTelemetry() {
	if (!state.running) return;
	const stats = await collectStats();
	const msg = {
		type: 'telemetry',
		...stats,
		visible: document.visibilityState === 'visible',
		wakeLock: !!state.wakeLock || !!(state.noSleep && !state.noSleep.paused),
		economy: state.economy,
		muted: state.muted,
		facing: state.facing || '',
	};
	if (state.battery) {
		msg.battery = Math.round(state.battery.level * 100);
		msg.charging = !!state.battery.charging;
	}
	if (state.pressure) msg.pressure = state.pressure;
	const track = state.stream && state.stream.getVideoTracks()[0];
	if (track && track.label) msg.camera = track.label.slice(0, 60);
	state.localStats = stats;
	send(msg);
	updateWarnings();
	renderStats();
}

function renderStats() {
	const s = state.serverStats;
	const l = state.localStats || {};
	if (!s && !l.fps) {
		$('stats').textContent = '';
		return;
	}
	const lines = [];
	lines.push(
		t('statsLine1', {
			w: (s && s.width) || l.width || '?',
			h: (s && s.height) || l.height || '?',
			fps: Math.round((s && s.fps) || l.fps || 0),
			kbps: Math.round((s && s.kbps) || l.kbps || 0),
		}),
	);
	if (s) {
		const rtt = s.rttMs || l.rtt || 0;
		lines.push(t('statsLine2', { rtt, loss: (s.lossPct || 0).toFixed(1), buffer: s.bufferMs || 0 }));
		lines.push(t('statsLine3', { latency: Math.round(rtt / 2 + (s.delayMs || 0)), watchers: s.watchers || 0 }));
	}
	$('stats').textContent = lines.join('\n');
}

function updateWarnings() {
	const warnings = [];
	if (document.visibilityState !== 'visible') warnings.push(t('warnHidden'));
	if (state.battery && !state.battery.charging && state.battery.level < 0.2)
		warnings.push(t('warnBattery', { level: Math.round(state.battery.level * 100) }));
	if ((state.localStats && state.localStats.qualityLimitation === 'cpu') || state.pressure === 'serious' || state.pressure === 'critical')
		warnings.push(t('warnHot'));
	if (state.running && !state.wakeLock && !(state.noSleep && !state.noSleep.paused)) warnings.push(t('warnWakeLock'));
	const el = $('warning');
	el.innerHTML = '';
	warnings.forEach((w) => {
		const span = document.createElement('span');
		span.textContent = w;
		el.appendChild(span);
	});
	el.hidden = warnings.length === 0;
}

// ---------------------------------------------------------------------------------------------
// Live controls

function setEconomy(on) {
	state.economy = on;
	$('economy-screen').hidden = !on;
}

function toggleMute() {
	state.muted = !state.muted;
	if (state.stream) state.stream.getAudioTracks().forEach((tr) => (tr.enabled = !state.muted));
	$('mute').textContent = t(state.muted ? 'unmute' : 'mute');
}

function flash() {
	const el = $('identify');
	el.hidden = false;
	setTimeout(() => (el.hidden = true), 2600);
}

async function flipCamera() {
	if (state.videoDevices.length > 1) {
		const current = state.stream && state.stream.getVideoTracks()[0] ? state.stream.getVideoTracks()[0].getSettings().deviceId : '';
		const idx = state.videoDevices.findIndex((d) => d.deviceId === current);
		state.deviceId = state.videoDevices[(idx + 1) % state.videoDevices.length].deviceId;
	} else {
		state.deviceId = '';
		state.facing = state.facing === 'user' ? 'environment' : 'user';
	}
	store.set('deviceId', state.deviceId);
	store.set('facing', state.facing);
	await reopenCamera();
}

// ---------------------------------------------------------------------------------------------
// Init

function init() {
	applyTranslations();

	const params = new URLSearchParams(location.search);
	const urlCode = params.get('code');
	if (urlCode) {
		store.set('code', urlCode);
		// Do not leave the code in the address bar (screenshots, history).
		params.delete('code');
		const rest = params.toString();
		history.replaceState(null, '', location.pathname + (rest ? '?' + rest : ''));
	}
	$('code').value = store.get('code', '');
	$('name').value = params.get('name') || store.get('name', '');
	$('quality').value = store.get('quality', '720');
	$('fps').value = store.get('fps', '30');
	$('mic').checked = store.get('mic', '1') === '1';
	$('processing').checked = store.get('processing', '0') === '1';
	$('mute').textContent = t('mute');

	$('setup-form').addEventListener('submit', start);
	$('camera').addEventListener('change', () => {
		state.deviceId = $('camera').value;
		store.set('deviceId', state.deviceId);
		reopenCamera();
	});
	$('flip').addEventListener('click', flipCamera);
	['quality', 'fps'].forEach((id) => $(id).addEventListener('change', () => store.set(id + '.user', '1')));
	['quality', 'fps', 'mic', 'processing'].forEach((id) => $(id).addEventListener('change', reopenCamera));
	$('stop').addEventListener('click', () => stop());
	$('mute').addEventListener('click', toggleMute);
	$('economy').addEventListener('click', () => setEconomy(true));
	$('economy-screen').addEventListener('click', () => setEconomy(false));
	window.addEventListener('pagehide', () => send({ type: 'bye' }));

	if (!window.isSecureContext) {
		showSetupError(t('errInsecure'));
		$('start').disabled = true;
		return;
	}
	if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia || !window.RTCPeerConnection) {
		showSetupError(t('errNoMedia'));
		$('start').disabled = true;
		return;
	}
	reopenCamera();
	if (params.get('autostart') === '1' && $('code').value && $('name').value) start();
}

init();

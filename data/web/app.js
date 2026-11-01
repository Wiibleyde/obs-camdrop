// Camdrop camera page: captures the camera (or the screen) and the microphone and sends them to OBS
// over WebRTC.
'use strict';

const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);
const TEST_CLOCK = params.get('test') === 'clock';

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

// Special entries of the video source and microphone lists.
const SCREEN = '__screen__';
const NO_VIDEO = '__none__';
const NO_MIC = '__off__';

const canShareScreen = !!(navigator.mediaDevices && navigator.mediaDevices.getDisplayMedia);

const state = {
	stream: null,
	videoDevices: [],
	audioDevices: [],
	videoMode: store.get('videoMode', 'camera') === 'none' ? 'none' : 'camera', // 'camera' | 'screen' | 'none'
	facing: store.get('facing', 'environment'),
	deviceId: store.get('deviceId', ''),
	micId: store.get('micId', store.get('mic', '1') === '0' ? NO_MIC : ''),
	screenTrack: null,
	clockTimer: null,
	zoom: null,
	torch: false,
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
	tally: 'off',
	tallyEnabled: true,
	orientation: 'any', // requested by OBS: 'any' | 'landscape' | 'portrait'
	clientId: store.get('clientId', ''),
};

if (!state.clientId) {
	state.clientId = (crypto.randomUUID ? crypto.randomUUID() : String(Math.random()).slice(2)).replace(/[^a-zA-Z0-9-]/g, '');
	store.set('clientId', state.clientId);
}

const videoTrack = () => (state.stream && state.stream.getVideoTracks()[0]) || null;
const audioTrack = () => (state.stream && state.stream.getAudioTracks()[0]) || null;
// Phones and tablets (a touch screen laptop keeps a fine primary pointer).
const isMobile = () => window.matchMedia('(pointer: coarse)').matches;

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
	if (state.micId === NO_MIC) return false;
	const processing = $('processing').checked;
	const c = {
		echoCancellation: processing,
		noiseSuppression: processing,
		autoGainControl: processing,
	};
	if (state.micId) c.deviceId = { exact: state.micId };
	return c;
}

// Test mode (?test=clock): replaces the camera by a picture of the current time encoded as 32
// black/white blocks, so that the latency up to OBS can be measured on the same machine.
function clockTrack() {
	const height = Number($('quality').value) || 720;
	const fps = Number($('fps').value) || 30;
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
	clearInterval(state.clockTimer);
	state.clockTimer = setInterval(draw, 1000 / fps);
	return canvas.captureStream(fps).getVideoTracks()[0];
}

async function openScreen() {
	const fps = Number($('fps').value) || 30;
	const height = Number($('quality').value) || 720;
	const s = await navigator.mediaDevices.getDisplayMedia({
		video: { frameRate: { ideal: fps, max: fps }, height: { max: height } },
		audio: false,
	});
	const track = s.getVideoTracks()[0];
	try {
		track.contentHint = 'detail';
	} catch {
		/* not supported */
	}
	// "Stop sharing" in the browser: back to the camera.
	track.addEventListener('ended', () => {
		if (state.screenTrack !== track) return;
		state.screenTrack = null;
		state.videoMode = 'camera';
		reopenCamera();
	});
	state.screenTrack = track;
	return track;
}

function stopStream(keep) {
	if (state.stream) state.stream.getTracks().forEach((tr) => tr !== keep && tr.stop());
	state.stream = null;
}

async function openMedia() {
	const keepScreen = state.videoMode === 'screen' && state.screenTrack && state.screenTrack.readyState === 'live' ? state.screenTrack : null;
	stopStream(keepScreen);
	if (state.videoMode !== 'screen' && state.screenTrack) {
		state.screenTrack.stop();
		state.screenTrack = null;
	}
	const audio = audioConstraints();
	let stream;
	try {
		if (state.videoMode === 'camera' && !TEST_CLOCK) {
			stream = await navigator.mediaDevices.getUserMedia({ video: videoConstraints(), audio });
		} else {
			stream = new MediaStream();
			if (state.videoMode === 'screen') stream.addTrack(keepScreen || (await openScreen()));
			else if (state.videoMode === 'camera') stream.addTrack(clockTrack());
			if (audio) (await navigator.mediaDevices.getUserMedia({ audio })).getAudioTracks().forEach((tr) => stream.addTrack(tr));
		}
	} catch (err) {
		const name = err && err.name;
		if (name === 'OverconstrainedError' && state.deviceId) {
			state.deviceId = '';
			return openMedia();
		}
		if (name === 'OverconstrainedError' && state.micId && state.micId !== NO_MIC) {
			state.micId = '';
			return openMedia();
		}
		if (state.videoMode === 'screen') {
			// Sharing cancelled or refused: back to the camera.
			state.videoMode = 'camera';
			await openMedia().catch(() => {});
			throw new Error(t('errScreen'));
		}
		if (name === 'NotAllowedError' || name === 'SecurityError') throw new Error(t('errPermission'));
		throw new Error(t('errCamera', { msg: (err && (err.message || err.name)) || err }));
	}
	state.stream = stream;

	const video = videoTrack();
	if (video && state.videoMode === 'camera') {
		try {
			video.contentHint = 'motion';
		} catch {
			/* not supported */
		}
		const settings = video.getSettings();
		if (settings.facingMode) state.facing = settings.facingMode;
		await restoreCameraControls(video);
	}
	const mic = audioTrack();
	if (mic) mic.enabled = !state.muted;

	$('preview').srcObject = stream;
	$('live-preview').srcObject = stream;
	$('preview-empty').hidden = !!video;
	$('preview-empty').textContent = t(state.videoMode === 'none' ? 'audioOnly' : 'previewEmpty');
	$('live-audio-only').hidden = !!video;
	await refreshDevices();
	await replaceSenderTracks();
	updateOrientationHint();
	sendCaps();
}

// Zoom and torch are lost when the camera is reopened (quality change, other lens): apply them again.
async function restoreCameraControls(track) {
	const caps = track.getCapabilities ? track.getCapabilities() : {};
	const advanced = {};
	if (state.zoom !== null && caps.zoom) advanced.zoom = Math.min(caps.zoom.max, Math.max(caps.zoom.min, state.zoom));
	if (state.torch && caps.torch) advanced.torch = true;
	if (Object.keys(advanced).length) await track.applyConstraints({ advanced: [advanced] }).catch(() => {});
}

function addOption(select, value, label) {
	const opt = document.createElement('option');
	opt.value = value;
	opt.textContent = label;
	select.appendChild(opt);
}

async function refreshDevices() {
	let devices = [];
	try {
		devices = await navigator.mediaDevices.enumerateDevices();
	} catch {
		/* ignore */
	}
	state.videoDevices = devices.filter((d) => d.kind === 'videoinput' && d.deviceId);
	state.audioDevices = devices.filter((d) => d.kind === 'audioinput' && d.deviceId && d.deviceId !== 'default' && d.deviceId !== 'communications');

	const camera = $('camera');
	const video = videoTrack();
	const current = video && state.videoMode === 'camera' ? video.getSettings().deviceId : '';
	camera.innerHTML = '';
	state.videoDevices.forEach((d, i) => {
		let label = d.label || t('cameraN', { n: i + 1 });
		if (/front|user|avant/i.test(label) && !/camera/i.test(label)) label = t('front');
		addOption(camera, d.deviceId, label);
	});
	if (!state.videoDevices.length) addOption(camera, '', t('cameraN', { n: 1 }));
	if (canShareScreen) addOption(camera, SCREEN, t('shareScreen'));
	addOption(camera, NO_VIDEO, t('noVideo'));
	camera.value = state.videoMode === 'screen' ? SCREEN : state.videoMode === 'none' ? NO_VIDEO : current || camera.options[0].value;

	const mic = $('mic');
	mic.innerHTML = '';
	addOption(mic, '', t('micDefault'));
	state.audioDevices.forEach((d, i) => addOption(mic, d.deviceId, d.label || t('micN', { n: i + 1 })));
	addOption(mic, NO_MIC, t('noMic'));
	mic.value = [...mic.options].some((o) => o.value === state.micId) ? state.micId : '';

	$('flip').disabled = state.videoMode !== 'camera' || (state.videoDevices.length < 2 && !('facingMode' in (navigator.mediaDevices.getSupportedConstraints() || {})));
}

async function replaceSenderTracks() {
	if (!state.stream) return;
	const video = videoTrack();
	const audio = audioTrack();
	try {
		if (state.videoSender && video) await state.videoSender.replaceTrack(video);
		if (state.audioSender && audio) await state.audioSender.replaceTrack(audio);
	} catch {
		if (state.running) restartPeer(0);
		return;
	}
	// A track appeared or disappeared (microphone or video turned on/off): renegotiate.
	const changed = !!video !== !!state.videoSender || !!audio !== !!state.audioSender;
	if (state.running && state.pc && changed) {
		restartPeer(0);
		return;
	}
	if (state.videoSender) tuneSender();
}

async function reopenCamera() {
	try {
		showSetupError('');
		await openMedia();
	} catch (err) {
		showSetupError(err.message);
		if (state.running) showToast(err.message);
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

function showToast(message) {
	const el = $('toast');
	el.textContent = message;
	el.hidden = false;
	clearTimeout(showToast.timer);
	showToast.timer = setTimeout(() => (el.hidden = true), 4000);
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
	store.set('processing', $('processing').checked ? '1' : '0');

	if (!state.stream) {
		try {
			await openMedia();
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
	unlockOrientation();
	setEconomy(false);
	setTally('off');
	$('live').hidden = true;
	$('setup').hidden = false;
	showSetupError(messageKey ? t(messageKey, vars) : '');
	updateOrientationHint();
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
				applyConfig({ orientation: (msg.video || {}).orientation, tally: msg.tally });
				lockOrientation();
				await applyServerVideoSettings(msg.video || {});
				sendCaps();
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
			case 'tally':
				setTally(msg.state);
				break;
			case 'config':
				applyConfig(msg);
				break;
			case 'control':
				await applyControl(msg.set || {});
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
		setTally('off');
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
			await openMedia();
		} catch (err) {
			console.warn('camera', err);
		}
	}
}

function applyConfig(config) {
	if (config.orientation) state.orientation = config.orientation;
	if (typeof config.tally === 'boolean') {
		state.tallyEnabled = config.tally;
		setTally(state.tally);
	}
	updateOrientationHint();
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
	const h265 = caps.codecs
		.filter((c) => /h265|hevc/i.test(c.mimeType))
		.sort((a, b) => (/profile-id=1/.test(a.sdpFmtpLine || '') ? 0 : 1) - (/profile-id=1/.test(b.sdpFmtpLine || '') ? 0 : 1));
	const vp8 = caps.codecs.filter((c) => /vp8/i.test(c.mimeType));
	const rtx = caps.codecs.filter((c) => /rtx/i.test(c.mimeType));
	if (preferred === 'vp8') return [...vp8, ...h264, ...h265, ...rtx];
	if (preferred === 'h265') return [...h265, ...h264, ...vp8, ...rtx];
	return [...h264, ...vp8, ...h265, ...rtx];
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

function senderLimits() {
	const video = state.welcome && state.welcome.video ? state.welcome.video : {};
	return {
		maxBitrate: (video.maxBitrateKbps || 5000) * 1000,
		fps: Number($('fps').value) || video.fps || 30,
	};
}

async function createPeer() {
	closePeer();
	if (!state.stream) return;
	const { maxBitrate, fps } = senderLimits();

	const pc = new RTCPeerConnection({ iceServers: [], bundlePolicy: 'max-bundle', rtcpMuxPolicy: 'require' });
	state.pc = pc;

	const video = videoTrack();
	const audio = audioTrack();
	if (video) {
		const tr = pc.addTransceiver(video, {
			direction: 'sendonly',
			streams: [state.stream],
			sendEncodings: [{ maxBitrate, maxFramerate: fps, priority: 'high', networkPriority: 'high' }],
		});
		setPreferences(tr, 'video');
		state.videoSender = tr.sender;
	}
	if (audio) {
		const tr = pc.addTransceiver(audio, { direction: 'sendonly', streams: [state.stream] });
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
				tuneSender();
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

async function tuneSender() {
	const sender = state.videoSender;
	if (!sender || !sender.getParameters) return;
	const { maxBitrate, fps } = senderLimits();
	try {
		const p = sender.getParameters();
		if (!p.encodings || !p.encodings.length) p.encodings = [{}];
		p.encodings[0].maxBitrate = maxBitrate;
		p.encodings[0].maxFramerate = fps;
		// Camera: 'balanced' starts at full resolution ('maintain-framerate' starts low and needs ~10 s
		// to climb). Screen: keep text sharp.
		p.degradationPreference = state.videoMode === 'screen' ? 'maintain-resolution' : 'balanced';
		await sender.setParameters(p);
	} catch (err) {
		console.warn('setParameters', err);
	}
}

// ---------------------------------------------------------------------------------------------
// Capabilities and remote control (from the OBS dock or obs-websocket)

function collectCaps() {
	const caps = {
		video: state.videoMode,
		devices: state.videoDevices.slice(0, 12).map((d, i) => ({ id: d.deviceId, label: (d.label || t('cameraN', { n: i + 1 })).slice(0, 60) })),
		deviceId: '',
		facing: state.facing || '',
		height: Number($('quality').value) || 0,
		fps: Number($('fps').value) || 0,
		mic: !!audioTrack(),
		muted: state.muted,
	};
	const track = videoTrack();
	if (track && state.videoMode === 'camera' && !TEST_CLOCK) {
		const settings = track.getSettings();
		caps.deviceId = settings.deviceId || '';
		const c = track.getCapabilities ? track.getCapabilities() : {};
		if (c.zoom && c.zoom.max > c.zoom.min) {
			caps.zoom = { min: c.zoom.min, max: c.zoom.max, step: c.zoom.step || 0.1, value: settings.zoom ?? c.zoom.min };
		}
		if (Array.isArray(c.torch) ? c.torch.includes(true) : c.torch) caps.torch = !!settings.torch;
	}
	return caps;
}

function sendCaps() {
	if (state.running) send({ type: 'caps', caps: collectCaps() });
}

async function applyControl(set) {
	const track = videoTrack();
	if (typeof set.zoom === 'number' && track && track.getCapabilities && track.getCapabilities().zoom) {
		const c = track.getCapabilities().zoom;
		state.zoom = Math.min(c.max, Math.max(c.min, set.zoom));
		await track.applyConstraints({ advanced: [{ zoom: state.zoom }] }).catch((err) => console.warn('zoom', err));
	}
	if (typeof set.torch === 'boolean' && track) {
		state.torch = set.torch;
		await track.applyConstraints({ advanced: [{ torch: set.torch }] }).catch((err) => console.warn('torch', err));
	}
	if (typeof set.muted === 'boolean') setMuted(set.muted);

	let reopen = false;
	const choose = (id, value) => {
		const select = $(id);
		if (![...select.options].some((o) => o.value === String(value)) || select.value === String(value)) return;
		select.value = String(value);
		store.set(id, select.value);
		store.set(id + '.user', '1');
		reopen = true;
	};
	if (set.height) choose('quality', set.height);
	if (set.fps) choose('fps', set.fps);
	if (set.video === 'none' && state.videoMode !== 'none') {
		state.videoMode = 'none';
		reopen = true;
	} else if (set.video === 'camera' && state.videoMode === 'none') {
		state.videoMode = 'camera';
		reopen = true;
	}
	if (typeof set.deviceId === 'string' && set.deviceId) {
		state.videoMode = 'camera';
		state.deviceId = set.deviceId;
		state.zoom = null;
		reopen = true;
	}
	if (set.facing) {
		if (set.facing === 'toggle') {
			await flipCamera();
		} else {
			state.videoMode = 'camera';
			state.deviceId = '';
			state.facing = set.facing;
			state.zoom = null;
			reopen = true;
		}
	}
	store.set('videoMode', state.videoMode === 'none' ? 'none' : 'camera');
	store.set('deviceId', state.deviceId);
	store.set('facing', state.facing);
	if (reopen) await reopenCamera();
	sendCaps();
	sendTelemetry();
}

// ---------------------------------------------------------------------------------------------
// Tally light and orientation

function setTally(value) {
	state.tally = value || 'off';
	const shown = state.running && state.tallyEnabled && (state.tally === 'program' || state.tally === 'preview');
	const el = $('tally');
	el.hidden = !shown;
	el.classList.toggle('program', state.tally === 'program');
	el.classList.toggle('preview', state.tally === 'preview');
	$('tally-label').textContent = state.tally === 'program' ? t('tallyProgram') : state.tally === 'preview' ? t('tallyPreview') : '';
}

function screenIsPortrait() {
	if (screen.orientation && screen.orientation.type) return screen.orientation.type.startsWith('portrait');
	return window.matchMedia('(orientation: portrait)').matches;
}

function updateOrientationHint() {
	const want = state.orientation;
	const portrait = screenIsPortrait();
	const wrong = state.running && state.videoMode === 'camera' && isMobile() && ((want === 'landscape' && portrait) || (want === 'portrait' && !portrait));
	const el = $('rotate-hint');
	el.hidden = !wrong;
	if (wrong) el.textContent = t(want === 'landscape' ? 'rotateLandscape' : 'rotatePortrait');
}

// Android browsers can lock the orientation in full screen; other browsers just show the hint.
async function lockOrientation() {
	if (state.orientation === 'any' || !isMobile() || !screen.orientation || !screen.orientation.lock) return;
	try {
		if (!document.fullscreenElement && document.documentElement.requestFullscreen) await document.documentElement.requestFullscreen({ navigationUI: 'hide' });
		await screen.orientation.lock(state.orientation);
	} catch {
		/* not supported (iOS, desktop) */
	}
}

function unlockOrientation() {
	try {
		if (screen.orientation && screen.orientation.unlock) screen.orientation.unlock();
		if (document.fullscreenElement && document.exitFullscreen) document.exitFullscreen().catch(() => {});
	} catch {
		/* ignore */
	}
}

window.addEventListener('resize', updateOrientationHint);
if (screen.orientation && screen.orientation.addEventListener) screen.orientation.addEventListener('change', updateOrientationHint);

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
		navigator
			.getBattery()
			.then((b) => {
				state.battery = b;
				b.addEventListener('levelchange', updateWarnings);
				b.addEventListener('chargingchange', updateWarnings);
			})
			.catch(() => {});
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
		if (s.type === 'remote-inbound-rtp' && (s.kind === 'video' || (!state.videoSender && s.kind === 'audio'))) {
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
		video: state.videoMode,
		orientation: screenIsPortrait() ? 'portrait' : 'landscape',
	};
	if (state.battery) {
		msg.battery = Math.round(state.battery.level * 100);
		msg.charging = !!state.battery.charging;
	}
	if (state.pressure) msg.pressure = state.pressure;
	const track = videoTrack();
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
	if (state.videoMode !== 'none') {
		lines.push(
			t('statsLine1', {
				w: (s && s.width) || l.width || '?',
				h: (s && s.height) || l.height || '?',
				fps: Math.round((s && s.fps) || l.fps || 0),
				kbps: Math.round((s && s.kbps) || l.kbps || 0),
			}),
		);
	}
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
	if (state.battery && !state.battery.charging && state.battery.level < 0.2) warnings.push(t('warnBattery', { level: Math.round(state.battery.level * 100) }));
	if ((state.localStats && state.localStats.qualityLimitation === 'cpu') || state.pressure === 'serious' || state.pressure === 'critical') warnings.push(t('warnHot'));
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

function setMuted(muted) {
	state.muted = muted;
	if (state.stream) state.stream.getAudioTracks().forEach((tr) => (tr.enabled = !state.muted));
	$('mute').textContent = t(state.muted ? 'unmute' : 'mute');
}

function flash() {
	const el = $('identify');
	el.hidden = false;
	setTimeout(() => (el.hidden = true), 2600);
}

async function flipCamera() {
	state.videoMode = 'camera';
	state.zoom = null;
	if (state.videoDevices.length > 1) {
		const current = videoTrack() ? videoTrack().getSettings().deviceId : '';
		const idx = state.videoDevices.findIndex((d) => d.deviceId === current);
		state.deviceId = state.videoDevices[(idx + 1) % state.videoDevices.length].deviceId;
	} else {
		state.deviceId = '';
		state.facing = state.facing === 'user' ? 'environment' : 'user';
	}
	store.set('deviceId', state.deviceId);
	store.set('facing', state.facing);
	store.set('videoMode', 'camera');
	await reopenCamera();
}

async function onVideoSourceChange() {
	const value = $('camera').value;
	state.zoom = null;
	if (value === SCREEN) {
		state.videoMode = 'screen';
	} else if (value === NO_VIDEO) {
		state.videoMode = 'none';
	} else {
		state.videoMode = 'camera';
		state.deviceId = value;
		store.set('deviceId', state.deviceId);
	}
	store.set('videoMode', state.videoMode === 'none' ? 'none' : 'camera');
	await reopenCamera();
}

function onMicChange() {
	state.micId = $('mic').value;
	store.set('micId', state.micId);
	reopenCamera();
}

// ---------------------------------------------------------------------------------------------
// Init

function init() {
	applyTranslations();

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
	$('processing').checked = store.get('processing', '0') === '1';
	$('mute').textContent = t('mute');

	$('setup-form').addEventListener('submit', start);
	$('camera').addEventListener('change', onVideoSourceChange);
	$('mic').addEventListener('change', onMicChange);
	$('flip').addEventListener('click', flipCamera);
	['quality', 'fps'].forEach((id) => $(id).addEventListener('change', () => store.set(id + '.user', '1')));
	['quality', 'fps', 'processing'].forEach((id) => $(id).addEventListener('change', reopenCamera));
	$('stop').addEventListener('click', () => stop());
	$('mute').addEventListener('click', () => {
		setMuted(!state.muted);
		sendCaps();
	});
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

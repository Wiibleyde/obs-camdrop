# Changelog

All notable changes to this project are documented here. Versions follow [SemVer](https://semver.org).

## [0.2.0] - 2026-11-02

- Tally light on the phones (red on air, green in the studio-mode preview) and in the dock.
- Remote control from the dock: camera / lens, zoom, torch, resolution, frame rate, microphone
  mute, video off.
- New cameras get a source in the current scene automatically (fitted to the canvas, hidden while
  streaming or recording, preview scene in studio mode).
- Phone page: screen sharing (desktop browsers), audio-only mode, microphone choice.
- Rotation setting per source; requested orientation with a reminder on the phone (and orientation
  lock on Android).
- H.265 / HEVC reception (RFC 7798) with hardware decoding, 10-bit (Main 10) pixel formats.
- obs-websocket vendor `camdrop`: `GetCameras`, `SetCameraControl`, `RequestKeyframe`, `Identify`,
  `Disconnect`, events `CameraConnected` / `CameraDisconnected`.
- macOS (universal) and Ubuntu 24.04 builds.
- Dock cards are updated in place (controls keep their state between refreshes).

## [0.1.0] - unreleased

First version.

- HTTPS + WebSocket server on a single port (self-signed certificate by default, imported PEM
  certificates reloaded automatically), access code, local network only, rate limiting.
- Camera web page: name, code, camera choice, resolution and frame rate, microphone, screen wake
  lock with fallback, economy mode, live statistics, automatic reconnection, English and French.
- WebRTC reception (libdatachannel): H.264 and VP8, Opus, NACK + RTX retransmissions, PLI, REMB,
  bitrate limit.
- Jitter buffer with adjustable delay per source, audio/video sync from RTCP sender reports.
- FFmpeg decoding with D3D11VA hardware acceleration and software fallback.
- "Remote camera (Camdrop)" OBS source selecting a camera by name, automatic rebind after a
  reconnection.
- Settings dialog (Tools → Camdrop) with QR code, supervision dock (Docks → Camdrop) with alerts.

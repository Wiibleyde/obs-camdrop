# Changelog

All notable changes to this project are documented here. Versions follow [SemVer](https://semver.org).

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

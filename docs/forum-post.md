# OBS forum resource (draft)

**Title**: Camdrop – Use phones & browsers as cameras in OBS (no app, WebRTC, low latency)

**Tagline**: Turn any phone, tablet or laptop on your network into a low-latency OBS camera, straight
from the browser.

**Category**: Plugins · **Supported**: Windows (OBS 32.2+) · macOS and Linux planned

---

Camdrop runs a small HTTPS server inside OBS. Open the address (or scan the QR code) on a phone, type
a name, allow the camera, tap Start: the phone becomes a **Remote camera (Camdrop)** source.

**Highlights**
- No app: Chrome, Safari (iOS 16.4+), Firefox, Edge
- 100–200 ms glass-to-glass on a local network (WebRTC, adjustable buffer per source)
- Several cameras at once, automatic reconnection, audio + video in sync
- Hardware decoding (D3D11VA), NACK/RTX loss recovery
- Supervision dock: network, battery, CPU/heat, page in background, disconnections
- Free and open source (GPL-2.0+): https://github.com/Wiibleyde/obs-camdrop

**Install**: download the zip from the GitHub releases and extract it into
`C:\ProgramData\obs-studio\plugins`, restart OBS, then Tools → Camdrop → Enable the server.

**Good to know**: local network only (no Internet relay); the default certificate is self-signed
(accept the warning once, or follow the HTTPS guide for a real certificate on your own domain).

[GIF: phone → OBS in a few seconds]

**Links**: Releases · HTTPS guide · FAQ · Issues

---

GitHub topics: `obs-plugin`, `obs-studio`, `webrtc`, `phone-camera`, `webcam`

Announcements: r/obs, OBS Discord #plugins channel.

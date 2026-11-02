# OBS forum resource (draft, v0.2.0)

Post at <https://obsproject.com/forum/resources/add> (log in, then pick **OBS Studio Plugins**).

Before posting:

1. The repository is public: the links and screenshots below point to GitHub.
2. The `0.2.0` tag is pushed and its draft release (created by CI) is published.

## Form fields

| Field | Value |
| --- | --- |
| Title | Camdrop – phones & browsers as low-latency OBS cameras (no app, WebRTC) |
| Tag line | Turn phones and browsers on your network into low-latency OBS cameras. No app, WebRTC, multicam. |
| Version | 0.2.0 |
| Download | External URL: `https://github.com/Wiibleyde/obs-camdrop/releases/latest` |
| Source code | `https://github.com/Wiibleyde/obs-camdrop` |
| Supported bit | 64-bit |
| Supported platforms | Windows, macOS, Linux |
| Minimum OBS version | 32.2.0 |

## Description

Switch the editor to BB code mode (the `[ ]` button), then paste:

```bbcode
Camdrop runs a small HTTPS server inside OBS. Open the address it shows (or scan its QR code) on any phone, tablet or laptop on the same network, type a name, allow the camera and tap [B]Start[/B]: the camera becomes an OBS source with 100–200 ms of latency. Nothing to install on the phone.

[IMG]https://raw.githubusercontent.com/Wiibleyde/obs-camdrop/main/docs/images/phone-tally.png[/IMG] [IMG]https://raw.githubusercontent.com/Wiibleyde/obs-camdrop/main/docs/images/dock-controls.png[/IMG]
[IMG]https://raw.githubusercontent.com/Wiibleyde/obs-camdrop/main/docs/images/settings.png[/IMG]

[B][SIZE=5]Features[/SIZE][/B]
[LIST]
[*][B]No app[/B]: Chrome, Safari (iOS 16.4+), Firefox, Edge, on Android, iOS and desktop.
[*][B]Low latency[/B]: WebRTC straight into OBS, adjustable buffer per source, NACK/RTX loss recovery and key-frame requests instead of big buffers.
[*][B]Several cameras at once[/B], picked by name in a [I]Remote camera (Camdrop)[/I] source, with automatic reconnection.
[*][B]Video + audio[/B]: H.264, H.265/HEVC (when the browser offers it), VP8 and Opus, kept in sync. Hardware decoding (D3D11VA, VideoToolbox, VAAPI) with software fallback.
[*][B]Tally light[/B]: the phone shows a red border when its source is on air, a green one when it is in the studio-mode preview.
[*][B]Remote control from OBS[/B]: from the dock, switch the camera or lens, zoom, torch, resolution, frame rate, mute the microphone or turn the video off without touching the phone.
[*][B]New cameras appear by themselves[/B]: a source is added to the current scene, fitted to the canvas (hidden if you are live).
[*][B]Supervision dock[/B]: network, frame rate, delay, battery, CPU/heat and alerts for every camera.
[*][B]Phone side[/B]: front/back camera, screen sharing from desktop browsers, audio only, microphone choice, screen kept awake, economy mode.
[*][B]Rotation[/B] per source, and a landscape/portrait request that reminds the operator to turn the phone.
[*][B]Automation[/B]: obs-websocket vendor requests for Stream Deck, Bitfocus Companion, Touch Portal or scripts.
[*][B]Safe defaults[/B]: server off until enabled, random access code, local network only, no telemetry.
[*]English and French.
[/LIST]

[B][SIZE=5]Requirements[/SIZE][/B]
[LIST]
[*]OBS Studio 32.2 or newer on Windows 10/11 64-bit, macOS 12+ (Apple Silicon and Intel) or Ubuntu 24.04 with OBS from the official PPA.
[*]Phone and PC on the same network (5 GHz Wi-Fi recommended, PC on Ethernet ideally).
[/LIST]

[B][SIZE=5]Installation[/SIZE][/B]
Download the file for your system from the [URL=https://github.com/Wiibleyde/obs-camdrop/releases]releases[/URL], then restart OBS.
[LIST]
[*][B]Windows[/B]: extract the zip into [ICODE]C:\ProgramData\obs-studio\plugins[/ICODE].
[*][B]macOS[/B]: open the .pkg. It is not notarized yet: if macOS blocks it, right-click it and choose [I]Open[/I].
[*][B]Ubuntu[/B]: in the download folder, [ICODE]sudo apt install ./camdrop-*.deb[/ICODE]
[/LIST]

[B][SIZE=5]Quick start[/SIZE][/B]
[LIST=1]
[*][B]Tools → Camdrop[/B]: tick [I]Enable the Camdrop server[/I], click Apply (allow OBS on private networks if Windows asks).
[*]On the phone, scan the QR code.
[*]The first time, accept the certificate warning (self-signed), or follow the HTTPS guide to use your own certificate.
[*]Enter the access code and a name, allow the camera, tap Start: a [I]Camdrop – name[/I] source appears in the current scene.
[*][B]Docks → Camdrop[/B] shows every camera with its alerts and remote controls.
[/LIST]

[B][SIZE=5]Good to know[/SIZE][/B]
[LIST]
[*]Local network only: no relay over the Internet.
[*]The macOS and Linux builds are new: please report anything odd in the issues.
[/LIST]

[URL=https://github.com/Wiibleyde/obs-camdrop]Source code[/URL] · [URL=https://github.com/Wiibleyde/obs-camdrop/releases]Releases[/URL] · [URL=https://github.com/Wiibleyde/obs-camdrop/blob/main/docs/https.md]HTTPS guide[/URL] · [URL=https://github.com/Wiibleyde/obs-camdrop/blob/main/docs/faq.md]FAQ[/URL] · [URL=https://github.com/Wiibleyde/obs-camdrop/issues]Issues[/URL]

Free and open source (GPL-2.0 or later).
```

## Elsewhere

- GitHub topics: `obs-plugin`, `obs-studio`, `webrtc`, `phone-camera`, `webcam`.
- Announcements: r/obs, OBS Discord #plugins channel.

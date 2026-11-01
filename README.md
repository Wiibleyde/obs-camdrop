# Camdrop for OBS

**Use phones and browsers as low-latency cameras in OBS — no app, WebRTC, multicam.**

Camdrop is an OBS Studio plugin that runs a small HTTPS server on your PC. Open the address it shows
(or scan its QR code) on any phone, tablet or computer on the same network, type a name, allow the
camera and microphone, tap **Start**: the camera becomes an OBS source with 100–200 ms of latency.

> 🇫🇷 Documentation en français : [docs/fr/README.md](docs/fr/README.md)

<!-- TODO: demo GIF (phone → OBS) -->

## Features

- **No app to install**: works in Chrome, Safari, Firefox, Edge (Android, iOS 16.4+, desktop).
- **Low latency**: WebRTC straight to the plugin over UDP, configurable jitter buffer per source,
  NACK/RTX retransmissions and key-frame requests instead of big buffers.
- **Several cameras at once**, each selectable by name in a **Remote camera (Camdrop)** source; a
  camera that reconnects is picked up again automatically.
- **Video + audio** (H.264 or VP8, Opus), audio/video synchronised with RTCP sender reports.
- **Hardware decoding** (D3D11VA) with automatic software fallback.
- **Supervision dock**: network, frame rate, delay, battery, CPU pressure and alerts for every camera.
- **Phone side**: front/back camera, resolution and frame rate, screen kept awake (Wake Lock with a
  fallback), economy mode (black screen), mute, live statistics.
- **Safe defaults**: server off until enabled, random access code, local network only, rate-limited
  code attempts, no telemetry.
- English and French.

## Comparison

| | Camdrop | VDO.Ninja | DroidCam / Camo | LensLink |
| --- | --- | --- | --- | --- |
| App to install on the phone | No | No | Yes | Yes (iPhone/iPad) |
| Android **and** iOS | Yes | Yes | Depends on the app | iOS only |
| Video arrives as a native OBS source | Yes | No (browser source) | Yes | Yes |
| Several cameras | Yes | Yes | See each app | See the app |
| Open source | Yes (GPL-2.0+) | Yes | No | Plugin: yes |
| Remote guests over the Internet | No (LAN only) | Yes | No | No |

Based on public information at the time of writing; check each project for details.

## Requirements

- Windows 10/11 64-bit, **OBS Studio 32.2 or newer** (the plugin uses the FFmpeg and libdatachannel
  libraries shipped with OBS; 32.0/32.1 ship an older FFmpeg).
- Phone and PC on the same network (Wi-Fi 5 GHz recommended, PC on Ethernet ideally).
- macOS and Linux builds will come after v1 (the code is written to be portable).

## Installation

1. Download `camdrop-x.y.z-windows-x64.zip` from the [releases](https://github.com/Wiibleyde/obs-camdrop/releases).
2. Extract it into `C:\ProgramData\obs-studio\plugins` (you get `...\plugins\camdrop\bin\64bit\camdrop.dll`).
3. Restart OBS.

## Quick start

1. **Tools → Camdrop**: tick **Enable the Camdrop server**, click **Apply**. Windows asks to allow
   OBS on the network: allow it for private networks.
2. On the phone, scan the QR code (or type the address, e.g. `https://192.168.1.20:7443`).
3. The first time, the browser warns about the certificate (self-signed): choose
   *Advanced → Continue*. To get rid of the warning, see the [HTTPS guide](docs/https.md).
4. Enter the access code and a name (e.g. `Stage`), allow the camera, tap **Start**.
5. In OBS, add a **Remote camera (Camdrop)** source and pick `Stage`. You can type the name before the
   phone connects.
6. **Docks → Camdrop** shows every camera with its alerts.

## Settings

**Plugin (Tools → Camdrop)**: server on/off, TCP port (page + signaling, default 7443), UDP port range
for WebRTC (default 51000–51100), access code, maximum number of cameras (8), certificate
(self-signed or imported PEM files, reloaded automatically when renewed), requested resolution,
frame rate (30 or 60) and maximum bitrate, preferred codec (H.264 or VP8), hardware decoding,
10 ms Opus frames.

**Source**: camera name, **buffer (ms)**, audio on/off. The buffer is the delay used to absorb network
jitter: 40–60 ms on a good network, 80–150 ms on a busy Wi-Fi. When several sources show the same
camera, the largest buffer applies.

## Latency

| Step | Estimated (ms) |
| --- | --- |
| Phone camera capture | 30–60 |
| Hardware encoding | 10–30 |
| Local network | 2–10 |
| Jitter buffer (source setting) | 20–60 |
| Decoding + OBS rendering | 20–50 |
| **Total** | **80–210** |

Tips: PC on Ethernet, phone on 5 GHz Wi-Fi with power saving off, 30 fps unless you need 60.
On a shared Wi-Fi, plan 4 to 6 phones in 1080p30; a dedicated Wi-Fi 6 access point handles about 8.

## Troubleshooting

See the [FAQ](docs/faq.md): firewall, guest Wi-Fi / client isolation, iOS specifics, certificates.

## Building

Camdrop uses the official [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)
build system. On Windows with Visual Studio 2022 and CMake 3.28+:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```

The first configuration downloads the OBS sources and `obs-deps` (FFmpeg, libdatachannel, mbedTLS,
Qt) and builds libobs. See [CONTRIBUTING.md](CONTRIBUTING.md).

### How it works

```
Phone browser ──HTTPS──▶ page + WebSocket (signaling, telemetry) ─┐
      │                                                           │  Camdrop plugin (OBS)
      └──────── WebRTC (UDP, DTLS-SRTP) ──▶ RTP receiver ──▶ jitter buffer ──▶ FFmpeg ──▶ OBS source
```

- `src/net`: HTTPS + WebSocket server on one port (mbedTLS), self-signed certificate generation.
- `src/core`: camera sessions, signaling, SDP tuning, named channels shared by sources.
- `src/media`: RTP/RTCP, NACK/RTX, frame assembly, A/V clock sync, decoders, playout.
- `src/obs`: the OBS source. `src/ui`: Qt settings dialog and dock. `data/web`: phone page and dock page.
- `src/platform`: the only OS-specific code (network interfaces, sockets).

## License

GPL-2.0-or-later, like OBS. Third-party licenses: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
Camdrop is not affiliated with the OBS Project.

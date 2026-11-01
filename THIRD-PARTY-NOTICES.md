# Third-party notices

Camdrop is distributed under the GNU General Public License, version 2 or (at your option) any later
version. It links to libraries shipped with OBS Studio (obs-deps) and uses the following components:

| Component | Use | License |
| --- | --- | --- |
| [OBS Studio](https://github.com/obsproject/obs-studio) (libobs, obs-frontend-api) | Plugin API | GPL-2.0-or-later |
| [obs-browser](https://github.com/obsproject/obs-browser) `browser-panel.hpp` (copied in `src/ui/third_party/`) | Supervision dock | GPL-2.0-or-later |
| [libdatachannel](https://github.com/paullouisageneau/libdatachannel) (+ libjuice, libsrtp, usrsctp) | WebRTC transport | MPL-2.0 (libjuice MPL-2.0, libsrtp BSD-3-Clause, usrsctp BSD-3-Clause) |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | HTTPS server, certificates, random numbers | Apache-2.0 (compatible with GPL-3.0, hence "or later") |
| [FFmpeg](https://ffmpeg.org) (libavcodec, libavutil) | H.264 / VP8 / Opus decoding | LGPL-2.1-or-later |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON | MIT |
| [QR Code generator](https://github.com/nayuki/QR-Code-generator) | QR code in the settings | MIT |
| [Qt 6](https://www.qt.io) | Settings dialog and dock | LGPL-3.0 |
| [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) | Build system and CI | GPL-2.0-or-later |

Mbed TLS and QR Code generator are linked statically into `camdrop.dll`. The other libraries are the
ones installed with OBS Studio; Camdrop does not ship copies of them.

# Contributing to Camdrop

Thanks for helping! Bug reports, tests on new phones, translations and code are all welcome.

## Reporting a bug

Use the issue templates. Always include the OBS version, Windows version, phone model and browser,
and the OBS log (*Help → Log Files → Upload Current Log File*). The supervision dock values (frame
rate, loss, delay) help a lot for quality problems.

## Building

Requirements: Windows 10/11, Visual Studio 2022 (Desktop C++ workload, Windows SDK 10.0.26100),
CMake 3.28+.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```

The first configuration downloads the OBS sources and `obs-deps` into `.deps/` and builds libobs.
The plugin is in `build_x64/rundir/RelWithDebInfo/`. To try it, copy `camdrop.dll` into
`<OBS>/obs-plugins/64bit/` and the `data` folder into `<OBS>/data/obs-plugins/camdrop/` of a
portable OBS (create `portable_mode.txt` next to `bin/` so your real OBS profile is not touched).

Dependencies all come from `obs-deps` (the same versions OBS ships): libdatachannel (WebRTC),
mbedTLS (HTTPS and certificates), FFmpeg (decoding), nlohmann/json, qrcodegen, Qt 6. Nothing else is
bundled.

## Testing without a phone

Chromium browsers can fake a camera:

```powershell
msedge --use-fake-device-for-media-stream --use-fake-ui-for-media-stream --ignore-certificate-errors `
  "https://127.0.0.1:7443/?code=<code>&name=Test&autostart=1"
```

Test hooks (environment variables read by the plugin, never needed in production):

- `CAMDROP_SIMULATED_LOSS=5` drops 5 % of the received RTP packets (NACK/RTX and recovery tests).
- `CAMDROP_UI_SNAPSHOT=<folder>` opens the settings dialog after start-up and saves a PNG of it.

## Code style

- C++20, `clang-format` 19 with the repository's `.clang-format` (`build-aux/run-clang-format`), CMake
  formatted with `gersemi` (`build-aux/run-gersemi`). CI checks both.
- Keep OS-specific code in `src/platform/` so that the macOS and Linux ports stay easy.
- User-facing strings go through `obs_module_text` (`data/locale/*.ini`) or `data/web/i18n.js`.
- No telemetry, no external requests from the plugin or the web pages.

## Translations

Add `data/locale/<lang>.ini` (copy `en-US.ini`) and a section in `data/web/i18n.js`.

## Pull requests

One topic per pull request, describe how you tested it (phone models, browsers). By contributing you
agree that your work is distributed under GPL-2.0-or-later.

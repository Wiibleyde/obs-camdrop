# FAQ and troubleshooting

## The phone cannot open the page

- **Same network?** The phone must be on the same network as the PC. Guest Wi-Fi networks and
  "client isolation" / "AP isolation" options prevent devices from talking to each other: use the
  main network or disable the isolation in the router.
- **Right address?** Use the address shown in Tools → Camdrop (or the QR code). If the PC has
  several network adapters (VPN, virtual machines, WSL), pick the one of your LAN in the list.
- **Firewall**: when the server starts, Windows asks whether OBS may communicate on networks. Allow
  *private networks*. If you dismissed the prompt: *Windows Security → Firewall → Allow an app* →
  enable OBS Studio for private networks, or add rules for TCP 7443 and UDP 51000–51100:

  ```powershell
  New-NetFirewallRule -DisplayName "Camdrop TCP" -Direction Inbound -Protocol TCP -LocalPort 7443 -Action Allow -Profile Private
  New-NetFirewallRule -DisplayName "Camdrop UDP" -Direction Inbound -Protocol UDP -LocalPort 51000-51100 -Action Allow -Profile Private
  ```

  Make sure your Wi-Fi network is set as *Private* in Windows, not *Public*.

## The page opens but the video never arrives ("connection failed")

The page and the signaling use TCP; the video uses UDP. A firewall that allows TCP 7443 but not the UDP
range gives exactly this symptom. Allow the UDP range (see above). Corporate or school networks may
block UDP between devices: Camdrop is meant for networks you control.

## "Wrong access code" / "Too many attempts"

The code is shown in Tools → Camdrop. After 5 wrong codes in a minute, the phone is blocked for one
minute (longer if it keeps trying). Regenerating the code disconnects every camera.

## The image stutters

- Wi-Fi is the first suspect: put the PC on Ethernet, the phone on 5 GHz, disable Wi-Fi power saving
  on the phone, keep the phone close to the access point.
- Increase the **buffer** of the source (80–150 ms).
- Lower the requested resolution or the maximum bitrate (Tools → Camdrop), or use 30 fps.
- The dock shows packet loss, frame rate and whether the phone limits itself (CPU/heat).

## The stream stops when the phone screen turns off

Browsers suspend pages in the background. Keep the page in front, do not lock the phone, plug the
charger. Camdrop keeps the screen awake (Wake Lock, or a fallback on older browsers) and offers an
**economy mode** (black screen) to reduce heat. If the page goes to the background anyway, the dock
shows it immediately; the camera reconnects by itself when the page comes back.

## iOS

- Safari 16.4 or newer is needed for the screen wake lock.
- Battery level is not available on iOS (no web API).
- Self-signed certificates: see the [HTTPS guide](https.md); a real certificate (own domain) is the
  most comfortable option for iPhones.
- iOS may ask for camera permission again after a while; allow it for the site in
  *Settings → Safari → Camera*.

## Android

- Some devices do not expose H.264 to WebRTC: Camdrop then uses VP8 automatically.
- Chrome can show the certificate warning again after an update; accept it again or use a real
  certificate.

## The picture turns when the phone turns / the phone is mounted sideways

Browsers send portrait pictures when the phone is held upright and landscape ones otherwise. Give the
item the bounding box *Scale to inner bounds* (Ctrl+F, *Fit to screen*) so the layout does not move.
To force an orientation, choose it in *Tools → Camdrop → Requested from the cameras → Orientation*:
the phone then shows a reminder when it is held the wrong way (Android also locks the orientation
when possible). For a phone fixed sideways with its rotation lock on, use the *Rotation* setting of
the source.

## H.265 / HEVC is not used

The browser must offer H.265 for WebRTC: Safari on recent iOS and macOS, Chrome or Edge only with a
hardware encoder and depending on the version. Otherwise Camdrop falls back to H.264 or VP8 by
itself; the dock shows the codec in use.

## The tally light does not show

It is enabled in *Tools → Camdrop → Studio*. Red means the source is in the program output, green
that it is shown elsewhere (studio-mode preview, projector, properties window).

## Audio and video are not in sync

Camdrop synchronises audio and video with the timestamps sent by the browser. OBS adds its own audio
buffering; if you notice a constant offset, adjust *Advanced Audio Properties → Sync Offset* of the
source.

## Several sources show the same camera with different buffers

The camera uses the largest buffer requested by the sources that show it.

## Where are the settings and the certificate stored?

In the `plugin_config/camdrop` folder of the OBS configuration (`settings.json`, `camdrop-cert.pem`,
`camdrop-key.pem`): `%APPDATA%\obs-studio\` on Windows, `~/Library/Application Support/obs-studio/`
on macOS, `~/.config/obs-studio/` on Linux. Delete the folder to reset Camdrop.

## Which OBS versions are supported?

OBS 32.2 and newer on Windows 64-bit, macOS 12+ and Ubuntu 24.04 (PPA). OBS 32.0 and 32.1 ship an
older FFmpeg and cannot load the Windows and macOS builds.

## Logs

*Help → Log Files → View Current Log* and search for `[camdrop]`. Attach the log to bug reports.

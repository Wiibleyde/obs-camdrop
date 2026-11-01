# HTTPS guide

Browsers only give a web page access to the camera and microphone over **HTTPS** (or on
`localhost`). Camdrop therefore serves its page over HTTPS. You have several options.

| Option | Pros | Limits |
| --- | --- | --- |
| **Own domain + Let's Encrypt (DNS-01)** — recommended | Real padlock, no warning, works on iOS and Android | A DNS record pointing to a local IP; some routers block "DNS rebinding" (add a local DNS exception); renewal every 90 days (automatic with a client) |
| Self-signed certificate (default) | Zero configuration | Warning to accept on every phone; painful on iOS |
| Tailscale `serve` | Automatic real certificate | Only devices of your tailnet can connect |
| Cloudflare Tunnel | HTTPS reachable everywhere | External dependency; only the page and signaling go through the tunnel, the WebRTC media stays direct over UDP on the LAN |

## 1. Self-signed certificate (default)

Nothing to do: Camdrop generates a certificate at first start (valid ~2 years, covers `localhost`, the
PC name and its local IP addresses).

- **Android / desktop Chrome / Edge**: on the warning page choose *Advanced* → *Continue to …*.
  The browser remembers the choice for a while.
- **Firefox**: *Advanced* → *Accept the risk and continue*.
- **iOS Safari**: tap *Show details* → *visit this website*. If Safari keeps refusing, install the
  certificate: open `https://<pc-address>:7443/camdrop.crt`, allow the download, then
  *Settings → General → VPN & Device Management* → install the profile, and *Settings → General →
  About → Certificate Trust Settings* → enable full trust for "Camdrop".

If the PC's IP address changes, the old certificate still works (only the warning text changes).
**Regenerate** in Tools → Camdrop → Certificate creates a new one (phones must accept it again).

## 2. Own domain with Let's Encrypt (DNS-01)

The DNS-01 challenge proves you own the domain through a TXT record, so the certificate can be issued
even though the name points to a private IP that Let's Encrypt cannot reach.

1. Pick a name, e.g. `cam.example.com`, and create an **A record** pointing to the PC's local IP
   (e.g. `192.168.1.20`). Give the PC a fixed IP (DHCP reservation in your router).
2. Get a certificate with a DNS-01 capable client, for example
   [win-acme](https://www.win-acme.com/) (Windows) or [lego](https://go-acme.github.io/lego/) /
   [acme.sh](https://github.com/acmesh-official/acme.sh), using your DNS provider's API
   (Cloudflare, OVH, Gandi, …). Example with lego and Cloudflare:

   ```powershell
   $env:CF_DNS_API_TOKEN = "<token with DNS edit permission>"
   lego --email you@example.com --dns cloudflare --domains cam.example.com run
   ```

   You get a full-chain certificate (`cam.example.com.crt`) and a private key (`cam.example.com.key`).
3. In **Tools → Camdrop → Certificate**, choose *Imported certificate* and select the two PEM files.
   Apply. Phones now open `https://cam.example.com:7443` without any warning.
4. Schedule the renewal (win-acme creates a scheduled task; for lego use `renew` in a scheduled task).
   Camdrop checks the files every 30 seconds and reloads them when they change.

**DNS rebinding protection**: some routers/DNS resolvers refuse public names that resolve to a private
address. Symptoms: the name does not resolve on the phone. Add an exception for your domain in the
router (e.g. "DNS rebind protection" whitelist), or use another DNS resolver on the phones.

Never share or commit the private key. Camdrop never ships one.

## 3. Tailscale

Install Tailscale on the PC and the phones, enable HTTPS certificates in the admin console, then on
the PC:

```powershell
tailscale serve --bg https+insecure://127.0.0.1:7443
```

Phones of your tailnet open `https://<pc-name>.<tailnet>.ts.net`. Guests outside your tailnet
cannot connect, which makes it less suited to events with visitors' phones. The WebRTC media uses
whatever path Tailscale provides (direct on the same LAN).

## 4. Cloudflare Tunnel

`cloudflared tunnel --url https://127.0.0.1:7443 --no-tls-verify` gives a public HTTPS name for the
page and the signaling WebSocket. The video itself does not go through the tunnel: the phone must
still be on the same network as the PC (the plugin only accepts local addresses for media).

## Plain HTTP

Camdrop redirects plain `http://` requests from the network to HTTPS (the access code must not travel
in clear text), so Chrome's `unsafely-treat-insecure-origin-as-secure` flag is not needed. Plain HTTP
is only answered on `127.0.0.1`, for the supervision dock inside OBS.

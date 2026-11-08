---
title: LAN Security (Experimental)
---

# LAN Security (Experimental)

Out of the box every device on your network can control the printer: Moonraker trusts all private address ranges, and the camera, MQTT broker and SSH have no real protection. LAN security closes that and keeps the Snapmaker software off the cloud.

Turn it on in `http://<printer-ip>/firmware-config/` under **Security**, then reboot. It works with Fluidd only (Mainsail has no Moonraker login).

## LAN Security (Authentication)

When enabled:

- Everything on the web interface needs a login or an API key: Fluidd, Moonraker, firmware-config, the camera streams (`/webcam*/`), the remote screen and metrics. Only the printer itself is trusted, so devices on your LAN, Tailscale included, must log in.
- Moonraker port 7125 listens on `127.0.0.1` only. Use port 80 or 443 through nginx.
- Snapmaker's MQTT ports 8883 (LAN pairing) and 1884 listen on `127.0.0.1` only, so Snapmaker Orca's own LAN mode no longer works. Send prints through a Moonraker/Klipper printer connection instead.
- SSH accepts keys only (see below).
- RTSP stays off, because `stream-rtsp` has no login and cannot bind to localhost.
- The Prometheus exporter listens on `127.0.0.1:9101`; scrape `https://<printer>/metrics` with an API key.

An `admin` user with a random password is created when you enable it. The password is shown once.

### Login redirect

The pages themselves (`/firmware-config/`, `/filament/`, `/notifications/`, `/screen/`, `/detect-http/`) also require a login. A visitor who is not logged in is redirected to Fluidd's login at `/`, and after logging in is sent back to the page they asked for.

How it works: nginx remembers the requested path in a short-lived `login_next` cookie (10 minutes, plain same-origin paths only). Fluidd's `auth-cookie.js` watches for a login, asks Moonraker whether the token is valid, and then goes to that path. A browser that is already logged in but lost its session cookie (after a restart) is sent back the same way as soon as Fluidd loads.

Fluidd itself, and the two small scripts it loads from `/firmware-config/` (`nav.js`, `print-map.js`), stay public so the login screen can load.

### Camera in Fluidd

Browsers cannot attach a login header to camera requests. Fluidd copies its login token into a `moonraker_token` cookie (`/auth-cookie.js`), and nginx checks it. Log in to Fluidd first; the camera in a private window or a bare `/webcam/` URL returns 401.

### Orca, Mobileraker and scripts

Use the API key: **Security > Show API Key**, sent as the `X-Api-Key` header. In Orca add a Klipper (Moonraker) printer with the printer's IP and paste the key. **Regenerate API Key** invalidates the old one.

### SSH keys

Put your public keys, one per line, in `extended/ssh/authorized_keys` (edit it in Fluidd's config editor). They are copied to `/root/.ssh` and `/home/lava/.ssh` at boot. With LAN security on, password logins are refused and SSH does not start at all while that file is empty.

### Locked out?

Put `extended-recover.txt` on a USB drive and reboot; the extended configuration is reset.

## Snapmaker Cloud

`snapmaker_cloud: blocked` (the default once the feature is present) does three things when the printer boots:

1. Sets `link_mode` to LAN in `printer_data/mqtt/client.json`, so the touchscreen cannot leave LAN mode.
2. Preloads `libcloud-guard.so` into `unisrv` and the processes it starts. Any connection or datagram to an address outside loopback, private (RFC 1918), link-local, CGNAT and multicast ranges fails with `ENETUNREACH`, which also covers TUTK/Kalay UDP. Blocked destinations are logged once each to syslog as `cloud-guard`. The firmware-update host you chose under `[components] upgrade` is allowed.
3. Answers `snapmaker.com`, `snapmaker.cn` and the TUTK relay domains with `0.0.0.0` / `::` through dnsmasq (`blocked-domains.txt`).

Other software is not affected: NTP, Tailscale and anything you enabled yourself still reach the internet.

To check, run `logread | grep cloud-guard` after a day, and look at the printer's traffic on your router or firewall.

## Settings

```ini
[security]
auth: false
snapmaker_cloud: blocked
```

# Operating guide

A practical guide for using the device once it is powered on. For how
the firmware works under the hood, see [Architecture](architecture.md).

## What you should hear at boot

Around 3–5 seconds after power-up, the device speaks two short clips
back-to-back:

1. **Either "Wi-Fi connected" or "Wi-Fi not connected"** — depending on
   whether the saved Wi-Fi credentials worked.
2. **The mode clip for the currently selected source** — one of
   "Bluetooth mode", "Snapcast mode", or "DLNA mode". Test-tone mode is
   silent (no spoken mode clip).

If you turn voice feedback off in the Web UI, the device starts silent.

## First-time setup

1. **Power on.** No Wi-Fi credentials are saved on a fresh device, so it
   immediately starts the captive hotspot:
   - SSID: `Geelen-Audio`
   - Password: `geelen123`
2. You should hear "Wi-Fi not connected" + the mode clip a few seconds
   in.
3. **Connect your phone or laptop** to `Geelen-Audio`. Most operating
   systems pop a captive-portal browser within ~10 s and land you on the
   setup page directly. If yours does not, open
   [http://192.168.4.1/](http://192.168.4.1/) manually.
4. **Pick your home network** from the SSID list (or type one in) and
   enter the Wi-Fi password. Click Save.
5. The device restarts (~500 ms). On the next boot you should hear
   "Wi-Fi connected" + the mode clip, and the device is reachable on
   your home network at the IP address your router assigned. mDNS
   hostname defaults to `geelen-audio.local` (slug of the friendly name
   **Geelen Audio**). Open `http://geelen-audio.local/` if your client
   supports multicast DNS, or use the DHCP IP. Rename hostname on
   **About → Identity** — see [identity.md](identity.md).

If the new credentials are wrong, the device will fall back to the AP
again on the next boot and you can retry.

## Switching modes

The device has four audio sources:

| Mode | Who plays | When to use |
|---|---|---|
| Bluetooth | Your phone over BT Classic A2DP | One-tap audio from a phone |
| Snapcast | A Snapcast server on your LAN | Multi-room sync, librespot/Spotify, custom pipes |
| DLNA | Any UPnP control point (BubbleUPnP, Hi-Fi Cast, VLC, foobar2000, Linn Kazoo, …) | Casting from apps that speak DLNA/UPnP |
| Test tone | On-device generator | Diagnostic / speaker check |

Switch modes from the Web UI ("Mode" page). The device persists the new
mode and **reboots** to rebuild its task graph cleanly. This takes ~1 s
plus the boot voice prompts.

See [audio_modes.md](audio_modes.md) for the full breakdown.

## Pairing Bluetooth

1. Make sure the device is in Bluetooth mode (or has DLNA enabled — DLNA
   does not interfere with the BT pairing flow). The device advertises
   as `Geelen Audio`.
2. From your phone's Bluetooth settings, choose `Geelen Audio` and
   confirm the pairing prompt.
   - For phones that ask for a PIN, the firmware accepts `1234`.
   - You will hear a "Pairing" voice prompt while the request is in
     flight.
3. On successful connect, you will hear "Bluetooth connected".
4. Music plays only AFTER the connect prompt finishes — that is by
   design so you don't lose the first second of a song.

The last paired device is remembered and will reconnect on next
power-on.

## Snapcast quick-start

1. Set up snapserver somewhere on your LAN. A Raspberry Pi running
   `snapserver` + `librespot` (for Spotify) is a popular choice.
2. In the Web UI, set Mode to Snapcast and enter your snapserver host /
   port. Default port is `1704` (the audio stream port; not 1705 which
   is the JSON-RPC control channel).
3. The snap task reconnects automatically when you save host/port — no
   reboot needed.

## DLNA / UPnP

DLNA is always available when `dlna_enabled` is on, regardless of the
selected mode (except test tone). To play to the device:

1. From your phone or laptop, open a DLNA controller app and choose
   `Geelen Audio` (or whatever you renamed it to).
2. Pick a track and hit Play. The device acknowledges within a second.
3. Volume / pause / next-track all work through standard UPnP
   AVTransport / RenderingControl actions.

A DLNA `Play` arriving during the boot mode clip waits up to 15 s for
the prompt to finish before starting music — so the user always gets
the spoken mode confirmation.

## Troubleshooting

### "I don't hear the boot prompts"

- Check Voice Feedback in the Web UI Settings page; if it is off, no
  prompts are spoken.
- Check the amp enable line — GPIO 13 must go HIGH within ~50 ms of
  I2S init. If you are seeing audio later but not at boot, this might
  be a hardware mod issue.
- The first prompt is delayed by 3.5 s while Bluedroid + Wi-Fi + I2S
  settle. If you only hear silence past 10 s, look at the serial log
  for `voice-fb` warnings.

### "Bluetooth connects but I don't hear music"

- Did you hear "Bluetooth connected" first? If yes, the prompt path
  works; check that the phone is actually streaming (and not muted on
  the phone side).
- If you hear nothing after the connect prompt, look at the serial log
  for `A2DP Audio state: 1` (started). No started → the phone is not
  streaming yet.
- If the prompt is also missing, the boot pair may still be in flight —
  wait 10 s and try again.

### "192.168.4.1 doesn't load"

- Make sure your phone is connected to `Geelen-Audio` (not your home
  Wi-Fi). Most phones automatically switch to the network that has
  Internet, which is the wrong direction here — turn cellular data off
  while you set up.
- Try the URL of any HTTP site (e.g. `http://example.com/`); the
  captive DNS will redirect to the setup page.
- If your OS is iOS / Android and refuses to follow the captive prompt,
  go directly to `http://192.168.4.1/` once connected to the AP.

### "Wi-Fi keeps falling back to AP"

- Double-check the saved password. ESP32 does not log the wrong-password
  error very loudly; on the SETUP HTML page you can re-enter it.
- 5 GHz-only networks will not work (this is an ESP32, 2.4 GHz only).
- If your router's 2.4 GHz radio is on a non-default channel (12 / 13)
  and you are in a country where the ESP32 region is locked to 1–11,
  STA association fails silently. Move the AP to channel 1 / 6 / 11.

### "Sound is glitchy / pulsing"

- Test-tone log sweep stuttered briefly in earlier firmware; current
  builds use a fast multiplicative step that should not stutter on a
  healthy board. If it does, check CPU usage with `/api/system/metrics`.
- Snapcast jitter usually comes from the network — wired backhaul
  to the snapserver is the easiest fix.
- Bluetooth pulsing tends to be coexistence with 2.4 GHz Wi-Fi. Move
  the device away from the router or switch the AP to channel 1 if
  the router defaults to channel 6 / 11.

### "Mode change didn't take effect"

The device reboots after any mode change. If you are on the Web UI you
will see a brief "device is restarting" indication; wait ~5 s and
reload. The boot prompts will tell you which mode it landed in.

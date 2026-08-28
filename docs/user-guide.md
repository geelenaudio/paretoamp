# User guide

## First-time setup

1. Power the device. If no Wi-Fi is configured (or credentials were never
   verified), it starts an AP **Geelen-Audio** (password: **geelen123**).
2. Connect your phone/PC to **Geelen-Audio**.
3. Open a browser; you may be redirected to the setup page, or go to
   **http://192.168.4.1**.
4. Pick a network, enter the password, and click **Validate & save**. The
   device tries the credentials **without dropping the hotspot**. Wrong
   passwords show `AUTH_FAIL` and stay on the portal. On success it saves
   and reboots into STA.

## WiFi

- **Station mode:** After a successful validate/commit the device joins
  your network. Find its IP (serial log or DHCP list) and open
  **http://\<device-ip\>/**.
- **AP fallback:** Unverified or unreachable credentials start the
  **Geelen-Audio** AP. The reconnect ladder does **not** run while the
  portal is up, so the phone is not kicked off mid-setup.
- Details: [wifi_onboarding.md](wifi_onboarding.md).

## Bluetooth

- Default source is **Bluetooth** (ESP32-WROVER builds). The device
  advertises as **Geelen Audio** (or the name you set on Overview / About).
  Pair from your phone; audio goes through the single PEQ curve (software
  on Amped, TAS5825M hardware on Louder). Renaming applies live; some
  phones keep the old GAP name until you re-pair.

## Identity / mDNS

- **Overview** has a device-name field. **About → Identity** also edits
  hostname (`http://<hostname>.local/`) and optional STA/BT MAC overrides.
  MAC changes reboot the device. Details: [identity.md](identity.md).

## Spotify / audio via WiFi (Network source)

On-device Spotify Connect is not supported. Use Snapcast instead:

1. On a Raspberry Pi or PC, install **snapserver** and a source such as
   **librespot**.
2. In the Web UI, set **Source** to **Network** and set **Snapcast host**
   to your snapserver IP (port **1704** — the audio stream; 1705 is JSON-RPC).

## Web UI

- **Dashboard:** Volume, source, meters. Optional WS2812 colour and
  brightness live on Overview (`POST /api/led`); pixel count is firmware
  compile-time, not a slider.
- **EQ:** One parametric curve. Amped caps at 10 bands; Louder at 15. See
  [eq.md](eq.md).
- **Wi-Fi:** Validate then commit (same flow as the captive portal).
- **Reset:** Scoped reset via `/api/reset`.

## EQ presets

Presets apply to the single curve. A 15-band Louder preset loaded on Amped
keeps all bands in storage; only the first 10 are applied.

## Manual test matrix (ESP32 Audio Amp)

This checklist is for the **ESP-IDF** PlatformIO environment (`amped-esp32-idf`) and covers the Web UI + all supported audio sources.

### Preconditions

- Flash the device and open a serial monitor at **115200**.
- Know the device IP (STA DHCP lease) or use AP fallback at `192.168.4.1`.
- In the Web UI, source IDs are:
  - **0** = Bluetooth
  - **1** = Network (Snapclient)

### Smoke boot

- **Boot**: device prints boot banner and does not reboot-loop.
- **HTTP**: open `/` and `/api/status` returns JSON with `volume`, `source`, `preset`.
- **WiFi AP fallback** (if STA not configured): AP comes up, Web UI reachable at `192.168.4.1`.

### NVS migration

- Flash onto a device whose NVS still has `audio_source = 2` (legacy Spotify Connect). It should boot without crashing, log `Migrating legacy audio source 2 -> Bluetooth`, and `/api/status` should report `source = 0`.

### Web UI / REST API

- **GET status**: `GET /api/status` returns JSON and reflects current config.
- **Set volume**: `GET /api/volume?v=0` then `v=50` then `v=100`; confirm `volume` updates in `/api/status`.
- **Set WiFi**: `POST /api/wifi` with form fields `ssid`, `pass` triggers reboot and attempts STA.
- **Set source**:
  - `POST /api/source?s=0|1` updates `/api/status`.
  - When switching **into or out of Bluetooth**, the device schedules a reboot (expected).
  - Values outside `0..1` are clamped.

### Identity / mDNS

- **mDNS**: from a LAN client, `http://geelen-audio.local/` (or the
  custom hostname) opens the SPA after STA has an IP. Not advertised
  on the captive AP.
- **MAC override** (About → Identity): set a custom STA and BT MAC,
  reboot, confirm About shows `overridden`; revert with factory
  (`null`) and reboot again. Multicast / identical STA+BT must 400
  without reboot.
- **Rename**: Overview or About Identity; `GET /api/identity` reflects
  the new `device_name` / hostname. Phones may keep the old Bluetooth
  GAP name until they re-pair.

### LED (WS2812)

- **Boot colours**: slow red pulse until STA, one green breath on
  Wi-Fi up, blue pulse while Bluetooth is unpaired (peaks capped by
  `HAL_WS2812_MAX_DUTY`).
- **Overview**: `POST /api/led` with `{ "color": "#rrggbb", "brightness": 0-100 }`
  takes over immediately (`phase: user`).
- **Count**: `GET /api/state` `led.count` matches compile-time
  `HAL_WS2812_COUNT` (12 on stock Amped-WROVER; 35 if built with
  `-DHAL_WS2812_COUNT=35`). Not writable from the SPA.

### Bluetooth (A2DP sink)

- **Select source**: `POST /api/source?s=0` and wait for reboot.
- **Discoverability**: phone sees the configured device name (default
  **“Geelen Audio”**). Rename from Overview or About; re-pair if the
  phone keeps the old GAP name.
- **Connect and play**: start playback; audio is heard on I2S output.
- **Reconnect**: disconnect/reconnect from phone and verify playback resumes.
- **Volume**: use device Web UI volume buttons; confirm change is audible.

### Network (Snapclient)

- **Configure Snapserver**: set host + port in the Web UI (default port 1704 — the Snapcast **stream** port).
- **Select source**: `POST /api/source?s=1` (Network).
- **Playback**: start Snapcast stream; confirm stable audio (no stutter) for 2–5 minutes.
- **Drop/reconnect**: stop server briefly, restart, confirm client reconnects and resumes.

### Regression / resource checks

- **Heap**: after starting each source, confirm no low-heap warnings or crashes.
- **Source switching**: switch through 0 → 1 → 0, waiting for reboots where expected, and verify each mode still functions.

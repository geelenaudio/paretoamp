## Manual test matrix (Pareto Amp)

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
- **WiFi AP fallback** (if STA not configured): AP comes up. Captive OS
  probes still get the Wi-Fi setup HTML. `http://192.168.4.1/` serves the
  full SPA (banner + `/wifi`). Snapcast / home-LAN DLNA / mDNS `.local`
  do not work until STA has an IP.

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
  Wi-Fi up, blue pulse only while the 180 s Bluetooth pairing window is
  open (peaks capped by `HAL_WS2812_MAX_DUTY`). Connected or quiet
  (window elapsed) uses the user colour.
- **Overview**: `POST /api/led` with `{ "color": "#rrggbb", "brightness": 0-100 }`
  takes over immediately (`phase: user`).
- **Count**: `GET /api/state` `led.count` matches compile-time
  `HAL_WS2812_COUNT` (12 on stock Amped-WROVER; 35 if built with
  `-DHAL_WS2812_COUNT=35`). Not writable from the SPA.

### Bluetooth (A2DP sink)

- **Select source**: `POST /api/source?s=0` and wait for reboot.
- **Pairing window**: after A2DP start the device is inquiry-visible for
  180 s (`GET /api/bluetooth` `discoverable` + `window_ms_left`). Then it
  goes quiet (`CONNECTABLE` + `NON_DISCOVERABLE`). Overview **Pair new
  device** (`POST /api/bluetooth` `{ "discoverable": true }`) reopens the
  window. While connected it is not in scan lists.
- **PIN**: **About → Identity** (or `POST /api/bluetooth` `{ "pin": "1234" }`).
  Legacy PIN_REQ only; changing PIN does not unpair. **Reset → Bluetooth
  pairings** actually wipes Bluedroid peers.
- **Connect and play**: start playback; audio is heard on I2S output.
- **Reconnect**: disconnect/reconnect from phone (disconnect opens a new
  180 s window) and verify playback resumes.
- **Volume**: Web UI / `POST /api/volume` is audible **and** the phone
  slider should follow (AVRCP Target absolute volume). Phone volume keys
  persist to NVS like the SPA. Volume 0 is level 0, not SPA mute.
- **AP + BT heap**: pair from the Geelen-Audio AP (full SPA at
  `192.168.4.1`). Confirm UART `heap: int8=` / `Bluedroid pre-init heap:
  int8=` after pair; soak a few minutes with SPA + A2DP. Do not expect
  more than ~24 KiB internal DRAM in this configuration.

### USB audio (S3 only)

Requires Amped-S3 or Louder-S3 with USB-C wired to the SoC PHY (GPIO 19/20).
WROVER builds must **not** show the USB card (`supports_usb: false`) and
`POST /api/mode {"mode":"usb"}` returns 400.

- **Idle S3 (not USB mode)**: serial is USB-Serial-JTAG (`ttyACM`). SPA Mode
  page shows **USB audio**.
- **Select USB**: reboot; host sees a UAC speaker (“Pareto Amp”) at 48 kHz
  16-bit stereo. Spoken/ident clip after the Wi-Fi prompt.
- **Playback**: Windows / macOS / Linux play through EQ and the volume
  control (host Feature Unit and SPA). Amped-S3: 10-band software PEQ on;
  Louder-S3: TAS PEQ.
- **Pause / unplug / replug**: audio recovers, no reboot loop, IIR not stuck.
- **Budget**: log `dma_wait` and underruns. Amped-S3 + EQ is the gate — if
  `dma_wait` collapses toward 12–20% with glitches, stop and cut USB work.
- **Voice**: trigger a prompt while the host is playing; music gates, prompt
  plays, music resumes.
- **Mode USB → Snapcast → USB**: USJ vs TinyUSB each boot; flashing still
  works (BOOT download or UART in USB mode).
- **Soak** ≥ 30 min: no SOF stalls, `overflows_` not climbing.
- **WROVER**: NVS `src=4` migrates to Bluetooth on boot.

Louder: speaker rail still required; USB does not power the Class-D.

### Network (Snapclient)

- **Configure Snapserver**: set host + port in the Web UI (default port 1704 — the Snapcast **stream** port).
- **Select source**: `POST /api/source?s=1` (Network).
- **Playback**: start Snapcast stream; confirm stable audio (no stutter) for 2–5 minutes.
- **Drop/reconnect**: stop server briefly, restart, confirm client reconnects and resumes.

### Regression / resource checks

- **Heap**: after starting each source, confirm no low-heap warnings or crashes.
- **Source switching**: switch through 0 → 1 → 0, waiting for reboots where expected, and verify each mode still functions.

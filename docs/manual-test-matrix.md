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

### Bluetooth (A2DP sink)

- **Select source**: `POST /api/source?s=0` and wait for reboot.
- **Discoverability**: phone sees device name **“Geelen Audio”**.
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

## Manual test matrix (ESP32 Audio Amp)

This checklist is intended for the **ESP-IDF** PlatformIO environment (`amped-esp32-idf`) and covers the Web UI + all audio sources.

### Preconditions

- Flash the device and open a serial monitor at **115200**.
- Know the device IP (STA DHCP lease) or use AP fallback at `192.168.4.1`.
- In the Web UI, source IDs are:
  - **0** = Bluetooth
  - **1** = Network (Snapclient)
  - **2** = Spotify Connect (cspot)

### Smoke boot

- **Boot**: device prints boot banner and does not reboot-loop.
- **HTTP**: open `/` and `/api/status` returns JSON with `volume`, `source`, `preset`.
- **WiFi AP fallback** (if STA not configured): AP comes up, Web UI reachable at `192.168.4.1`.

### Web UI / REST API

- **GET status**:
  - `GET /api/status` returns JSON and reflects current config.
- **Set volume**:
  - `GET /api/volume?v=0` then `v=50` then `v=100`; confirm `volume` updates in `/api/status`.
- **Set WiFi**:
  - `POST /api/wifi` with form fields `ssid`, `pass` triggers reboot and attempts STA.
- **Set source**:
  - `POST /api/source?s=0|1|2` updates `/api/status`.
  - When switching **into or out of Bluetooth/Spotify**, device schedules a reboot (expected).

### Bluetooth (A2DP sink)

- **Select source**: `POST /api/source?s=0` and wait for reboot.
- **Discoverability**: phone sees device name **“Geelen Audio”**.
- **Connect and play**: start playback; audio is heard on I2S output.
- **Reconnect**: disconnect/reconnect from phone and verify playback resumes.
- **Volume**: use device Web UI volume buttons; confirm change is audible (if your pipeline applies volume post-decode).

### Network (Snapclient)

- **Configure Snapserver** (in Web UI / config): set host + port (or whatever UI you use for snap settings).
- **Select source**: `POST /api/source?s=1` (Network).
- **Playback**: start Snapcast stream; confirm stable audio (no stutter) for 2–5 minutes.
- **Drop/reconnect**: stop server briefly, restart, confirm client reconnects and resumes.

### Spotify Connect (cspot)

- **Select source**: `POST /api/source?s=2` and wait for reboot.
- **Zeroconf discovery**: Spotify app sees the device as a Connect target.
- **Pair**:
  - `GET /spotify_info` returns JSON.
  - Spotify app pairs (device becomes controllable).
- **Playback**: start a track; confirm audio output.
- **Persistence**:
  - Reboot device; verify it starts Spotify using saved credentials (no re-pairing).
- **Factory reset**:
  - Trigger factory reset (whatever UI endpoint exists); verify Spotify credentials are cleared and pairing is required again.

### Regression / resource checks

- **Heap**: after starting each source, confirm no low-heap warnings or crashes.
- **Source switching**: switch through 0 → 1 → 2 → 0, waiting for reboots where expected, and verify each mode still functions.


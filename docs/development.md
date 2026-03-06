# Development

## Architecture

Three main modules:

1. **Audio Engine** – I2S output, 10-band biquad EQ, PSRAM ring buffer. Receives PCM from A2DP or Snapclient and outputs to the DAC.
2. **Web Server** – ESPAsyncWebServer, serves the control UI and REST API (volume, source, EQ, WiFi, Snapcast, factory reset).
3. **Config Manager** – LittleFS (presets, custom EQ slots) and NVS (volume, preset, source, WiFi, Snapcast host/port, last BT device). Saves with debounce.

Data flow: **A2DP** or **Snapclient** → ring buffer (PSRAM) → **EQ** → **I2S** (GPIO 25/26/22). Source selection and config are in Config Manager; Web Server and main apply them.

## Build

- **PlatformIO** env: **amped-esp32**
- From the project directory (the one containing `platformio.ini`):
  - `pio run`
  - `pio run --target upload`
  - `pio device monitor`

## Dependencies

- **espressif32** – Arduino framework
- **ESP32-A2DP** (GitHub) – Bluetooth A2DP Sink
- **ESPAsyncWebServer** + **AsyncTCP** – async HTTP server
- **ArduinoJson** – JSON for API
- **LittleFS** – flash filesystem (partition in `partitions.csv`)

## Code layout

- `src/main.cpp` – entry, I2S init, GPIO 13, WiFi, A2DP/Snapclient source switch, web server, loop
- `src/config_manager.cpp`, `include/config_manager.h` – config and presets
- `src/audio_engine.cpp`, `include/audio_engine.h` – EQ and I2S output
- `src/snapclient.cpp`, `include/snapclient.h` – Snapcast client
- `src/audio_util.cpp`, `include/audio_util.h` – portable dB→linear (used by audio_engine and native tests)
- `src/snapcast_protocol.cpp`, `include/snapcast_protocol.h` – portable Snapcast header parsing (used by snapclient and native tests)
- `src/web_server.cpp`, `include/web_server.h` – HTTP server and API
- `test/test_native/` – Unity tests for native env; `test/audio_util.cpp`, `test/snapcast_protocol.cpp` are built with native tests
- `partitions.csv` – NVS, app, LittleFS partition

## Testing

- **Host (CI):** `pio test -e native` runs unit tests on the build host (no device). Covers `audio_util` (gainDbToLinear) and `snapcast_protocol` (Snapcast base header parsing). Used in GitHub Actions.
- **Device (optional):** With a board connected, `pio test -e amped-esp32` can run tests on the target; not run in CI.

## Amped-ESP32 board

- Board: **esp-wrover-kit** (PlatformIO)
- 8 MB flash; LittleFS partition for presets
- I2S and amp enable pins defined in `platformio.ini` build flags and in code.

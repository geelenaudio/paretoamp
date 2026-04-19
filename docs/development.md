# Development

## Architecture

Three main modules:

1. **Audio Engine** – I2S output, 10-band biquad EQ, PSRAM ring buffer. Receives PCM from A2DP or Snapclient and outputs to the DAC.
2. **HTTP Server** – `esp_http_server`, serves the control UI and REST API (volume, source, EQ, WiFi, Snapcast, factory reset).
3. **Config Manager** – LittleFS (presets, custom EQ slots) and NVS (volume, preset, source, WiFi, Snapcast host/port). Saves with debounce.

Data flow: **A2DP** or **Snapclient** → ring buffer (PSRAM) → **EQ** → **I2S** (GPIO 25/26/22). Source selection and config are in Config Manager; HTTP server and `app_main` apply them.

## Build

- **PlatformIO** env: **amped-esp32-idf** (pure ESP-IDF, no Arduino).
- From the project directory (the one containing `platformio.ini`):
  - `pio run`
  - `pio run --target upload`
  - `pio device monitor`

## Dependencies

- **espressif32** platform with the **espidf** framework.
- ESP-IDF components used at runtime: `nvs_flash`, `spiffs`, `esp_wifi`, `esp_netif`, `esp_event`, `esp_http_server`, `lwip`, `mdns`, `bt`.

## Code layout

- `src/idf_main.cpp` – entry, I2S init, GPIO 13, WiFi, A2DP/Snapclient source switch, HTTP server
- `src/idf_config_manager.{h,cpp}` – config and presets (NVS + LittleFS)
- `src/audio_engine.{h,cpp}` – EQ and I2S output
- `src/idf_snapclient.{h,cpp}`, `src/snapcast_protocol.{h,cpp}` – Snapcast client
- `src/idf_a2dp_sink.{h,cpp}` – Bluetooth Classic A2DP sink
- `src/idf_wifi_manager.{h,cpp}` – WiFi STA with AP fallback
- `src/idf_http_server.{h,cpp}` – HTTP server and API
- `src/audio_util.{h,cpp}` – portable dB→linear (used by audio_engine and native tests)
- `test/test_native/` – Unity tests for the native env; `audio_util` and `snapcast_protocol` are built against host code
- `partitions_idf.csv` – NVS, app, SPIFFS partition table

## Testing

- **Host (CI):** `pio test -e native` runs unit tests on the build host (no device). Covers `audio_util` (gainDbToLinear) and `snapcast_protocol` (Snapcast base header parsing). Used in GitHub Actions.

## Amped-ESP32 board

- Board: **esp-wrover-kit** (PlatformIO)
- 8 MB flash; SPIFFS partition for presets
- I2S and amp enable pins defined in `idf_main.cpp`.

# Development

The firmware is a pure ESP-IDF project, built through PlatformIO so the
toolchain + IDF version are pinned via `platformio.ini`. There is also a
`native` env for host-side unit tests that compile against the same
audio / Snapcast helpers.

## Architecture overview

Three main layers:

1. **Audio Engine** – I2S output, single-layer parametric EQ + auto
   headroom + limiter, PSRAM ring buffer. Receives PCM via `feedPCM`
   from any number of producers and drains it to the DAC in `process()`.
2. **HTTP server** – `esp_http_server` serves the SPA, a JSON REST API,
   the OpenAPI spec at `/docs`, the captive-portal setup page, and DLNA
   SOAP / SSDP descriptors.
3. **Config Manager** – NVS for scalar config (volume, source, Wi-Fi,
   Snapcast host/port, EQ enables, …), SPIFFS for static assets (SPA
   bundle + voice MP3s). Saves are debounced.

See [Architecture](architecture.md) for the task graph, the ring buffer
invariants, and the voice-prompt state machine.

## Build / flash / monitor

The PlatformIO env is `amped-esp32-idf` (default). All commands are run
from `ESP32_AudioPlatform/` (the directory that contains
`platformio.ini`):

```sh
pio run                     # build firmware (ELF + bin)
pio run -t upload           # flash firmware over USB
pio run -t buildfs          # build SPIFFS image from data/
pio run -t uploadfs         # flash SPIFFS partition
pio device monitor          # 115200 baud, with esp32 exception decoder
```

Typical full-flash workflow after editing both code and SPA / audio
assets:

```sh
pio run -t uploadfs && pio run -t upload && pio device monitor
```

Helpful flags:

- `pio run -e native` — build the host-side test binary.
- `pio run -t menuconfig` — open the IDF sdkconfig menu (saved to
  `sdkconfig.defaults`).

## Code layout

- `src/idf_main.cpp` — entry, I2S init, GPIO 13, Wi-Fi, A2DP / Snapclient /
  test-tone wiring, voice scheduling, source-specific task graph.
- `src/audio_engine.{h,cpp}` and `include/audio_engine.h` — EQ, limiter,
  ring buffer, I2S output.
- `src/idf_a2dp_sink.{h,cpp}` — Bluetooth Classic A2DP sink.
- `src/idf_snapclient.{h,cpp}` + `src/snapcast_protocol.{h,cpp}` —
  Snapcast client.
- `src/idf_dlna.{h,cpp}` — DLNA MediaRenderer (SSDP + SOAP +
  AVTransport / RenderingControl / ConnectionManager).
- `src/idf_mp3_streamer.{h,cpp}` — Helix-based MP3 decoder for HTTP +
  SPIFFS clips.
- `src/idf_led.{h,cpp}` — WS2812 boot/pairing/user animator (Core 0,
  prio 3, PSRAM stack).
- `src/idf_identity.{h,cpp}` — device name, hostname slug, MAC
  override helpers.
- `src/idf_mdns.{h,cpp}` — STA `_http._tcp` responder after got-IP.
- `src/idf_voice_cache.{h,cpp}` — boot-time MP3 → PSRAM PCM cache;
  prompt paths live in the `kClipManifest` table there.
- `src/idf_voice_feedback.{h,cpp}` — voice-owner state machine + prompt
  queue.
- `src/idf_test_tone.{h,cpp}` — sine / square / pink-noise / log-sweep
  generator.
- `src/idf_wifi_manager.{h,cpp}` — STA + AP fallback + captive DNS +
  runtime recovery timer.
- `src/idf_http_server.{h,cpp}` + `src/idf_api.cpp` — HTTP server, JSON
  API endpoints, captive-portal HTML, OpenAPI rendering.
- `src/idf_config_manager.{h,cpp}` — NVS + SPIFFS config.
- `src/idf_logs.{h,cpp}` — ring buffer of recent log lines for
  `/api/logs`.
- `src/idf_cpu_sampler.{h,cpp}` — periodic CPU sampling for
  `/api/system/metrics`.
- `partitions_idf.csv` — NVS, app, SPIFFS partition table (8 MB flash).

## Web SPA

The SPA source lives in `web/` (`api.js`, `app.js`, `pages/*.js`,
`api/openapi.yaml`). On every build, `scripts/build_web.py` (registered
as a pre-build step in `platformio.ini`) bundles + gzips the SPA into
`data/web/` so the SPIFFS image picks it up.

To iterate on UI changes:

1. Edit files under `web/`.
2. Run `pio run -t buildfs -e amped-esp32-idf && pio run -t uploadfs`
   to flash just the SPIFFS partition. The firmware does not need
   re-flashing for SPA changes.

## SPIFFS audio assets

Voice prompts live in `ESP32_AudioPlatform/data/audio/`. Names must match
the `kClipManifest` table in `src/idf_voice_cache.cpp`:

```46:52:ESP32_AudioPlatform/src/idf_voice_cache.cpp
    {VOICE_CLIP_WIFI_CONNECTED,      "/spiffs/audio/WifiConnected.mp3",      "WifiConnected.mp3"},
    {VOICE_CLIP_WIFI_DISCONNECTED,   "/spiffs/audio/WifiDisconnected.mp3",   "WifiDisconnected.mp3"},
    {VOICE_CLIP_BLUETOOTH_MODE,      "/spiffs/audio/BluetoothMode.mp3",      "BluetoothMode.mp3"},
    {VOICE_CLIP_SNAPCAST_MODE,       "/spiffs/audio/SnapcastMode.mp3",       "SnapcastMode.mp3"},
    {VOICE_CLIP_DLNA_MODE,           "/spiffs/audio/DLNAMode.mp3",           "DLNAMode.mp3"},
    {VOICE_CLIP_BLUETOOTH_CONNECTED, "/spiffs/audio/BluetoothConnected.mp3", "BluetoothConnected.mp3"},
    {VOICE_CLIP_PAIRING,             "/spiffs/audio/Pairing.mp3",            "Pairing.mp3"},
```

To add a new prompt:

1. Drop a short MP3 (≤ 2 s, mono or stereo, any common sample rate) in
   `data/audio/`.
2. Add a `VOICE_CLIP_*` enum value and a matching `kClipManifest` row
   in `idf_voice_cache.{h,cpp}`, then enqueue it from
   `idf_voice_feedback.cpp` (or the event that should speak it).
3. `pio run -t uploadfs && pio run -t upload`.

To swap an existing prompt, just replace the MP3 with a same-named file
and re-run `uploadfs`.

## OpenAPI

The canonical API schema is `web/api/openapi.yaml`. The SPA renders it
at `/docs` on the device using a small viewer page.

When you add or change an endpoint:

1. Update the handler in `src/idf_api.cpp` (or DLNA / HTTP server as
   appropriate).
2. Update `web/api/openapi.yaml` to match.
3. Bump any matching client code in `web/api.js`.
4. Re-run `pio run -t uploadfs` so the new spec is served from SPIFFS.

## Native tests

```sh
pio test -e native
```

Runs Unity tests under `test/test_native/`. Current coverage:

- `audio_util` — `gainDbToLinear` matches the ESP target's behaviour.
- `snapcast_protocol` — base header parsing.
- `eq_coeffs` — biquad coefficient generation matches the reference.

The native env is also exercised in CI, so any change that breaks the
host build will fail before flash.

## Toolchain dependencies

- `platform = espressif32`, framework `espidf`. Version pinned by the
  PlatformIO platform release.
- IDF components: `nvs_flash`, `spiffs`, `esp_wifi`, `esp_netif`,
  `esp_event`, `esp_http_server`, `esp_timer`, `lwip`, `mdns`, `bt`.
- External libraries: `chmorgan/esp-libhelix-mp3` (MP3 decoding), pulled
  via the IDF component manager; vendored `espressif/led_strip` 3.0.3
  (`components/led_strip/`) for WS2812 RMT.

## Hardware

See [hardware.md](hardware.md) for the MCU / DAC / amp pinout. The
firmware targets the Amped-ESP32 board (non-plus, non-S):

- ESP32-WROVER-N8R8 (8 MB Flash, 8 MB PSRAM, BT 4.2 BR/EDR + BLE).
- PCM5100A DAC on I2S0 (BCLK GPIO 26, WS GPIO 25, DOUT GPIO 22).
- TPA3128D2 amp gated by GPIO 13 (driven HIGH only after I2S clock is
  stable to avoid pops).

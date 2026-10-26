# Development

Pareto Amp firmware (from [geelen.audio](https://geelen.audio)) is a pure ESP-IDF project, built through PlatformIO so the
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

Four PlatformIO envs live in `pareto-amp/platformio.ini`. The env is
**compile-time** (HAL + TAS driver). Flashing Amped onto a Louder board leaves
EQ on the software cascade. Prefer named tasks from the **repo root**:

```sh
task fw:amped       # Amped-WROVER
task fw:louder      # Louder-WROVER + TAS58xx
task fw:amped-s3
task fw:louder-s3
task fw:targets     # cheat sheet
task fw:ports       # serial devices
```

Generic `task fw:upload` defaults to Amped. Override with
`task fw:upload PIO_ENV=louder-esp32-idf` (or `PIO_ENV=…` in the environment).
Port defaults to `/dev/ttyUSB0`; pass `PIO_PORT=/dev/ttyACM0` for S3 CDC
(USB-Serial-JTAG). In **USB audio** mode TinyUSB owns the PHY — flash via
ROM download (hold BOOT, still `ttyACM`) or a UART adapter.

Raw PlatformIO, from `pareto-amp/` (the directory that contains
`platformio.ini`):

```sh
pio run -e louder-esp32-idf                     # build
pio run -e louder-esp32-idf -t upload           # flash app
pio run -e louder-esp32-idf -t uploadfs         # flash SPIFFS
pio device monitor -e louder-esp32-idf          # 115200, exception decoder
```

Omit `-e` and PlatformIO uses `default_envs = amped-esp32-idf`. Typical
full-flash after editing both firmware and SPA:

```sh
task fw:louder && task fw:monitor PIO_ENV=louder-esp32-idf
```

Helpful flags:

- `pio run -e native` — build the host-side test binary.
- `pio run -t menuconfig` — open the IDF sdkconfig menu (saved to
  `sdkconfig.defaults`).
- `task fw:factory` — app + SPIFFS merged into one `0x0` factory image under
  `pareto-amp/dist/` (flashing it wipes NVS).

## Versioning and releases

[semantic-release](https://github.com/semantic-release/semantic-release)
runs on every push to `main` after native tests pass. It reads
**Conventional Commits**, picks the next SemVer, tags `vMAJOR.MINOR.PATCH`,
writes [`pareto-amp/version.txt`](../pareto-amp/version.txt) (`PROJECT_VER` /
About → Version), and opens a GitHub Release. CI then builds the four
factory images and attaches them.

| Commit | Release |
|---|---|
| `fix: …` | PATCH |
| `feat: …` | MINOR |
| footer `BREAKING CHANGE: …` | MAJOR |
| `docs:`, `chore:`, `style:`, `test:` | none |

```
feat(usb): add UAC playback on S3

fix: stop EQ click on mute

docs: explain factory flash

feat!: new NVS layout

BREAKING CHANGE: existing devices must factory-reset.
```

`chore(release):` commits from the bot include `[skip ci]` so they do not
loop. OpenAPI `info.version` is the HTTP schema, not the firmware tag.

Do not push `vMAJOR.MINOR.PATCH` tags. semantic-release is the only
release path; a tag push does not build or publish firmware. How to
open issues and pull requests: [Contributing](../CONTRIBUTING.md).

Each Release attaches four factory images (bootloader + partition table +
app + SPIFFS website). `esptool write-flash 0x0 …` is a **factory write**:
NVS at `0x9000` is 0xFF, so Wi-Fi credentials and EQ presets are cleared.
`task fw:flash` updates app + SPIFFS only and keeps NVS.

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
  prio 3, PSRAM stack) plus a 2 s white volume bar overlay.
- `src/idf_encoder.{h,cpp}` — Louder-WROVER rotary encoder (volume,
  mute, long-press pairing). No-op when `HAL_HAS_ENCODER=0`.
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
2. Run `task fw:web PIO_ENV=<env>` (or `pio run -e <env> -t uploadfs`)
   to flash just the SPIFFS partition. The firmware does not need
   re-flashing for SPA changes. Use the same env you last flashed
   (`louder-esp32-idf` on Louder, not the Amped default).

## SPIFFS audio assets

Voice prompts live in `pareto-amp/data/audio/`. Names must match
the `kClipManifest` table in `src/idf_voice_cache.cpp`:

```46:52:pareto-amp/src/idf_voice_cache.cpp
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

The canonical API schema is `web/api/openapi.yaml`. `GET /docs` on the
device is a Swagger UI page (CDN bundle, on-device fallback) pointed at
`GET /openapi.yaml`.

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
- Mode FSM — USB↔USB Noop; USB↔any other RebootRequired; `nameForSource("USB")`.
- Event IDs — `EV_USB_*` appended; `EV_COUNT_` is 22.

The native env is also exercised in CI, so any change that breaks the
host build will fail before flash.

## Toolchain dependencies

- `platform = ${common.platform}` in `platformio.ini`: pioarduino
  `55.03.37` (ESP-IDF 5.5.2). Do not leave this unpinned — GitHub Actions
  otherwise installs a current `espressif32` with IDF 6.x.
- PlatformIO Core **6.1.19** (not 6.2). 6.2 ships SCons 4.11.1; this
  platform pins 4.8.1, and the mismatch wipes `tool-scons` mid-link. CI
  pins the same version in `.github/workflows/ci.yml`.
- IDF components: `nvs_flash`, `spiffs`, `esp_wifi`, `esp_netif`,
  `esp_event`, `esp_http_server`, `esp_timer`, `lwip`, `mdns`, `bt`.
- External libraries: vendored `chmorgan/esp-libhelix-mp3`
  (`components/libhelix-mp3/`); vendored `espressif/led_strip` 3.0.3
  (`components/led_strip/`) for WS2812 RMT; vendored
  `espressif/usb_device_uac` 1.3.2 (`components_s3/usb_device_uac/`, S3
  only) plus managed `espressif/tinyusb` for USB audio.

## Hardware

See [hardware.md](hardware.md) for the MCU / DAC / amp pinout. The
firmware targets the Amped-ESP32 board (non-plus, non-S):

- ESP32-WROVER-N8R8 (8 MB Flash, 8 MB PSRAM, BT 4.2 BR/EDR + BLE).
- PCM5100A DAC on I2S0 (BCLK GPIO 26, WS GPIO 25, DOUT GPIO 22).
- TPA3128D2 amp gated by GPIO 13 (driven HIGH only after I2S clock is
  stable to avoid pops).

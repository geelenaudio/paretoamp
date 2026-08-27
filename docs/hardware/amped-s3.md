# Hardware — Amped-ESP32-S3 (WROOM-N8R8)

Same DAC + amp topology as the WROVER Amped, new SoC.

> **Build-validated only in this refactor.** The pin map is the
> ESP32-S3-DevKitC-1 default; verify against your actual Amped-S3 revision
> before flashing hardware. On-device validation is a Phase 10 follow-up
> (see [docs/roadmap.md](../roadmap.md)).

## MCU

- **ESP32-S3-WROOM-N8R8** — dual-core 240 MHz, 8 MB Flash, 8 MB Octal PSRAM.
- Bluetooth **5.0** (BLE only — no A2DP in the vanilla S3 firmware today).
- The A2DP sink code in `src/idf_a2dp_sink.cpp` is Bluetooth Classic; on S3
  it will need to be re-implemented against BLE Audio profiles in a follow-up.

## I2S (DAC)

- **PCM5100A**, same silicon as the Amped-WROVER build.
- Pinout (S3-DevKitC-1 defaults, see `src/hal/boards/amped_s3.h`):
  - **BCK** — GPIO 5
  - **WS** — GPIO 4
  - **DATA** — GPIO 6
- The S3 has different strapping / USB-serial / flash pins than the WROVER,
  so these choices are the safe defaults on a DevKitC-1. A production revision
  should ship its own board header under `src/hal/boards/`.

## Amp enable

- **GPIO 7** drives the TPA3128D2 enable pin (placeholder — confirm on the
  board revision you're flashing).

## WS2812B (optional)

- **GPIO 9** — Sonocotta ESP32-S3 `RGB OUT`. Count is `HAL_WS2812_COUNT`
  (default 1); override with `-DHAL_WS2812_COUNT=N`. Peak duty
  `HAL_WS2812_MAX_DUTY` default 50; `-DHAL_WS2812_MAX_DUTY=N`.

## EQ backend

- **Software** parametric EQ, 10 bands (identical to Amped-WROVER).

## Flashing

```bash
pio run -e amped-esp32s3-idf -t upload
pio run -e amped-esp32s3-idf -t uploadfs
```

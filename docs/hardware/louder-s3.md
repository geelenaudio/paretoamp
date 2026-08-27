# Hardware — Louder-ESP32-S3 (WROOM-N8R8)

TAS5825M on the ESP32-S3 SoC. Same DAC control path as Louder-WROVER;
different SoC pinout.

> **Build-validated only.** Pin map is the S3-DevKitC-1 default; verify
> against the actual Louder-S3 revision before flashing.

## MCU

- **ESP32-S3-WROOM-N8R8** — dual-core 240 MHz, 8 MB Flash, 8 MB Octal PSRAM.
- BLE 5.0. Same A2DP caveat as [Amped-S3](amped-s3.md).

## I2S / DAC

- **TAS5825M**, same silicon as Louder-WROVER.
- Pinout (S3-DevKitC-1 defaults; see `src/hal/boards/louder_s3.h`):
  - **BCK** — GPIO 15
  - **WS**  — GPIO 16
  - **DATA** — GPIO 7
- I2C bus at 400 kHz on SDA=GPIO 8, SCL=GPIO 9.
- **PDN** — GPIO 17.

## WS2812B (optional)

- **GPIO 21** — Tindie Louder-Plus S3 `RGB OUT`. Do not use GPIO 9 (I2C SCL).
- `HAL_WS2812_COUNT` default 1; override with `-DHAL_WS2812_COUNT=N`.
- `HAL_WS2812_MAX_DUTY` default 50; override with `-DHAL_WS2812_MAX_DUTY=N`.

## EQ backend

- **Hardware** parametric EQ, 15 bands via the vendored sonocotta driver.

## Flashing

```bash
pio run -e louder-esp32s3-idf -t upload
pio run -e louder-esp32s3-idf -t uploadfs
```

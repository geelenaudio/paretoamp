# Hardware variants index

This firmware targets four boards. Every build uses the same code tree; the
[`src/hal/`](../../ESP32_AudioPlatform/src/hal/) layer selects pinouts, DAC
drivers, and EQ backend at compile time.

| Board                                             | SoC                    | DAC      | EQ backend | Bands | RGB GPIO | PIO env               |
| ------------------------------------------------- | ---------------------- | -------- | ---------- | ----- | -------- | --------------------- |
| [Amped-ESP32 (WROVER)](amped-wrover.md)           | ESP32-WROVER-N8R8      | PCM5100A | Software   | 10    | 12       | `amped-esp32-idf`     |
| [Amped-ESP32-S3](amped-s3.md)                     | ESP32-S3-WROOM-N8R8    | PCM5100A | Software   | 10    | 9        | `amped-esp32s3-idf`   |
| [Louder-ESP32-Plus (WROVER)](louder-wrover.md)    | ESP32-WROVER-N8R8      | TAS5825M | Hardware   | 15    | 12       | `louder-esp32-idf`    |
| [Louder-ESP32-S3](louder-s3.md)                   | ESP32-S3-WROOM-N8R8    | TAS5825M | Hardware   | 15    | 21       | `louder-esp32s3-idf`  |

RGB is optional WS2812B on Sonocotta `RGB OUT`. Pixel count and peak
brightness are compile-time per board: Amped-WROVER is **12 / 10**; the
other three headers default to **1 / 50**. Override with
`-DHAL_WS2812_COUNT=N` / `-DHAL_WS2812_MAX_DUTY=N` (count cap 64). See
[`docs/hal.md`](../hal.md).

See [`docs/board_variants.md`](../board_variants.md) for the HAL rationale and
[`docs/hal.md`](../hal.md) for the layering (per-board header → DAC HAL → the
rest of the firmware).

## Validation status

Only the **Amped-WROVER** target is validated on-device by the refactor
authors (`/dev/ttyUSB0`). The other three variants are built by
`pio run -e <env>` but not run on hardware; on-device bring-up is captured
under "Board / HAL follow-ups" in [`docs/roadmap.md`](../roadmap.md).

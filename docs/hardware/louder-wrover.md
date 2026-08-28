# Hardware — Louder-ESP32-Plus (WROVER-N8R8)

TAS5825M-based Class-D amplifier with on-chip 15-band DSP. This board unlocks
the hardware PEQ path in the firmware (`HAL_HAS_HW_PEQ=1`).

> **Build-validated only in this refactor.** Verify pin choices against your
> Louder-ESP32-Plus schematic revision. Full on-device validation of Louder
> variants is a Phase 10 follow-up (see [../roadmap.md](../roadmap.md)).

## MCU

- **ESP32-WROVER-N8R8** — same as Amped-WROVER.

## I2S (DAC)

- **TAS5825M** — stereo I2C-controlled Class-D DAC + power stage with on-chip
  15-band biquad EQ, DRC, and mono/stereo mixing.
- I2S pinout (see `src/hal/boards/louder_wrover.h`; confirm against your
  schematic):
  - **BCK** — GPIO 27
  - **WS**  — GPIO 25
  - **DATA** — GPIO 26
- Uses the new I2S driver (`driver/i2s_std.h`).

## DAC control (TAS5825M)

- I2C bus at 400 kHz on SDA=GPIO 21, SCL=GPIO 22.
- 7-bit address `0x4C` (ADR pin low). Set `HAL_TAS5825M_I2C_ADDR` in
  `src/hal/boards/louder_wrover.h` if your board straps ADR high.
- The DAC is controlled through the vendored sonocotta driver at
  [`components/tas5805m/`](../../ESP32_AudioPlatform/components/tas5805m/).
  Refer to that component's README for licensing (GPL-3.0-or-later — see
  the top-level [`COPYING`](../../COPYING) file) and for how the raw biquad
  upload API works.

## Amp enable / PDN

- **GPIO 33** drives the TAS5825M PDN line. Firmware holds it low during
  I2S bring-up, then transitions the DAC HI-Z → PLAY inside
  `geelen_hal::dacAmpEnable(true)`.
- The PDN sequence is not the same as the Amped GPIO-13 enable; it runs the
  TAS5825M control-state machine (`TAS5805M_CTRL_HIZ` → `TAS5805M_CTRL_PLAY`)
  so the DAC unmutes deterministically.

## EQ backend

- **Hardware** parametric EQ, 15 bands (see [`docs/eq.md`](../eq.md)).
- The firmware pushes user bands into the DAC by calling
  `geelen_hal::dacApplyPeqBands` on every `POST /api/eq`. Each band is
  expanded to 1–2 biquad stages, coefficients are converted to Q1.31, and
  the resulting 15 slots are uploaded to Book 0x8C, pages 0x2C..0x37 (L)
  and 0x38..0x43 (R).
- Fallback: if the upload fails (I2C NAK, page-switch mismatch), the
  AudioEngine backend flips to software and the SPA surfaces
  `state.eq.backend = "software (fallback)"`.

## WS2812B (optional)

- **GPIO 12** — same Sonocotta `RGB OUT` as Amped-WROVER. It is MTDI
  (flash-voltage strapping); idle-low is safe at reset. Firmware inits
  the strip in `app_main`. `HAL_WS2812_COUNT` default 1;
  `-DHAL_WS2812_COUNT=N` for a daisy-chain. `HAL_WS2812_MAX_DUTY` default
  50; `-DHAL_WS2812_MAX_DUTY=N`. RMT takes the full 512-symbol ESP32
  SRAM block; see [hal.md](../hal.md) (WS2812 RMT memory).

## Thermal / power notes

- The TAS5825M has a die-temperature fault register. Surfacing it as
  `state.dac.fault` is on the Phase 10 roadmap.
- Recommended supply: 12–24 V for the amp rail (see the TAS5825M datasheet).
  The MCU has its own onboard step-down.

## Flashing

```bash
pio run -e louder-esp32-idf -t upload
pio run -e louder-esp32-idf -t uploadfs
```

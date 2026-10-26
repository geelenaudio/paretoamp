# Hardware — Louder-ESP32-Plus (WROVER-N8R8)

TAS58xx Class-D amplifier with on-chip 15-band DSP. This board unlocks
hardware volume (`0x4C`) and the hardware PEQ path (`HAL_HAS_HW_PEQ=1`).
Louder-ESP32 (TAS5805M) and Louder-ESP32-Plus (TAS5825M) share this image
when the pins match; firmware probes `DIE_ID` (`0x95` = TAS5825M).

Primary datasheet: [tas5825m.pdf](tas5825m.pdf) (SLASEH7H). Do not copy
[slaa847.pdf](slaa847.pdf) EEPROM-SPI GPIO mux (`0x61`/`0x60`) into
`enter_play`. [sbaa414.pdf](sbaa414.pdf) is PCM ADC Q1.31 — wrong silicon.

> Verify pin choices against your Louder-ESP32-Plus schematic revision.

## MCU

- **ESP32-WROVER-N8R8** — same as Amped-WROVER.

## I2S (DAC)

- **TAS5805M or TAS5825M** — stereo I2C-controlled Class-D DAC + power stage
  with on-chip 15-band biquad EQ. Plus boards ship TAS5825M.
- I2S pinout (Sonocotta Louder-ESP32-Plus, ESP32):
  - **BCK** — GPIO 26
  - **WS**  — GPIO 25
  - **DATA** — GPIO 22
- Uses the new I2S driver (`driver/i2s_std.h`). 16-bit Philips I2S; BCLK is
  32-FS on ESP32 I2S HW v1. Speaker rail 12–26 V is required (`PVDD_ADC`);
  USB does not power the Class-D.

## DAC control (TAS58xx)

- I2C bus at 400 kHz on SDA=GPIO 21, SCL=GPIO 27.
- 7-bit address `0x4C` (ADR pin low). Set `HAL_TAS5825M_I2C_ADDR` in
  `src/hal/boards/louder_wrover.h` if your board straps ADR high.
- The DAC is controlled through the vendored sonocotta driver at
  [`components/tas5805m/`](../../pareto-amp/components/tas5805m/).
  Refer to that component's README for licensing (GPL-3.0-or-later — see
  the top-level [`COPYING`](../../COPYING) file).
- Bring-up follows datasheet §9.5.3: I2S stable → Hi-Z + DSP enable →
  ≥5 ms → host BQ RAM (identity or saved curve) → `USE_DEFAULT_COEFFS=0`
  → PLAY. Volume and EQ then change on the fly without a PDN cycle.

## Amp enable / PDN

- **GPIO 33** drives the TAS PDN line. Firmware raises it before I2C
  (the slave is dead while PDN is low), then transitions HI-Z → PLAY inside
  `geelen_hal::dacAmpEnable(true)` after I2S clocks are stable.
- The PDN sequence is not the same as the Amped GPIO-13 enable; analog unmute
  is the TAS control-state machine (`TAS5805M_CTRL_HIZ` →
  `TAS5805M_CTRL_PLAY`), not the GPIO edge. PDN wipes DSP coefficient RAM,
  so the next enable reloads identity then the user curve.

## Volume

- User fader is TAS digital volume `0x4C` (+24 dB to −103 dB, 0.5 dB steps,
  reset `0x30` = 0 dB). Firmware maps 0–100% like software amplitude
  (`dB = 20*log10(pct/100)`, mute at 0) and never boosts above 0 dB.
  Encoder, SPA, and phone AVRCP all call the same apply path so the 0–100%
  value stays in sync. `AudioEngine` gain stays at unity.
- Leave ramp `0x4E` at default (clickless). Do not use analog `AGAIN` as
  the user fader. Headroom/limiter stay in the engine (pre-I2S).

## EQ backend

- **Hardware** parametric EQ, 15 bands per channel (see [`docs/eq.md`](../eq.md)).
- Coefficients are Q5.27 with negated textbook `a1`/`a2`. Book **0xAA**,
  not Book 0x8C pages 0x2C–0x43. TAS5825M left BQ1 is page `0x01` offset
  `0x30`; TAS5805M left BQ1 is page `0x24` offset `0x18`. Right banks are
  independent; ganged mode still writes both tables and sets TAS5825M
  gang=1. Split mode sets gang=0.
- EQ on/off: TAS5825M Book 0x8C page `0x0B` gang/bypass; TAS5805M
  `DSP_MISC` `0x66` (independent RAM, no gang bit).
- Live edits stay in PLAY (bypass → RAM rewrite → un-bypass). Fallback: if
  the upload fails, the AudioEngine backend flips to a **ganged** software
  cascade on the left curve and the SPA surfaces
  `state.eq.backend = "software (fallback)"`.

## WS2812B

- **GPIO 12** — Sonocotta `RGB OUT` (MTDI / flash-voltage strapping). Idle-low
  is safe at reset. Firmware inits the strip in `app_main`.
- Default **8** pixels (`HAL_WS2812_COUNT`); override with
  `-DHAL_WS2812_COUNT=N`. `HAL_WS2812_MAX_DUTY` default 50;
  `-DHAL_WS2812_MAX_DUTY=N`. RMT takes the full 512-symbol ESP32 SRAM block;
  see [hal.md](../hal.md) (WS2812 RMT memory).
- Volume/mute shows a **white bar** for ~2 s (full LEDs = percent, last pixel
  scaled by remainder; mute or 0% = all off), then the user NVS colour or the
  pairing blue pulse. Boot red and the STA green breath are not overlaid.

## Rotary encoder (volume / mute / pair)

Front-panel Bourns PEC11R (or compatible). **GPIO 13 / 14 / 35 are free on
this board** because amp PDN is GPIO 33, not GPIO 13.

| Signal | GPIO | Notes |
| ------ | ---- | ----- |
| A      | 35   | Input-only; external 10k to 3.3V + 0.1µF to GND |
| B      | 14   | External 10k to 3.3V + 0.1µF to GND |
| SW     | 13   | Active-LOW; same RC as A/B |

Firmware: `GPIO_MODE_INPUT`, **no** internal pull-up/down. Hardware RC is the
debounce. Common of A/B/SW to GND.

- One detent → ±1% ganged L/R volume (0–100), same path as the phone slider.
- Switch **release under 1 s** → mute toggle.
- Switch **hold ≥ 1 s** → disconnect A2DP if connected, then 180 s pairing
  window. Does **not** wipe Bluetooth bonds (Reset → Bluetooth still does).
- Encoder is compile-gated (`HAL_HAS_ENCODER=1`). Do not copy these pins onto
  Amped-WROVER: GPIO 13 is TPA3128 amp-enable there.

## Thermal / power notes

- The TAS5825M has a die-temperature fault register. Surfacing it as
  `state.dac.fault` is on the Phase 10 roadmap.
- Recommended supply: 12–24 V for the amp rail (see the TAS5825M datasheet).
  The MCU has its own onboard step-down.

## Flashing

From the repo root: `task fw:louder`. Raw PlatformIO:

```bash
pio run -e louder-esp32-idf -t upload
pio run -e louder-esp32-idf -t uploadfs
```

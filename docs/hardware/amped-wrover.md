# Hardware — Amped-ESP32 (WROVER-N8R8)

The historical / default target for this firmware. `pio run -e amped-esp32-idf`
produces this build; the Amped-WROVER is the only board the refactor validated
end-to-end on `/dev/ttyUSB0`.

## MCU

- **ESP32-WROVER-N8R8** — dual-core 240 MHz, 8 MB Flash, 8 MB PSRAM.
- Bluetooth is **4.2** (BR/EDR + BLE) — not 5.0. A2DP sink works.

## I2S (DAC)

- **PCM5100A** — 32-bit stereo DAC, I2S input only, no I2C control path.
- Pinout (see `src/hal/boards/amped_wrover.h`):
  - **BCK** — GPIO 26
  - **WS (LRCK)** — GPIO 25
  - **DATA (DOUT)** — GPIO 22
- Firmware uses the new I2S driver (`driver/i2s_std.h`, channel-based API).

## Amp enable

- **GPIO 13** drives the TPA3128D2 enable / mute pin.
- The pin is HIGH only after I2S is initialised and the clock is stable to
  avoid the power-on pop the amp otherwise emits (see
  `amp_enable_after_i2s_stable` in `src/idf_main.cpp` and
  `HAL_I2S_STABLE_MS` in `src/hal/boards/amped_wrover.h`).

## EQ backend

- **Software** parametric EQ, 10 bands (see the SPA `/dsp` page).
- Cascade lives in `AudioEngine::processSamples` (biquads + first-order
  LP/HP). Preset storage is 15 bands so a preset created on Louder round-
  trips through Amped without losing metadata; only the first 10 are pushed
  to the DSP.
- Related file: [`docs/eq.md`](../eq.md).

## PSRAM

- 8 MB external PSRAM used for audio ring buffers, cJSON allocations, and
  SSE frame staging so playback stays smooth under Wi-Fi + web load.
- GPIO 16 and 17 are reserved for PSRAM on the WROVER module and MUST NOT be
  used by application code.

## Power

- The amp is fed from an external supply (12 V typical).
- MCU is powered through an onboard step-down.
- Do not drive headphones and speakers simultaneously unless the board is
  modified.

## WS2812B (optional)

- **GPIO 12** is Sonocotta's `RGB OUT` on every ESP32 dock (same pin as
  Louder-Plus). It is a strapping pin (MTDI / flash voltage): the line
  idles low, which is safe at reset. Firmware initialises the strip in
  `app_main`, never in the bootloader.
- Pixel count is compile-time `HAL_WS2812_COUNT` (default **12** on this
  board). A longer daisy-chain is a rebuild: `-DHAL_WS2812_COUNT=N` on
  the PIO env, not a SPA slider. Peak brightness is compile-time
  `HAL_WS2812_MAX_DUTY` (default **10**); `-DHAL_WS2812_MAX_DUTY=N`.
  RMT uses the full 512-symbol ESP32 SRAM block so BT on Core 0 cannot
  underrun a 12–50 LED frame mid-strip — see [hal.md](../hal.md)
  (WS2812 RMT memory).
- Boot: slow red pulse until STA, one green breath, blue pulse while
  Bluetooth is unpaired, then user colour/brightness from Overview
  (`POST /api/led`). All peaks are capped by max duty.

## Flashing this variant

```bash
pio run -e amped-esp32-idf -t uploadfs   # SPIFFS assets
pio run -e amped-esp32-idf -t upload      # firmware
pio device monitor -e amped-esp32-idf
```

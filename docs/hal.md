# HAL — Hardware Abstraction Layer

The firmware is compiled from a single tree that targets four boards
(see [`hardware/`](hardware/README.md)). The differences between those
boards — pin numbers, DAC capabilities, whether the amp is controlled via a
GPIO or via a TAS5825M state machine — are captured in the
[`src/hal/`](../ESP32_AudioPlatform/src/hal/) layer at compile time.

## Layering

```
+----------------------------------------------+
|  Application code                            |
|  (AudioEngine, WifiManager, HTTP server, …)  |
+----------------------------------------------+
                    |
                    v
+----------------------------------------------+
|  geelen_hal::* (src/hal/dac.h, src/hal/led.h) |
|  - dacInit / dacAmpEnable / dacSetVolume     |
|  - dacHasHwVolume / dacSupportsHwPeq / dacApplyPeqBands / dacApplyPeqBandsLR |
|  - ledInit / ledSetRgb / ledSetPixel / ledRefresh |
+----------------------------------------------+
                    |
        picked at build time
                    v
+---------------------+----------------------+
| dac_none.cpp        | dac_tas5825m.cpp     |
| (Amped: PCM5100A)   | (Louder: TAS5825M)   |
+---------------------+----------------------+
                    |
                    v
+----------------------------------------------+
|  src/hal/hal_board.h                         |
|  #if BOARD_AMPED_WROVER  → amped_wrover.h    |
|  #elif BOARD_AMPED_S3    → amped_s3.h        |
|  #elif BOARD_LOUDER_...  → louder_*.h        |
+----------------------------------------------+
```

## What each board header defines

| Symbol                       | Purpose                                          |
| ---------------------------- | ------------------------------------------------ |
| `HAL_BOARD_NAME`             | Human-readable string used in `/api/system/identity`. |
| `HAL_PIN_I2S_{BCK,WS,DATA}`  | I2S output pins for the DAC.                     |
| `HAL_HAS_AMP_EN`, `HAL_PIN_AMP_EN`, `HAL_I2S_STABLE_MS` | Amp-enable GPIO + settle time. |
| `HAL_HAS_HW_PEQ`             | Whether the DAC has on-chip PEQ we should push to. |
| `HAL_HAS_USB_AUDIO`          | Native USB PHY for UAC playback (1 on S3, 0 on WROVER UART-bridge). |
| `HAL_HAS_I2C_DAC`            | Whether a `dac_tas5825m` implementation is present. |
| `HAL_EQ_MAX_BANDS_ACTIVE`    | Runtime cap for the EQ (10 on Amped, up to 15 on Louder). |
| `HAL_HAS_WS2812`             | 1 if this board has an optional WS2812B data pin. |
| `HAL_PIN_WS2812`             | Data GPIO (see table below). Initialise in `app_main`, not the bootloader. |
| `HAL_WS2812_COUNT`           | Compile-time pixel count (1–64). Per-board header default (see table); override with `-DHAL_WS2812_COUNT=N`. Not NVS, not `POST /api/led`. |
| `HAL_WS2812_MAX_DUTY`        | Peak electrical brightness (1–100%). Per-board header default (see table); override with `-DHAL_WS2812_MAX_DUTY=N`. Applied in `ledSetRgb` / `ledSetPixel`; not NVS, not API. |
| `HAL_HAS_ENCODER`            | 1 if this board has a front-panel rotary encoder. Default 0. |
| `HAL_PIN_ENC_{A,B,SW}`       | Encoder GPIOs when `HAL_HAS_ENCODER=1`. Input, no internal pulls, active-LOW. |
| `HAL_PIN_I2C_{SDA,SCL}`, `HAL_I2C_FREQ_HZ` (Louder only) | I2C bus for DAC control. |
| `HAL_TAS5825M_I2C_ADDR` (Louder only) | 7-bit DAC address. |

Amped (`HAL_HAS_I2C_DAC=0`) keeps software volume and software PEQ.
`dacHasHwVolume()` / `dacSupportsHwPeq()` are always false. Louder uses the
TAS58xx as amp + fader + EQ: engine gain stays at unity, `dacSetVolume`
writes register `0x4C` with an amplitude-style dB map, and PEQ is Q5.27 in
Book 0xAA (independent left and right tables). `dacApplyPeqBandsLR` writes
those banks and sets TAS5825M Book 0x8C **gang=1** when ganged, **gang=0**
when split. TAS5805M `DSP_MISC` is unchanged (RAM is already per-channel).
Runtime `DIE_ID` (`0x95` = TAS5825M, otherwise TAS5805M) picks
the biquad page map so one WROVER image covers Louder-ESP32 and
Louder-ESP32-Plus when the pins match.

## WS2812B status LED

Sonocotta lists RGB as an optional peripheral on every dock. Firmware drives
it with ESP-IDF `led_strip` (RMT, GRB). Unpopulated parts are harmless:
RMT writes go nowhere.

| Board            | `HAL_PIN_WS2812` | Default `HAL_WS2812_COUNT` | Default `HAL_WS2812_MAX_DUTY` |
| ---------------- | ---------------- | -------------------------- | ----------------------------- |
| Amped-WROVER     | GPIO 12          | 12                         | 10                            |
| Louder-WROVER    | GPIO 12          | 8                          | 50                            |
| Amped-S3         | GPIO 9           | 1                          | 50                            |
| Louder-S3        | GPIO 21          | 1                          | 50                            |

GPIO 12 is ESP32 MTDI (flash-voltage strapping). WS2812 idle is low, which
is safe at reset. Do not hold the line high in the bootloader.

Count is a firmware-image constant (1–64). A daisy-chain on the same data
pin is a rebuild, not an NVS or SPA setting — the protocol cannot probe
length. A longer strip is `-DHAL_WS2812_COUNT=N` (e.g. **35**), not a
larger RMT buffer — 512 symbols is already the ESP32 SRAM cap. Override
without editing the board header:

```
build_flags = -DBOARD_AMPED_WROVER=1 -DHAL_WS2812_COUNT=35
```

Each board header wraps the defaults in `#ifndef HAL_WS2812_COUNT` /
`#ifndef HAL_WS2812_MAX_DUTY` so those flags win. `hal_board.h` `#error`s
if count is outside 1..64 or max duty is outside 1..100.
`ledSetRgb` scales every channel by max duty, so Overview 100% is “100%
of the firmware cap”, not full 255. Status colours (boot red, one green
breath on STA, pairing blue) are a ~2 s sine from 0 to that cap — not
on/off flashes. Volume/mute paints a white per-pixel bar for ~2 s
(`ledSetPixel` + `ledRefresh`), then the current phase resumes.

### RMT memory (do not use the IDF default of 64)

WS2812 is 800 kbit/s. The RMT peripheral plays a small RAM FIFO of
level/duration **symbols** (two symbols per bit). The CPU must refill
that FIFO before the data pin has been idle for ~50 µs, or the strip
treats the gap as RESET: the first pixels latch, the rest of the frame
is eaten as a new frame (wrong colours on later LEDs). The next good
refresh snaps them back.

IDF’s default `mem_block_symbols = 64` is ~1.3 pixels of waveform
(~40 µs of slack). Bluetooth Classic on Core 0 regularly turns the
flash cache off; that stall misses the deadline. The encoder ISR then
retries, coexist/BT work piles up, and Core 0 looks 70–90% busy even
though I2S/PEQ run on Core 1.

Firmware therefore takes **all ESP32 RMT SRAM: 512 symbols** (8 blocks
× 64) in [`led_ws2812.cpp`](../ESP32_AudioPlatform/src/hal/led_ws2812.cpp)
— about 10 pixels of slack. That is the hardware cap. A 50-LED frame
still needs ~2400 symbols, so the driver will always ping-pong; 512
only makes each refill rarer. Do not compute `count × 24 × 2` and
allocate that — `rmt_new_tx_channel` will fail.

ESP32-S3 RMT blocks are 48 symbols; this image uses 192 (4 blocks).
Long S3 strips should set `with_dma` rather than asking for more SRAM.

Serial log line: `WS2812 GPIO … rmt_mem=512` (or `192` on S3).

`state.led.count` is read-only so the SPA can label “N pixels”; Overview
sliders are colour and brightness only (`POST /api/led`). Max duty is not
in the API.

## Rotary encoder

Compile-gated per board. Only **Louder-WROVER** ships with pins:

| Board            | `HAL_HAS_ENCODER` | A     | B     | SW    |
| ---------------- | ----------------- | ----- | ----- | ----- |
| Louder-WROVER    | 1                 | GPIO 35 (input-only) | GPIO 14 | GPIO 13 |
| Amped-WROVER     | 0                 | —     | —     | GPIO 13 is `HAL_PIN_AMP_EN` |
| Amped-S3         | 0                 | —     | —     | until a pinout exists |
| Louder-S3        | 0                 | —     | —     | until a pinout exists |

External 10k pull-ups to 3.3V and 0.1µF to GND on A, B, and SW. Firmware
uses `GPIO_MODE_INPUT` with **no** internal pull-up/down. Do not enable
the encoder on Amped-WROVER without moving the switch off GPIO 13.

One detent is ±1% ganged volume. Switch release under 1 s toggles mute;
hold ≥ 1 s disconnects A2DP if needed and opens the 180 s pairing window
without wiping bonds.

## Why compile-time and not runtime?

Runtime discovery ("read some ID register at boot and pick a driver") is
tempting but risky here: the WROVER and S3 packages have different pin lists,
so misdetection would flash the wrong DAC config into a TAS5825M. Trust the
env you picked. If you flashed the wrong env, the boot banner tells you —
`HAL_BOARD_NAME` is logged early in `app_main`.

## Adding a new board

1. Create `src/hal/boards/<name>.h` mirroring one of the existing files.
2. Add a `#elif defined(BOARD_<NAME>) …` case in
   [`src/hal/hal_board.h`](../ESP32_AudioPlatform/src/hal/hal_board.h).
3. Add a matching `[env:<name>]` block in `platformio.ini` with
   `build_flags = -DBOARD_<NAME>=1`.
4. Ship a `sdkconfig.<env>` next to the existing ones.
5. Populate `docs/hardware/<name>.md` (linked from
   [`docs/hardware/README.md`](hardware/README.md)).

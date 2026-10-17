# Geelen Audio Amp – Documentation

[![PlatformIO CI](https://github.com/geelenaudio/paretoamp/actions/workflows/platformio.yml/badge.svg)](https://github.com/geelenaudio/paretoamp/actions/workflows/platformio.yml)

Firmware for four boards in one tree: **Amped-ESP32** (WROVER-N8R8, PCM5100A),
**Amped-ESP32-S3**, **Louder-ESP32-Plus** (WROVER-N8R8 + TAS58xx),
**Louder-ESP32-S3**. Pinouts and HAL notes:
[docs/hardware/README.md](docs/hardware/README.md),
[docs/board_variants.md](docs/board_variants.md).

**Licence:** GPL-3.0-or-later — [COPYING](COPYING),
[docs/licensing.md](docs/licensing.md).

## Flash

The PlatformIO env is compile-time. An Amped image never talks to the TAS, so
flashing it onto a Louder board makes EQ stay **software**. Use a named task:

| Board | Flash (app + SPIFFS) | PIO env |
| ----- | -------------------- | ------- |
| Amped-ESP32 (WROVER) | `task fw:amped` | `amped-esp32-idf` |
| Louder-ESP32-Plus (WROVER) | `task fw:louder` | `louder-esp32-idf` |
| Amped-ESP32-S3 | `task fw:amped-s3` | `amped-esp32s3-idf` |
| Louder-ESP32-S3 | `task fw:louder-s3` | `louder-esp32s3-idf` |

```sh
task fw:targets                          # board → env cheat sheet
task fw:ports                            # serial devices
task fw:louder                           # WROVER + TAS58xx
task fw:louder PIO_PORT=/dev/ttyACM0     # S3 / other USB CDC port
task fw:monitor PIO_ENV=louder-esp32-idf
```

`task fw:upload` / `fw:web` / `fw:all` still default to **Amped**. Both of these
select Louder on the generic tasks:

```sh
task fw:upload PIO_ENV=louder-esp32-idf
PIO_ENV=louder-esp32-idf task fw:upload
```

Each flash prints `env=` and `port=` before esptool runs. Wrong env names are
rejected before a write.

Confirm the image after reset:

- Amped: `DAC/none: DAC (Amped-…) init — software PEQ backend`
- Louder: `TAS5825M detected` (or TAS5805M) and `HW PEQ uploaded` when EQ is on

`task` finds `pio` on PATH, otherwise `~/.platformio/penv/bin/pio`. Run from the
repo root (the directory with `Taskfile.yml`). Equivalent raw PlatformIO:

```sh
cd ESP32_AudioPlatform
pio run -e louder-esp32-idf -t upload --upload-port /dev/ttyUSB0
pio run -e louder-esp32-idf -t uploadfs --upload-port /dev/ttyUSB0
```

## Contents

- [Hardware variants](docs/hardware/README.md) – four boards, pinouts, DAC
- [EQ](docs/eq.md) – single-layer PEQ, software vs TAS5825M hardware
- [User guide](docs/user-guide.md) – Setup, WiFi/AP, Bluetooth, Snapcast, EQ, Web UI
- [Development](docs/development.md) – Architecture, modules, build, PlatformIO
- [API reference](docs/api.md) – REST endpoints for the Web UI
- [Manual test matrix](docs/manual-test-matrix.md) – Quick bring-up checks
- [Roadmap](docs/roadmap.md) – deferred follow-ups

## Overview

The Geelen Audio Amp is a networked I2S audio solution with:

- **Bluetooth A2DP Sink** – stream from phone/tablet (Classic BT on ESP32-WROVER)
- **USB audio** – S3 boards enumerate as a UAC playback gadget (computer sound card). WROVER USB is UART-only. See [docs/audio_modes.md](docs/audio_modes.md)
- **Network (Snapclient)** – Snapcast client; pair with `snapserver` + `librespot`
- **Parametric EQ** – Amped: 10-band software PEQ + software volume, **always ganged** (one curve on both channels). Independent L/R software curves are out of scope: the stereo cascade already fills most of Core 1 on Amped-WROVER. Louder: 15-band TAS hardware PEQ per channel + hardware volume, optional Unlink. Details: [docs/eq.md](docs/eq.md).
- **Web control panel** – volume, source, EQ, WiFi setup, factory reset
- **WiFi** – validate-then-commit onboarding, STA with AP fallback, captive portal at 192.168.4.1

On-device Spotify Connect is **not** supported; use the Snapcast + librespot path above.

## CI

Builds run on **GitHub Actions** (`.github/workflows/platformio.yml`): on push/PR to `main` or `master`, the workflow installs PlatformIO and runs `pio run -e amped-esp32-idf` in the `ESP32_AudioPlatform` project directory.

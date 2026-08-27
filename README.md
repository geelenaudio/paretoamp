# Geelen Audio Amp – Documentation

[![PlatformIO CI](https://github.com/psmgeelen/ESP32_AudioPlatform/actions/workflows/platformio.yml/badge.svg)](https://github.com/psmgeelen/ESP32_AudioPlatform/actions/workflows/platformio.yml)

Documentation for the Geelen Audio firmware. Supports four boards from a
single tree: **Amped-ESP32** (WROVER-N8R8, reference), **Amped-ESP32-S3**
(WROOM-N8R8), **Louder-ESP32-Plus** (WROVER-N8R8 + TAS5825M),
**Louder-ESP32-S3** (WROOM-N8R8 + TAS5825M). See
[docs/hardware/README.md](docs/hardware/README.md) and
[docs/board_variants.md](docs/board_variants.md).

**Licence:** GPL-3.0-or-later — see [COPYING](COPYING) and
[docs/licensing.md](docs/licensing.md).

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
- **Network (Snapclient)** – Snapcast client; pair with `snapserver` + `librespot`
- **Parametric EQ** – one curve; 10-band software PEQ on Amped, 15-band hardware PEQ on Louder (TAS5825M)
- **Web control panel** – volume, source, EQ, WiFi setup, factory reset
- **WiFi** – validate-then-commit onboarding, STA with AP fallback, captive portal at 192.168.4.1

On-device Spotify Connect is **not** supported; use the Snapcast + librespot path above.

Default on-device target during this refactor: **Amped-ESP32 (WROVER-N8R8)** with PCM5100A DAC and TPA3128 amp (`/dev/ttyUSB0`).

## CI

Builds run on **GitHub Actions** (`.github/workflows/platformio.yml`): on push/PR to `main` or `master`, the workflow installs PlatformIO and runs `pio run -e amped-esp32-idf` in the `ESP32_AudioPlatform` project directory.

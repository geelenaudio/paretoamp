# Geelen Audio Amp – Documentation

[![PlatformIO CI](https://github.com/psmgeelen/ESP32_AudioPlatform/actions/workflows/platformio.yml/badge.svg)](https://github.com/psmgeelen/ESP32_AudioPlatform/actions/workflows/platformio.yml)

Documentation for the Geelen Audio Amp firmware (Amped-ESP32 non-plus, non-S).

## Contents

- [Hardware](docs/hardware.md) – Amped-ESP32 pins, GPIO 13, I2S, PSRAM
- [User guide](docs/user-guide.md) – Setup, WiFi/AP, Bluetooth, Snapcast, EQ, Web UI
- [Development](docs/development.md) – Architecture, modules, build, PlatformIO
- [API reference](docs/api.md) – REST endpoints for the Web UI
- [Manual test matrix](docs/manual-test-matrix.md) – Quick bring-up checks for all sources + Web UI

## Overview

The Geelen Audio Amp is a networked I2S audio solution with:

- **Bluetooth 4.2 A2DP Sink** – stream from phone/tablet
- **Network (Snapclient)** – Snapcast client; pair with `snapserver` + `librespot` on a Pi/PC to play Spotify on the network
- **10-band software EQ** – fixed presets + custom slots, persisted to flash
- **Web control panel** – volume, source, EQ, WiFi setup, factory reset
- **WiFi** – Station mode with AP fallback and captive portal at 192.168.4.1

On-device Spotify Connect is **not** supported; use the Snapcast + librespot path above.

Target hardware: **Amped-ESP32 (non-plus, non-S)** with PCM5100A DAC and TPA3128 amp.

## CI

Builds run on **GitHub Actions** (`.github/workflows/platformio.yml`): on push/PR to `main` or `master`, the workflow installs PlatformIO and runs `pio run -e amped-esp32-idf` in the `ESP32_AudioPlatform` project directory.

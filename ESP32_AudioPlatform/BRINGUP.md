# ESP32_AudioPlatform bring-up checklist (Amped-ESP32, ESP-IDF)

## Serial logging
- Named flash from repo root: `task fw:amped` (or `task fw:louder` on TAS boards)
- Build/upload: `pio run -e amped-esp32-idf -t upload`
- Monitor: `pio device monitor -e amped-esp32-idf` (Louder: `-e louder-esp32-idf`)
- If you see "nothing", press **RESET** once while the monitor is open.

Expected early output:
- ROM boot text (`ets Jul 29 2019 ...`)
- `Geelen Audio Amp – ESP-IDF env boot`

## First boot after partition changes
- Erase once: `pio run -e amped-esp32-idf -t erase`
- Upload again.

Expected:
- No `esp_littlefs: partition "spiffs" could not be found` errors.

## Network / UI
- If STA credentials are not configured, device should start AP:
  - SSID: `Geelen-Audio` (open, no password)
  - IP: `192.168.4.1`
- Visit `/` and `/api/status` in a browser.

## Louder / S3 bring-up (build-validated)

S3 and Louder envs compile with `pio run -e amped-esp32s3-idf` (etc). Pin
maps are DevKitC-1 / sonocotta defaults — confirm the schematic before
flashing real hardware. Louder boot should log `DAC (Louder-…) init` and
`TAS5825M detected`. See [docs/board_variants.md](../docs/board_variants.md)
and [docs/hardware/README.md](../docs/hardware/README.md).

## Audio
- I2S init logs use HAL pins (Amped-WROVER: WS=25, BCK=26, DATA=22)
- Amp enable should log `Amp enable (GPIO 13) HIGH` on Amped
- If source is Bluetooth, you should see `a2dp_pump up` then the A2DP sink start.

## Troubleshooting signals
- Capture free heap / PSRAM early in boot if you suspect memory pressure.
- If BT fails to start, try switching source to Network (Snapcast) to confirm whether the pressure is BT-specific.

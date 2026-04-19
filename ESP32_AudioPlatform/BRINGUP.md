# ESP32_AudioPlatform bring-up checklist (Amped-ESP32, ESP-IDF)

## Serial logging
- Build/upload: `pio run -e amped-esp32-idf -t upload`
- Monitor: `pio device monitor -e amped-esp32-idf`
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
  - SSID: `Geelen-Audio`
  - Pass: `geelen123`
  - IP: `192.168.4.1`
- Visit `/` and `/api/status` in a browser.

## Audio
- I2S init logs (WS=25, BCK=26, DATA=22)
- Amp enable should log `Amp enable (GPIO 13) HIGH`
- If source is Bluetooth, you should see the A2DP sink start.

## Troubleshooting signals
- Capture free heap / PSRAM early in boot if you suspect memory pressure.
- If BT fails to start, try switching source to Network (Snapcast) to confirm whether the pressure is BT-specific.

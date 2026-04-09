# ESP32_AudioPlatform bring-up checklist (Amped-ESP32)

## Serial logging
- Build/upload: `pio run -e amped-esp32 -t upload`
- Monitor: `pio device monitor -e amped-esp32`
- If you see “nothing”, press **RESET** once while the monitor is open.

Expected early output:
- ROM boot text (`ets Jul 29 2019 ...`)
- `Geelen Audio Amp – Amped-ESP32`

## First boot after partition changes
- Erase once: `pio run -e amped-esp32 -t erase`
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
- `I2S OK (WS=25 BCK=26 DATA=22)`
- Amp enable should log `Amp enable (GPIO 13) HIGH`
- If source is Bluetooth, you should see:
  - `A2DP Sink started (Geelen Audio). Connect from phone.`

## Troubleshooting signals
- If you see `[AsyncTCP.cpp:...] begin(): failed to start task`:
  - Capture the memory stats lines printed by the firmware (`[boot]`, `[pre_web]`, etc.).
  - Try starting without Bluetooth (set source to Network, or temporarily disable A2DP) to confirm heap pressure.


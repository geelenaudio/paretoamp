# Hardware – Amped-ESP32 (non-plus, non-S)

## MCU

- **ESP32-WROVER-N8R8** – dual-core 240 MHz, 8 MB Flash, 8 MB PSRAM
- Not ESP32-S3; Bluetooth is **4.2** (BR/EDR + BLE), not 5.0

## I2S (DAC)

- **PCM5100A** – 32-bit stereo DAC, I2S only (no I2C)
- Pinout:
  - **BCLK** – GPIO 26
  - **LRCK (WS)** – GPIO 25
  - **DOUT (DATA)** – GPIO 22

## Amp enable (Rev H+)

- **GPIO 13** – Amp enable for TPA3128D2
- Firmware drives this **HIGH** only after I2S is initialized and the clock is stable to avoid pops/clicks on boot.

## PSRAM

- 8 MB external PSRAM used for audio ring buffers so playback stays smooth under WiFi/Web load.
- GPIO 16 and 17 are reserved for PSRAM on the WROVER module.

## Power

- Amp is fed from an external supply; the MCU uses an onboard step-down. Do not drive headphones and speakers simultaneously unless the board is modified.

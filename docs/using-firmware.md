# Using the firmware

You do not need to build anything. Each [GitHub Release](https://github.com/geelenaudio/paretoamp/releases)
attaches one factory image per board: bootloader, partition table, app, and
the web UI. Flash that file at offset `0x0` and the board is a Pareto Amp.

To change the code instead, see [Development](development.md) and
[Contributing](../CONTRIBUTING.md).

## 1. Pick the file for your board

| Board | SoC | File | `--chip` |
| ----- | --- | ---- | -------- |
| Amped-ESP32 | ESP32-WROVER | `pareto-amp-amped-esp32-vX.Y.Z.bin` | `esp32` |
| Louder-ESP32 / Plus | ESP32-WROVER + TAS58xx | `pareto-amp-louder-esp32-vX.Y.Z.bin` | `esp32` |
| Amped-ESP32-S3 | ESP32-S3 | `pareto-amp-amped-esp32s3-vX.Y.Z.bin` | `esp32s3` |
| Louder-ESP32-S3 | ESP32-S3 + TAS58xx | `pareto-amp-louder-esp32s3-vX.Y.Z.bin` | `esp32s3` |

`X.Y.Z` is the release tag without the `v` (About on the device shows the
same string). Amped and Louder images are **not** interchangeable: an Amped
image never talks to the TAS, so EQ stays software if you flash it onto a
Louder board.

Pinouts: [hardware](hardware/README.md).

## 2. Install esptool

Python 3, then:

```sh
pip install esptool
esptool version
```

The command is `esptool` (not `esptool.py`). You also need a USB data cable
and a serial port: typically `/dev/ttyUSB0` (WROVER), `/dev/ttyACM0` (S3),
`/dev/cu.usbserial-*` (macOS), or `COMx` (Windows).

## 3. Flash at `0x0`

A factory write **erases NVS**: Wi-Fi, EQ, presets, device name. That is
what you want on a new board or after a messy upgrade. To update app +
website and **keep** NVS, clone the matching tag and run `task fw:flash`
instead ([Development](development.md)).

Put the S3 into download mode if the port does not appear: hold **BOOT**,
tap **RESET**, release **BOOT**. WROVER boards usually reset themselves
over USB-serial.

```sh
# ESP32 (Amped or Louder WROVER)
esptool --chip esp32 --port /dev/ttyUSB0 write-flash 0x0 pareto-amp-amped-esp32-vX.Y.Z.bin

# ESP32-S3 (Amped or Louder S3)
esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0x0 pareto-amp-amped-esp32s3-vX.Y.Z.bin
```

Replace the filename with the Louder image when that is your hardware.
When it finishes, tap RESET (or power-cycle).

## 4. First boot

1. After a few seconds you should hear **Wi-Fi not connected** and a mode
   clip (Bluetooth / Snapcast / …), unless voice feedback is off.
2. Join the open hotspot **Geelen-Audio** (no password).
3. The phone often pops a captive sheet. If it does not, open
   [http://192.168.4.1/](http://192.168.4.1/).
4. Pick your home Wi-Fi, enter the password, save. Wrong passwords stay on
   the portal; a success reboots onto the LAN.

Then open `http://geelen-audio.local/` (or the DHCP IP). The device
advertises Bluetooth as **Pareto Amp**. Full walkthrough: [Operating
guide](operating_guide.md).

## After that

| Want | Where |
| ---- | ----- |
| Volume, source, EQ, identity | Web UI on the LAN |
| USB sound card (S3 only) | Mode → USB audio ([audio modes](audio_modes.md)) |
| Spotify / multi-room | Snapcast on the LAN, not on-device Connect |
| Serial log | 115200 8N1 on the same USB port |
| Bug or idea | [Contributing](../CONTRIBUTING.md) |

# Board variants

Four PlatformIO environments share one firmware tree. The compile-time HAL
under `ESP32_AudioPlatform/src/hal/` selects pin maps, DAC drivers, and the
EQ backend from the `BOARD_*` symbol passed by the active env.

| Product                | SoC               | PIO env              | `BOARD_*`             | Volume + EQ                                              |
| ---------------------- | ----------------- | -------------------- | --------------------- | -------------------------------------------------------- |
| Amped-ESP32            | ESP32-WROVER-N8R8 | `amped-esp32-idf`    | `BOARD_AMPED_WROVER`  | Software volume, software PEQ (10 bands)                 |
| Amped-ESP32-S3         | ESP32-S3-WROOM    | `amped-esp32s3-idf`  | `BOARD_AMPED_S3`      | Software volume, software PEQ (10 bands)                 |
| Louder-ESP32 / Plus    | ESP32-WROVER-N8R8 | `louder-esp32-idf`   | `BOARD_LOUDER_WROVER` | TAS hardware volume + 15-band PEQ (DIE_ID selects chip)  |
| Louder-ESP-Pro         | ESP32-S3-WROOM    | `louder-esp32s3-idf` | `BOARD_LOUDER_S3`     | TAS5825M hardware volume + 15-band PEQ                   |

Louder-ESP32 (TAS5805M) and Louder-ESP32-Plus (TAS5825M) share
`louder-esp32-idf` when the pinout matches; `tas5805m_init` probes `DIE_ID`
(`0x95` = TAS5825M). Amped Plus PCM5122 DSP is not in this firmware —
Amped envs always stay on software volume and software PEQ.

Bluetooth AVRCP still syncs the same 0–100% control value on every board.

Build:

```bash
pio run -e amped-esp32-idf
# or
task fw:build                          # default PIO_ENV=amped-esp32-idf
PIO_ENV=louder-esp32-idf task fw:build
```

`hal_board.h` includes exactly one of `src/hal/boards/*.h`. Amped builds
compile `dac_none.cpp` (GPIO amp enable). Louder builds compile
`dac_tas5825m.cpp` and link `components/tas5805m`.

S3 pin maps are DevKitC-1 defaults. Those two envs are **build-validated
only** until the user confirms the schematic. See
[hardware/README.md](hardware/README.md) and [roadmap.md](roadmap.md).

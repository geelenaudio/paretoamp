<!--
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Licensing

The Geelen Audio firmware is distributed under the
**GNU General Public License, version 3, or (at your option) any later
version**. See the [COPYING](../COPYING) file at the repo root for the
full text of the licence.

The device also serves `COPYING` at `http://<device>/COPYING` and the
SPA About screen (`web/pages/about.js`) links to it — that satisfies
GPL §5(d) ("Appropriate Legal Notices") for anyone using the device
without touching the source tree.

## Why GPL-3.0

The firmware links (on Louder builds) against
[sonocotta/esp32-tas5805m-dac](https://github.com/sonocotta/esp32-tas5805m-dac),
which is licensed **GPL-3.0-or-later**. Under GPL §5, "a covered work
that requires no special permission to make, run and propagate copies…"
must, when combined with GPL-licensed code, itself be licensed under the
GPL. We accept that and relicense the firmware as a whole under
GPL-3.0-or-later so we can ship the on-chip hardware PEQ on Louder.

Amped builds do not link the sonocotta driver (see
`components/tas5805m/README.md` for the gating), but the source tree
still contains the driver — the whole repo therefore ships under a
single licence to avoid confusion.

## Obtaining the corresponding source

The corresponding source is this repository. Build instructions live in
[../ESP32_AudioPlatform/BRINGUP.md](../ESP32_AudioPlatform/BRINGUP.md);
you need PlatformIO with the ESP-IDF 5.5+ platform. `pio run -e <env>`
produces byte-identical firmware from a clean tree (all timestamps in
the SPIFFS image are zero-mtime'd by `scripts/build_web.py`; the ELF is
deterministic modulo the toolchain version).

To install a modified version on the same hardware, follow the
`fw:upload` and `fw:uploadfs` targets in `Taskfile.yml`; those satisfy
GPL §6 ("Installation Information for User Products").

## Third-party inventory

| Component                                | Licence           | Where             |
|------------------------------------------|-------------------|-------------------|
| sonocotta esp32-tas5805m-dac (vendored)  | GPL-3.0-or-later  | `components/tas5805m/` (Louder only) |
| ESP-IDF (framework, drivers, esp_http_server) | Apache-2.0   | Toolchain / SDK   |
| espressif/led_strip (vendored v3.0.3)        | Apache-2.0   | `components/led_strip/` (WS2812 RMT) |
| chmorgan/esp-libhelix-mp3 (vendored)         | Apache-2.0 wrapper; RealNetworks RPSL/RCSL decoder | `components/libhelix-mp3/` |
| espressif/usb_device_uac 1.3.2 (S3 UAC gadget) | Apache-2.0 | `components_s3/usb_device_uac/` (S3 only; `usb_descriptors.c` renamed for PlatformIO) |
| espressif/tinyusb (pulled by usb_device_uac) | MIT          | `managed_components/` on S3 builds |
| ESP-IDF `nvs_flash`, `esp_wifi`, `esp_a2dp_api` | Apache-2.0 + ESP-IDF proprietary licence bundle | Toolchain / SDK |
| esp32-audioI2S (MP3 decoder path)        | LGPL-3.0          | `platformio.ini` `lib_deps` |
| esp32-a2dp                               | LGPL-3.0          | `platformio.ini` `lib_deps` |
| cJSON                                    | MIT               | ESP-IDF `components/json/cJSON` |
| htm / preact (SPA vendor)                | MIT / Apache-2.0  | `web/vendor/`     |
| Inter (latin 400/600)                    | SIL OFL 1.1       | `web/fonts/`      |
| Space Grotesk (latin 500/600)            | SIL OFL 1.1       | `web/fonts/`      |

Apache-2.0 and MIT are compatible with GPL-3.0-or-later downstream
combination. LGPL-3.0 is likewise compatible: LGPL was designed to be
combinable with GPL code (see the LGPL preamble).

## Pinned upstream

The vendored sonocotta driver was imported from commit
`c5d70d6ad765a58d798e887c62c3e9cdf49982a0` (2025-*). The bump procedure
is documented in `components/tas5805m/README.md` ("Bumping the upstream").
Any local modifications to the vendored driver are recorded in the
"Modifications from upstream" section of that README, per GPL §5a.

## SPDX headers

Every firmware source file authored or substantially modified by the
Geelen Audio project carries an SPDX identifier at the top:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
```

Files that are byte-identical to their pre-refactor state may still be
missing the header — we add them opportunistically on the next edit
rather than in a giant sweep. New files added to this tree MUST include
the header at creation.

## FAQ

- **"I only want to build Amped. Do I still have to comply with GPL?"** —
  yes, because the source tree you cloned includes the sonocotta driver
  (`components/tas5805m/`). If you distribute a build (binary or otherwise)
  you distribute a work covered by GPL-3.0-or-later.
- **"Can I keep my downstream changes private?"** — only if you never
  distribute the resulting device or firmware to anyone else. Once you
  distribute, the recipient must be able to obtain the corresponding
  source. Point them at your fork or hand them a tarball.
- **"What about my proprietary changes to the SPA?"** — the SPA is
  loaded on-device and served from SPIFFS. It's part of the "work as a
  whole" (GPL §0). Distribute the source.

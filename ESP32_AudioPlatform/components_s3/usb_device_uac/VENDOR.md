<!--
SPDX-License-Identifier: Apache-2.0
-->
# Vendored `espressif/usb_device_uac` 1.3.2

Copied from [espressif/esp-iot-solution](https://github.com/espressif/esp-iot-solution)
(`components/usb/usb_device_uac/`, Apache-2.0). Test apps omitted.

This tree lives under `components_s3/` so classic ESP32 (WROVER) never
discovers it. PlatformIO's ESP-IDF CMake path does not reliably fetch
registry components from the project-root `idf_component.yml` (see
`components/led_strip/VENDOR.md`); when it does fetch, SCons collides on
the basename `usb_descriptors.c` between TinyUSB and this gadget.

## Local modifications

- `tusb/usb_descriptors.c` renamed to `tusb/uac_usb_descriptors.c`.
- Descriptors compiled as this component's SRCS (not `target_sources` on
  TinyUSB) so PlatformIO SCons does not compile the same file twice.

TinyUSB itself stays a managed component (`espressif/tinyusb` via this
folder's `idf_component.yml`) downloaded into `managed_components/` on
S3 builds.

To bump: replace this folder from upstream 1.3.x, re-apply the rename,
rebuild `amped-esp32s3-idf`.

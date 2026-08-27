<!--
SPDX-License-Identifier: Apache-2.0
-->
# Vendored `espressif/led_strip` 3.0.3

Copied from [espressif/idf-extra-components](https://github.com/espressif/idf-extra-components)
(`led_strip/`, Apache-2.0). Examples and Sphinx docs are omitted.

PlatformIO's ESP-IDF CMake path does not fetch registry components from the
project-root `idf_component.yml`, so this tree is vendored the same way as
`mdns` and `libhelix-mp3`.

To bump: replace this folder from upstream 3.x, keep `LICENSE`, and rebuild
`amped-esp32-idf` (RMT on GPIO 12).

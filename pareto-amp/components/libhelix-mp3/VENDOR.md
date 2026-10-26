<!--
SPDX-License-Identifier: Apache-2.0
-->
# Vendored `chmorgan/esp-libhelix-mp3`

Wrapper: [chmorgan/esp-libhelix-mp3](https://github.com/chmorgan/esp-libhelix-mp3)
commit `585d7a6` (Apache-2.0). Nested decoder:
[chmorgan/libhelix-mp3](https://github.com/chmorgan/libhelix-mp3) commit
`f443079` (RealNetworks RPSL / RCSL — see `libhelix-mp3/LICENSE.txt`).

This used to be a gitlink (`160000`) without a root `.gitmodules`. GitHub
Actions then checked out an empty `components/libhelix-mp3/` (no
`CMakeLists.txt`) and CMake skipped the component.

PlatformIO's ESP-IDF path also does not fetch `chmorgan/esp-libhelix-mp3`
from the project-root `idf_component.yml`, so the tree is vendored the
same way as `led_strip` and `mdns`. Nested `.git` directories were
removed so the parent repo tracks the sources.

To bump: replace this folder from upstream, keep `LICENSE` / `LICENSE.txt`,
rebuild `amped-esp32-idf`.

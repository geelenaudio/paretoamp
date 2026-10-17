// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// Compile-time board selector. Every board-specific pin map, capability flag,
// and DAC binding lives behind a `BOARD_*` symbol defined by the PlatformIO env
// (see `platformio.ini`). This header is included by everything that needs
// board-scoped constants (`idf_main.cpp`, the DAC HAL, sdkconfig-driven bits).
//
// Adding a new board:
//   1. Create `src/hal/boards/<name>.h` mirroring the shape of the existing
//      files (pin map, capability flags, DAC descriptor).
//   2. Add a matching `[env:<name>]` block in `platformio.ini` with
//      `build_flags = ${common.build_flags} -DBOARD_<NAME>=1`.
//   3. Ship a per-env `sdkconfig.<name>` next to the existing ones.
//   4. Populate `docs/hardware/<name>.md`.
//
// Runtime board discovery is deliberately not attempted: the ESP32-WROVER and
// ESP32-S3-WROOM parts share the same package footprint but have different
// pin lists, and any misdetection would flash the wrong DAC config into the
// TAS5825M. Trust the toolchain instead.

#pragma once

#if defined(BOARD_AMPED_WROVER)
#  include "boards/amped_wrover.h"
#elif defined(BOARD_AMPED_S3)
#  include "boards/amped_s3.h"
#elif defined(BOARD_LOUDER_WROVER)
#  include "boards/louder_wrover.h"
#elif defined(BOARD_LOUDER_S3)
#  include "boards/louder_s3.h"
#else
#  error "No BOARD_* symbol defined. Pick a PlatformIO env: amped-esp32-idf, amped-esp32s3-idf, louder-esp32-idf, or louder-esp32s3-idf."
#endif

// Sanity check: every board header must define these. Keeps a typo in one of
// the per-board files from silently building a firmware without a DAC.
#if !defined(HAL_BOARD_NAME)
#  error "Board header did not define HAL_BOARD_NAME"
#endif
#if !defined(HAL_PIN_I2S_BCK) || !defined(HAL_PIN_I2S_WS) || !defined(HAL_PIN_I2S_DATA)
#  error "Board header did not define HAL_PIN_I2S_*"
#endif
#if !defined(HAL_HAS_HW_PEQ)
#  error "Board header did not define HAL_HAS_HW_PEQ (0 for software PEQ, 1 for hardware)"
#endif
#if !defined(HAL_HAS_USB_AUDIO)
#  error "Board header did not define HAL_HAS_USB_AUDIO (1 on S3 native USB, 0 on WROVER UART-bridge)"
#endif
#if !defined(HAL_EQ_MAX_BANDS_ACTIVE)
#  error "Board header did not define HAL_EQ_MAX_BANDS_ACTIVE (10 on Amped, up to 15 on Louder)"
#endif
#if !defined(HAL_HAS_WS2812)
#  error "Board header did not define HAL_HAS_WS2812"
#endif
#if !defined(HAL_HAS_ENCODER)
#  define HAL_HAS_ENCODER 0
#endif
#if HAL_HAS_ENCODER
#  if !defined(HAL_PIN_ENC_A) || !defined(HAL_PIN_ENC_B) || !defined(HAL_PIN_ENC_SW)
#    error "HAL_HAS_ENCODER=1 requires HAL_PIN_ENC_A, HAL_PIN_ENC_B, HAL_PIN_ENC_SW"
#  endif
#endif
#if HAL_HAS_WS2812
#  if !defined(HAL_PIN_WS2812)
#    error "HAL_HAS_WS2812=1 requires HAL_PIN_WS2812"
#  endif
#  if !defined(HAL_WS2812_COUNT) || (HAL_WS2812_COUNT < 1) || (HAL_WS2812_COUNT > 64)
#    error "HAL_WS2812_COUNT must be in 1..64 (compile-time strip length; override with -DHAL_WS2812_COUNT=N)"
#  endif
#  if !defined(HAL_WS2812_MAX_DUTY) || (HAL_WS2812_MAX_DUTY < 1) || (HAL_WS2812_MAX_DUTY > 100)
#    error "HAL_WS2812_MAX_DUTY must be in 1..100 (peak electrical brightness %; override with -DHAL_WS2812_MAX_DUTY=N)"
#  endif
#endif

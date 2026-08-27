# SPDX-License-Identifier: GPL-3.0-or-later
#
# Vendored TAS5825M / TAS5805M driver. See README.md for the modification
# notes required by GPL §5a and the upstream provenance.
#
# We register the component on ALL boards (not just Louder). On Amped
# the HAL wrapper (`src/hal/dac_tas5825m.cpp`) is `HAL_HAS_I2C_DAC=0`-
# gated, so nothing here is referenced and the linker drops the object
# code from the final image. Registering unconditionally means the tree
# builds cleanly no matter which env you flipped on last.

file(GLOB TAS5805M_SRCS "src/*.c")

idf_component_register(
  SRCS       ${TAS5805M_SRCS}
  INCLUDE_DIRS "include"
  REQUIRES   driver esp_common
)

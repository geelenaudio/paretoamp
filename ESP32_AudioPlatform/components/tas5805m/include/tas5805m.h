/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * TAS5825M DAC driver — public header used by the Geelen Audio firmware.
 *
 * The API surface mirrors the sonocotta upstream
 * (https://github.com/sonocotta/esp32-tas5805m-dac) with two Geelen-specific
 * additions on top:
 *
 *   - Bus-shared I/O: `tas5805m_init` takes an existing
 *     `i2c_master_bus_handle_t` instead of owning the whole I2C peripheral.
 *   - Raw BQ coefficient upload: `tas5805m_set_bq_coeffs` +
 *     `tas5805m_commit_eq_page` for on-chip 15-band PEQ, driven at runtime
 *     from the SPA. Upstream only ships baked-in preset tables.
 *
 * Implementation lives in `src/tas5805m.c`. See `README.md` for the
 * licensing chain (GPL-3.0-or-later) and the modification notes required by
 * GPL §5a.
 */

#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Control-state selector used by `tas5805m_set_state`. Matches the datasheet
 * device-state register values in Table 6-3.
 */
typedef enum {
  TAS5805M_CTRL_DEEP_SLEEP = 0,
  TAS5805M_CTRL_SLEEP      = 1,
  TAS5805M_CTRL_HIZ        = 2,
  TAS5805M_CTRL_PLAY       = 3,
} tas5805m_state_t;

/**
 * Channel selector for per-channel coefficient uploads. TAS5825M-Q1 has
 * independent left and right BQ banks (Book 0x8C pages 0x2C..0x37 vs
 * 0x38..0x43). Ganged control is done by writing to both channels.
 */
typedef enum {
  TAS5805M_CH_LEFT  = 0,
  TAS5805M_CH_RIGHT = 1,
} TAS5805M_EQ_CHANNELS;

typedef struct {
  i2c_master_bus_handle_t i2c_bus;
  uint8_t i2c_addr;  // 7-bit; typically 0x4C or 0x4D
} tas5805m_config_t;

/**
 * Configure I2C, verify the DAC responds, load the safe default page bank.
 * Idempotent — a second call reinitialises the DAC from a known state.
 */
esp_err_t tas5805m_init(const tas5805m_config_t* cfg);

/** Drive the DAC into the requested control state (see enum above). */
esp_err_t tas5805m_set_state(tas5805m_state_t s);

/** Set digital volume in percent (0..100 → 0..255 register). */
esp_err_t tas5805m_set_volume_pct(uint8_t pct);

/**
 * Upload one raw biquad slot. Coefficients are signed Q1.31, MSB first.
 * See `docs/eq.md` for the derivation of Q1.31 from our floating-point
 * biquads.
 */
esp_err_t tas5805m_set_bq_coeffs(uint8_t band,
                                 TAS5805M_EQ_CHANNELS ch,
                                 int32_t b0_q1_31, int32_t b1_q1_31,
                                 int32_t b2_q1_31, int32_t a1_q1_31,
                                 int32_t a2_q1_31);

/**
 * Commit the pending BQ page bank update via the page-reset dance in
 * datasheet section 8.5 (0x7F=0x00, 0x00=0x00, 0x7F=0x8C, 0x00=0x2C, …).
 * Call once after uploading all 15 bands for the frame.
 */
esp_err_t tas5805m_commit_eq_page(void);

/** Convert a float biquad coefficient in [-1, 1) to signed Q1.31. */
int32_t tas5805m_float_to_q1_31(float v);

#ifdef __cplusplus
}
#endif

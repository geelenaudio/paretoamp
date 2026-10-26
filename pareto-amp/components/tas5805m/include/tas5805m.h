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

#include <stdbool.h>
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
 * Probed silicon. TAS5825M DIE_ID reset is 0x95 (SLASEH7H). Anything else
 * on this I2C address is treated as TAS5805M (Louder-ESP32 vs Plus).
 */
typedef enum {
  TAS5805M_MODEL_UNKNOWN = 0,
  TAS5805M_MODEL_5805,
  TAS5805M_MODEL_5825,
} tas5805m_model_t;

/**
 * Channel selector for per-channel coefficient uploads. Both chips use
 * Book 0xAA; page maps differ (TAS5825M left starts 0x01/0x30, TAS5805M
 * left starts 0x24/0x18). Ganged control writes the same coefficients
 * to both channels.
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

/**
 * Full analog bring-up after I2S clocks are running (TAS5825M §9.5.3):
 * reset, 16-bit I2S, Hi-Z + DSP enable, ≥5 ms, identity BQ in Book 0xAA,
 * USE_DEFAULT_COEFFS=0 (48 kHz host RAM), PLAY, clear faults.
 */
esp_err_t tas5805m_enter_play(void);

/** Dump CTRL / clock-detect / PVDD / fault registers (page 0). */
void tas5805m_log_status(void);

/** Silicon detected at `tas5805m_init` (DIE_ID). */
tas5805m_model_t tas5805m_probed_model(void);

/**
 * Set digital volume 0..100%. Amplitude-style dB map onto register 0x4C
 * (0 → mute 0xFF, 100 → 0 dB = 0x30). Matches `eqVolumePctToTasDigVol`.
 */
esp_err_t tas5805m_set_volume_pct(uint8_t pct);

/**
 * Upload one raw biquad slot. Coefficients are signed Q5.27, MSB first
 * (1.0 = 0x08000000). a1/a2 are TI-form (negated textbook / software IIR).
 * Book 0xAA; page map follows `tas5805m_probed_model()`.
 */
esp_err_t tas5805m_set_bq_coeffs(uint8_t band,
                                 TAS5805M_EQ_CHANNELS ch,
                                 int32_t b0_q5_27, int32_t b1_q5_27,
                                 int32_t b2_q5_27, int32_t a1_q5_27,
                                 int32_t a2_q5_27);

/**
 * After uploading BQ slots, enable the 15-band EQ (ganged, bypass off).
 * TAS5825M: Book 0x8C page 0x0B. TAS5805M: DSP_MISC 0x66.
 */
esp_err_t tas5805m_commit_eq_page(void);

/**
 * Bypass (on=false) or enable (on=true) the on-chip EQ without touching
 * coefficients. Live EQ stays in PLAY — do not PDN. Disable must bypass
 * or the last upload stays in the DSP and analog never returns to dry.
 * TAS5825M gang bit follows the last `tas5805m_set_eq_ganged` call
 * (default ganged). TAS5805M DSP_MISC is independent of gang.
 */
esp_err_t tas5805m_set_eq_enabled(bool on);

/**
 * TAS5825M Book 0x8C gang_eq: true = L/R share left RAM (ganged), false =
 * independent banks. No-op on TAS5805M (independent Book 0xAA already).
 * Applied on the next `tas5805m_set_eq_enabled`.
 */
void tas5805m_set_eq_ganged(bool ganged);

/** Convert a float biquad coefficient in [-16, 16) to signed Q5.27. */
int32_t tas5805m_float_to_q5_27(float v);

#ifdef __cplusplus
}
#endif

/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * TAS5825M / TAS5805M DAC driver — vendored implementation for the Geelen
 * Audio firmware.
 *
 * This driver is derived from the sonocotta upstream
 * (https://github.com/sonocotta/esp32-tas5805m-dac, commit
 * c5d70d6ad765a58d798e887c62c3e9cdf49982a0) and licensed GPL-3.0-or-later.
 * The full text lives at the project root under COPYING.
 *
 * We re-implemented the I/O layer against the modern ESP-IDF
 * `driver/i2c_master.h` bus API (the upstream uses the legacy
 * `driver/i2c.h`) so that the driver plays nicely with the shared bus
 * handle owned by our HAL. Register addresses, coefficient page layout,
 * and startup semantics come from the upstream and the TAS5825M datasheet
 * chapters 6.5 and 8.
 *
 * This file implements only what the wrapper in `src/hal/dac_tas5825m.cpp`
 * needs today:
 *   - `tas5805m_init` — attach the device to the bus, verify presence,
 *     drive the state machine into HI-Z.
 *   - `tas5805m_set_state` — HI-Z / PLAY transitions (Deep-Sleep and
 *     Sleep are exposed for symmetry but not currently used).
 *   - `tas5805m_set_volume_pct` — 0..100 % → 0..255 register on
 *     `TAS5805M_DIG_VOL_CTRL`.
 *   - `tas5805m_set_bq_coeffs` / `tas5805m_commit_eq_page` — raw biquad
 *     coefficient upload for the on-chip 15-band per-channel PEQ. See
 *     the register-map notes at the top of `tas5805m_bq_upload` below.
 *
 * Modifications from the sonocotta upstream (GPL §5a):
 *   - Rewritten I/O layer for `i2c_master_bus_handle_t` (bus-shared API).
 *   - New `tas5805m_set_bq_coeffs` / `tas5805m_commit_eq_page` API for
 *     runtime raw coefficient uploads (upstream only ships baked-in
 *     preset tables). The register-map for those uploads is inferred
 *     from the upstream preset tables under `eq/tas5805m_eq.h`.
 *   - Dropped the state singleton / mute / mixer helpers — the wrapper
 *     handles those through the ampstate abstraction instead.
 */

#include "tas5805m.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "tas5805m";

// TAS5825M register map (control page, page 0x00 of book 0x00).
// Values match the datasheet Table 6-2 and the upstream `tas5805m_cfg.h`.
#define REG_PAGE_SET       0x00
#define REG_BOOK_SET       0x7F
#define REG_RESET_CTRL     0x01
#define REG_DEVICE_CTRL_1  0x02
#define REG_DEVICE_CTRL_2  0x03
#define REG_SAP_CTRL1      0x33
#define REG_CLOCK_DET_CTRL 0x29
#define REG_FS_MON         0x37
#define REG_BCK_MON        0x38
#define REG_CLKDET_STATUS  0x39
#define REG_DIG_VOL_CTRL   0x4C
#define REG_DSP_CTRL       0x46
#define REG_ANA_CTRL       0x53
#define REG_AGAIN          0x54
#define REG_PVDD_ADC       0x5E
#define REG_DSP_MISC       0x66
#define REG_DIE_ID         0x67
#define REG_POWER_STATE    0x68
#define REG_AUTOMUTE       0x69
#define REG_CHAN_FAULT     0x70
#define REG_GLOBAL_FAULT1  0x71
#define REG_GLOBAL_FAULT2  0x72
#define REG_FAULT_CLEAR    0x78

// Book/page numbers used by the coefficient upload path.
// TAS5825M 15-band PEQ: Book 0xAA. Layout from ESPHome tas58xx
// (TAS5805M starts at page 0x24 / 0x18 — do not use that map here).
#define BOOK_CONTROL       0x00
#define BOOK_EQ            0xAA
#define BOOK_EQ_CTRL       0x8C
#define PAGE_ZERO          0x00
#define PAGE_EQ_MODE       0x0B
#define REG_GANG_EQ        0x28  // 32-bit LE; 1 = L/R ganged

// TAS5825M DIE_ID reset (SLASEH7H). Other IDs on this address = TAS5805M.
#define DIE_ID_TAS5825M    0x95
// DSP_CTRL 0x46: bits[4:3] rate (01 = 48 kHz), bit0 USE_DEFAULT_COEFFS.
#define DSP_CTRL_48K_ZROM  0x09
#define DSP_CTRL_48K_RAM   0x08
// TAS5805M DSP_MISC 0x66 (ESPHome tas58xx): off=0b0111, 15-band=0b0110.
#define DSP_MISC_EQ_OFF    0x07
#define DSP_MISC_EQ_15     0x06
#define Q5_27_ONE          0x08000000

typedef struct {
  uint8_t page;
  uint8_t offset;
} bq_slot_t;

// TAS5825M Book 0xAA (ESPHome tas58xx / process-flow dumps).
static const bq_slot_t kBqLeft5825[15] = {
  { 0x01, 0x30 }, { 0x01, 0x44 }, { 0x01, 0x58 }, { 0x01, 0x6C }, { 0x02, 0x08 },
  { 0x02, 0x1C }, { 0x02, 0x30 }, { 0x02, 0x44 }, { 0x02, 0x58 }, { 0x02, 0x6C },
  { 0x03, 0x08 }, { 0x03, 0x1C }, { 0x03, 0x30 }, { 0x03, 0x44 }, { 0x03, 0x58 },
};

static const bq_slot_t kBqRight5825[15] = {
  { 0x03, 0x6C }, { 0x04, 0x08 }, { 0x04, 0x1C }, { 0x04, 0x30 }, { 0x04, 0x44 },
  { 0x04, 0x58 }, { 0x04, 0x6C }, { 0x05, 0x08 }, { 0x05, 0x1C }, { 0x05, 0x30 },
  { 0x05, 0x44 }, { 0x05, 0x58 }, { 0x05, 0x6C }, { 0x06, 0x08 }, { 0x06, 0x1C },
};

// TAS5805M Book 0xAA (ESPHome tas58xx_eq_common.h).
static const bq_slot_t kBqLeft5805[15] = {
  { 0x24, 0x18 }, { 0x24, 0x2C }, { 0x24, 0x40 }, { 0x24, 0x54 }, { 0x24, 0x68 },
  { 0x24, 0x7C }, { 0x25, 0x18 }, { 0x25, 0x2C }, { 0x25, 0x40 }, { 0x25, 0x54 },
  { 0x25, 0x68 }, { 0x25, 0x7C }, { 0x26, 0x18 }, { 0x26, 0x2C }, { 0x26, 0x40 },
};

static const bq_slot_t kBqRight5805[15] = {
  { 0x26, 0x54 }, { 0x26, 0x68 }, { 0x26, 0x7C }, { 0x27, 0x18 }, { 0x27, 0x2C },
  { 0x27, 0x40 }, { 0x27, 0x54 }, { 0x27, 0x68 }, { 0x27, 0x7C }, { 0x28, 0x18 },
  { 0x28, 0x2C }, { 0x28, 0x40 }, { 0x28, 0x54 }, { 0x28, 0x68 }, { 0x28, 0x7C },
};

// Runtime state. Only one TAS58xx per device is supported.
static i2c_master_dev_handle_t s_dev = NULL;
static uint8_t s_current_book = 0xFF;
static uint8_t s_current_page = 0xFF;
static tas5805m_model_t s_model = TAS5805M_MODEL_UNKNOWN;
static bool s_eq_ganged = true;

static const bq_slot_t* bq_slot(uint8_t band, TAS5805M_EQ_CHANNELS ch) {
  const bq_slot_t* table;
  if (s_model == TAS5805M_MODEL_5805) {
    table = (ch == TAS5805M_CH_LEFT) ? kBqLeft5805 : kBqRight5805;
  } else {
    table = (ch == TAS5805M_CH_LEFT) ? kBqLeft5825 : kBqRight5825;
  }
  return &table[band];
}

static esp_err_t write_identity_all(void);

static esp_err_t reg_write_byte(uint8_t reg, uint8_t val) {
  if (!s_dev) return ESP_ERR_INVALID_STATE;
  uint8_t buf[2] = { reg, val };
  return i2c_master_transmit(s_dev, buf, sizeof(buf), 200);
}

static esp_err_t reg_read_byte(uint8_t reg, uint8_t* val) {
  if (!s_dev) return ESP_ERR_INVALID_STATE;
  return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 200);
}

static esp_err_t reg_write_bytes(uint8_t reg, const uint8_t* data, size_t len) {
  if (!s_dev) return ESP_ERR_INVALID_STATE;
  // Small transfers on this DAC never exceed 21 bytes (one 20-byte BQ +
  // the leading register address), so a stack scratch is fine.
  uint8_t buf[24];
  if (len + 1 > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
  buf[0] = reg;
  memcpy(buf + 1, data, len);
  return i2c_master_transmit(s_dev, buf, len + 1, 200);
}

static esp_err_t select_book_page(uint8_t book, uint8_t page) {
  // Setting the book requires a two-step incantation because the book
  // register lives at page 0x00 of any book. Always land on page 0x00
  // first, then bump the book, then move to the target page.
  if (s_current_book == book && s_current_page == page) return ESP_OK;
  esp_err_t err = reg_write_byte(REG_PAGE_SET, PAGE_ZERO);
  if (err != ESP_OK) return err;
  err = reg_write_byte(REG_BOOK_SET, book);
  if (err != ESP_OK) return err;
  err = reg_write_byte(REG_PAGE_SET, page);
  if (err == ESP_OK) {
    s_current_book = book;
    s_current_page = page;
  } else {
    // Failing partway through leaves the DAC in an unknown state; next
    // caller must re-issue the incantation.
    s_current_book = 0xFF;
    s_current_page = 0xFF;
  }
  return err;
}

esp_err_t tas5805m_init(const tas5805m_config_t* cfg) {
  if (!cfg || !cfg->i2c_bus) return ESP_ERR_INVALID_ARG;

  if (s_dev) {
    // Idempotent: allow a re-init from a known state.
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
  }

  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = cfg->i2c_addr,
      .scl_speed_hz = 400000,
  };
  esp_err_t err = i2c_master_bus_add_device(cfg->i2c_bus, &dev_cfg, &s_dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2c_master_bus_add_device(0x%02x) failed: %s",
             (unsigned)cfg->i2c_addr, esp_err_to_name(err));
    return err;
  }
  s_current_book = 0xFF;
  s_current_page = 0xFF;

  // Land on the control page. Anything before this point could have left
  // the DAC in an unexpected book/page bank across resets.
  err = select_book_page(BOOK_CONTROL, PAGE_ZERO);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "book/page select failed: %s", esp_err_to_name(err));
    return err;
  }

  // DIE_ID: TAS5825M reset is 0x95. Louder-ESP32 (TAS5805M) reports a
  // different value; one WROVER image covers both when the pins match.
  uint8_t die_id = 0;
  err = reg_read_byte(REG_DIE_ID, &die_id);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "DIE_ID read failed at 0x%02x: %s",
             (unsigned)cfg->i2c_addr, esp_err_to_name(err));
    return err;
  }
  s_model = (die_id == DIE_ID_TAS5825M) ? TAS5805M_MODEL_5825 : TAS5805M_MODEL_5805;
  ESP_LOGI(TAG, "%s detected (DIE_ID=0x%02x) at 0x%02x",
           s_model == TAS5805M_MODEL_5825 ? "TAS5825M" : "TAS5805M",
           (unsigned)die_id, (unsigned)cfg->i2c_addr);

  // Boot into HI-Z. PLAY (and analog config) waits until I2S is clocking.
  err = tas5805m_set_state(TAS5805M_CTRL_HIZ);
  if (err != ESP_OK) return err;
  vTaskDelay(pdMS_TO_TICKS(5));
  return ESP_OK;
}

void tas5805m_log_status(void) {
  if (!s_dev) return;
  if (select_book_page(BOOK_CONTROL, PAGE_ZERO) != ESP_OK) return;
  uint8_t ctrl2 = 0, fs = 0, bck = 0, clk = 0, pvdd = 0, pwr = 0;
  uint8_t mute = 0, chf = 0, gf1 = 0, gf2 = 0, vol = 0, again = 0;
  (void)reg_read_byte(REG_DEVICE_CTRL_2, &ctrl2);
  (void)reg_read_byte(REG_FS_MON, &fs);
  (void)reg_read_byte(REG_BCK_MON, &bck);
  (void)reg_read_byte(REG_CLKDET_STATUS, &clk);
  (void)reg_read_byte(REG_PVDD_ADC, &pvdd);
  (void)reg_read_byte(REG_POWER_STATE, &pwr);
  (void)reg_read_byte(REG_AUTOMUTE, &mute);
  (void)reg_read_byte(REG_CHAN_FAULT, &chf);
  (void)reg_read_byte(REG_GLOBAL_FAULT1, &gf1);
  (void)reg_read_byte(REG_GLOBAL_FAULT2, &gf2);
  uint8_t sap = 0, dsp = 0;
  (void)reg_read_byte(REG_DIG_VOL_CTRL, &vol);
  (void)reg_read_byte(REG_AGAIN, &again);
  (void)reg_read_byte(REG_SAP_CTRL1, &sap);
  (void)reg_read_byte(REG_DSP_CTRL, &dsp);
  ESP_LOGI(TAG,
           "status CTRL2=0x%02x FS=0x%02x BCK=0x%02x CLKDET=0x%02x PVDD_ADC=0x%02x "
           "PWR=0x%02x AUTOMUTE=0x%02x CHAN_FAULT=0x%02x GF1=0x%02x GF2=0x%02x "
           "VOL=0x%02x AGAIN=0x%02x SAP=0x%02x DSP_CTRL=0x%02x",
           ctrl2, fs, bck, clk, pvdd, pwr, mute, chf, gf1, gf2, vol, again, sap, dsp);
  if (pvdd == 0) {
    ESP_LOGW(TAG, "PVDD_ADC=0 — speaker rail is off; USB does not power the Class-D");
  }
}

esp_err_t tas5805m_enter_play(void) {
  if (!s_dev) return ESP_ERR_INVALID_STATE;
  s_current_book = 0xFF;
  s_current_page = 0xFF;

  ESP_LOGI(TAG, "pre-seq");
  tas5805m_log_status();

  // 16-bit Philips I2S (32-FS). SAP_CTRL1=0x00 is I2S + 16-bit words
  // (reset default is 24-bit). DSP_CTRL 0x46=0x09 is 48 kHz processing +
  // ZROM until host RAM is loaded. Skip ESPHome's 96 kHz GPIO/hidden
  // regs (0x61/0x60/0x7D/0x7E/page1) — those are SLAA847 SPI mux, not
  // our I2C boot, and they left this chip in Sleep with CHAN_FAULT.
  // TAS5825M §9.5.3: I2S stable → Hi-Z + DSP enable → ≥5 ms → init
  // coefficients → PLAY. Load identity in Hi-Z, then 0x46=0x08 (host
  // RAM, USE_DEFAULT_COEFFS=0) before PLAY.
  enum { kDelay = 0xFE };
  static const uint8_t kSeq[][2] = {
      {0x00, 0x00}, {0x7F, 0x00},
      {0x03, 0x02}, {0x01, 0x11}, {0x03, 0x02},
      {kDelay, 10},
      {0x03, 0x00},
      {0x46, DSP_CTRL_48K_ZROM},
      {0x03, 0x02},
      {kDelay, 5},
      {0x02, 0x00},
      {0x33, 0x00},
      {0x4C, 0x30}, {0x53, 0x00}, {0x54, 0x00},
      {0x29, 0x7C},
  };

  for (size_t i = 0; i < sizeof(kSeq) / sizeof(kSeq[0]); ++i) {
    if (kSeq[i][0] == kDelay) {
      vTaskDelay(pdMS_TO_TICKS(kSeq[i][1]));
      continue;
    }
    esp_err_t err = reg_write_byte(kSeq[i][0], kSeq[i][1]);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "enter_play write 0x%02x=0x%02x failed: %s",
               kSeq[i][0], kSeq[i][1], esp_err_to_name(err));
      return err;
    }
  }
  s_current_book = 0xFF;
  s_current_page = 0xFF;

  esp_err_t id_err = write_identity_all();
  if (id_err != ESP_OK) {
    ESP_LOGE(TAG, "identity BQ load failed: %s", esp_err_to_name(id_err));
    return id_err;
  }

  s_current_book = 0xFF;
  s_current_page = 0xFF;
  esp_err_t err = select_book_page(BOOK_CONTROL, PAGE_ZERO);
  if (err != ESP_OK) return err;
  err = reg_write_byte(REG_DSP_CTRL, DSP_CTRL_48K_RAM);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "DSP_CTRL host-RAM write failed: %s", esp_err_to_name(err));
    return err;
  }
  err = reg_write_byte(REG_DEVICE_CTRL_2, TAS5805M_CTRL_PLAY);
  if (err != ESP_OK) return err;
  err = reg_write_byte(REG_FAULT_CLEAR, 0x80);
  if (err != ESP_OK) return err;
  vTaskDelay(pdMS_TO_TICKS(50));
  ESP_LOGI(TAG, "post-seq");
  tas5805m_log_status();

  uint8_t pwr = 0;
  if (select_book_page(BOOK_CONTROL, PAGE_ZERO) == ESP_OK &&
      reg_read_byte(REG_POWER_STATE, &pwr) == ESP_OK && pwr != 0x03) {
    ESP_LOGW(TAG, "analog still PWR=0x%02x after PLAY; retry via Sleep", pwr);
    (void)reg_write_byte(REG_DEVICE_CTRL_2, TAS5805M_CTRL_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(5));
    (void)reg_write_byte(REG_DEVICE_CTRL_2, TAS5805M_CTRL_PLAY);
    (void)reg_write_byte(REG_FAULT_CLEAR, 0x80);
    vTaskDelay(pdMS_TO_TICKS(20));
    tas5805m_log_status();
  }
  return ESP_OK;
}

tas5805m_model_t tas5805m_probed_model(void) {
  return s_model;
}

esp_err_t tas5805m_set_state(tas5805m_state_t s) {
  esp_err_t err = select_book_page(BOOK_CONTROL, PAGE_ZERO);
  if (err != ESP_OK) return err;
  return reg_write_byte(REG_DEVICE_CTRL_2, (uint8_t)s);
}

esp_err_t tas5805m_set_volume_pct(uint8_t pct) {
  if (pct > 100) pct = 100;
  esp_err_t err = select_book_page(BOOK_CONTROL, PAGE_ZERO);
  if (err != ESP_OK) return err;
  // Keep in sync with geelen::eqVolumePctToTasDigVol (eq_q131.h).
  uint8_t reg;
  if (pct == 0) {
    reg = 0xFF;
  } else if (pct >= 100) {
    reg = 0x30;
  } else {
    double db = 20.0 * log10((double)pct / 100.0);
    long steps = lround(-db / 0.5);
    if (steps < 0) steps = 0;
    long val = 0x30L + steps;
    if (val > 0xFE) val = 0xFE;
    reg = (uint8_t)val;
  }
  ESP_LOGI(TAG, "digital vol pct=%u reg=0x%02x", (unsigned)pct, (unsigned)reg);
  return reg_write_byte(REG_DIG_VOL_CTRL, reg);
}

// Raw BQ coefficient upload path.
//
// We take the 5 Q5.27 coefficients, pack them big-endian into a 20-byte
// blob, and stream them to the right (book, page, offset) using an
// auto-page-wrapping helper. The BQ page bank cache (`bq_slot`)
// tells us where each of the 15 slots starts.
//
// The TAS5825M won't apply new coefficients until we cross-write a "commit"
// register (see `tas5805m_commit_eq_page`). This is why a full frame update
// is: 15 × left + 15 × right + 1 × commit.
static esp_err_t bq_write_slot(uint8_t book, uint8_t page, uint8_t offset,
                               const uint8_t bytes[20]) {
  esp_err_t err = ESP_OK;
  size_t written = 0;
  uint8_t cur_page = page;
  uint8_t cur_offset = offset;
  while (written < 20) {
    err = select_book_page(book, cur_page);
    if (err != ESP_OK) return err;
    // Usable range on any page is 0x08..0x7F (0x00..0x07 are page/book
    // control). A slot may span two pages when it starts at 0x7C.
    size_t page_room = 0x80 - cur_offset;
    size_t chunk = 20 - written;
    if (chunk > page_room) chunk = page_room;
    err = reg_write_bytes(cur_offset, bytes + written, chunk);
    if (err != ESP_OK) return err;
    written += chunk;
    if (written < 20) {
      cur_page++;
      cur_offset = 0x08;
    }
  }
  return ESP_OK;
}

static void q1_31_to_be(int32_t v, uint8_t out[4]) {
  uint32_t u = (uint32_t)v;
  out[0] = (uint8_t)((u >> 24) & 0xFF);
  out[1] = (uint8_t)((u >> 16) & 0xFF);
  out[2] = (uint8_t)((u >> 8)  & 0xFF);
  out[3] = (uint8_t)( u        & 0xFF);
}

esp_err_t tas5805m_set_bq_coeffs(uint8_t band,
                                 TAS5805M_EQ_CHANNELS ch,
                                 int32_t b0, int32_t b1,
                                 int32_t b2, int32_t a1, int32_t a2) {
  if (band >= 15) return ESP_ERR_INVALID_ARG;
  if (!s_dev)     return ESP_ERR_INVALID_STATE;

  uint8_t blob[20];
  q1_31_to_be(b0, blob + 0);
  q1_31_to_be(b1, blob + 4);
  q1_31_to_be(b2, blob + 8);
  q1_31_to_be(a1, blob + 12);
  q1_31_to_be(a2, blob + 16);

  const bq_slot_t* slot = bq_slot(band, ch);
  return bq_write_slot(BOOK_EQ, slot->page, slot->offset, blob);
}

static esp_err_t write_identity_all(void) {
  for (uint8_t i = 0; i < 15; ++i) {
    esp_err_t err = tas5805m_set_bq_coeffs(i, TAS5805M_CH_LEFT,
                                           Q5_27_ONE, 0, 0, 0, 0);
    if (err != ESP_OK) return err;
    err = tas5805m_set_bq_coeffs(i, TAS5805M_CH_RIGHT,
                                 Q5_27_ONE, 0, 0, 0, 0);
    if (err != ESP_OK) return err;
  }
  ESP_LOGI(TAG, "identity BQ loaded (15+15 Q5.27 slots, model=%s)",
           s_model == TAS5805M_MODEL_5805 ? "TAS5805M" : "TAS5825M");
  return ESP_OK;
}

esp_err_t tas5805m_set_eq_enabled(bool on) {
  if (!s_dev) return ESP_ERR_INVALID_STATE;
  s_current_book = 0xFF;
  s_current_page = 0xFF;

  esp_err_t err;
  if (s_model == TAS5805M_MODEL_5805) {
    err = select_book_page(BOOK_CONTROL, PAGE_ZERO);
    if (err != ESP_OK) return err;
    err = reg_write_byte(REG_DSP_MISC, on ? DSP_MISC_EQ_15 : DSP_MISC_EQ_OFF);
  } else {
    // Little-endian u32 pair: gang_eq @ 0x28, bypass_eq @ 0x2C (ESPHome tas58xx).
    uint8_t gang_bypass[8] = {0};
    gang_bypass[0] = s_eq_ganged ? 0x01 : 0x00;
    gang_bypass[4] = on ? 0x00 : 0x01;  // bypass when EQ is off
    err = select_book_page(BOOK_EQ_CTRL, PAGE_EQ_MODE);
    if (err != ESP_OK) return err;
    err = reg_write_bytes(REG_GANG_EQ, gang_bypass, sizeof(gang_bypass));
    s_current_book = 0xFF;
    s_current_page = 0xFF;
  }
  if (err != ESP_OK) return err;

  err = select_book_page(BOOK_CONTROL, PAGE_ZERO);
  if (err != ESP_OK) return err;
  uint8_t pwr = 0;
  (void)reg_read_byte(REG_POWER_STATE, &pwr);
  if (pwr != TAS5805M_CTRL_PLAY) {
    ESP_LOGW(TAG, "EQ mode change left PWR=0x%02x; restoring PLAY", pwr);
    (void)reg_write_byte(REG_DEVICE_CTRL_2, TAS5805M_CTRL_PLAY);
    (void)reg_write_byte(REG_FAULT_CLEAR, 0x80);
  }
  tas5805m_log_status();
  return ESP_OK;
}

esp_err_t tas5805m_commit_eq_page(void) {
  return tas5805m_set_eq_enabled(true);
}

void tas5805m_set_eq_ganged(bool ganged) {
  s_eq_ganged = ganged;
}

int32_t tas5805m_float_to_q5_27(float v) {
  if (isnan(v) || isinf(v)) return 0;
  if (v >= 16.0f) return INT32_MAX;
  if (v <= -16.0f) return INT32_MIN;
  double x = (double)v * 134217728.0;
  if (x >= 2147483647.0) return INT32_MAX;
  if (x <= -2147483648.0) return INT32_MIN;
  return (int32_t)(x >= 0 ? x + 0.5 : x - 0.5);
}

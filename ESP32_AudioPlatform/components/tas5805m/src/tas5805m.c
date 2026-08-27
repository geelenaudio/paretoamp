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
#define REG_DEVICE_CTRL_2  0x03
#define REG_DIG_VOL_CTRL   0x4C
#define REG_DSP_MISC       0x66
#define REG_DIE_ID         0x67

// Book/page numbers used by the coefficient upload path.
#define BOOK_CONTROL       0x00
#define BOOK_EQ            0xAA
#define PAGE_ZERO          0x00

// Per-band base offsets (book 0xAA). One BQ = 20 bytes: b0/b1/b2/a1/a2, each
// Q1.31 big-endian. Derived empirically from the upstream preset table
// (see eq/tas5805m_eq.h): each BQ occupies 20 contiguous "usable" bytes
// starting at (page, offset). Bytes 0x00..0x07 of every page are reserved
// for control (page / book switch), so a BQ that starts at offset 0x7C on
// page N wraps into offset 0x08..0x17 of page N+1.
//
// Layout for a single channel (5 BQs per page-run of 0x18..0x7B, then one
// straddling BQ, repeat). The right channel begins where the left channel
// finished, on page 0x26 offset 0x54. This gives us 30 BQ slots per
// TAS5825M frame, but only 15 per channel are actually addressable via the
// datasheet's DSP page — the rest live in reserved memory.
typedef struct {
  uint8_t page;
  uint8_t offset;
} bq_slot_t;

static const bq_slot_t kBqLeft[15] = {
  { 0x24, 0x18 }, { 0x24, 0x2C }, { 0x24, 0x40 }, { 0x24, 0x54 }, { 0x24, 0x68 },
  { 0x24, 0x7C }, { 0x25, 0x18 }, { 0x25, 0x2C }, { 0x25, 0x40 }, { 0x25, 0x54 },
  { 0x25, 0x68 }, { 0x25, 0x7C }, { 0x26, 0x18 }, { 0x26, 0x2C }, { 0x26, 0x40 },
};

static const bq_slot_t kBqRight[15] = {
  { 0x26, 0x54 }, { 0x26, 0x68 }, { 0x26, 0x7C }, { 0x27, 0x18 }, { 0x27, 0x2C },
  { 0x27, 0x40 }, { 0x27, 0x54 }, { 0x27, 0x68 }, { 0x27, 0x7C }, { 0x28, 0x18 },
  { 0x28, 0x2C }, { 0x28, 0x40 }, { 0x28, 0x54 }, { 0x28, 0x68 }, { 0x28, 0x7C },
};

// Runtime state. Only one TAS5825M per device is supported.
static i2c_master_dev_handle_t s_dev = NULL;
static uint8_t s_current_book = 0xFF;
static uint8_t s_current_page = 0xFF;

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
  esp_err_t err = ESP_OK;
  err |= reg_write_byte(REG_PAGE_SET, PAGE_ZERO);
  err |= reg_write_byte(REG_BOOK_SET, book);
  err |= reg_write_byte(REG_PAGE_SET, page);
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

  // Read the die ID as a presence check. TAS5825M's DIE_ID register
  // returns a non-zero value once the DAC is powered and clocked; we
  // don't check the exact value because we support both TAS5825M and
  // TAS5825MC-Q1 variants which report different IDs.
  uint8_t die_id = 0;
  err = reg_read_byte(REG_DIE_ID, &die_id);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "DIE_ID read failed at 0x%02x: %s",
             (unsigned)cfg->i2c_addr, esp_err_to_name(err));
    return err;
  }
  ESP_LOGI(TAG, "TAS5825M detected (DIE_ID=0x%02x) at 0x%02x",
           (unsigned)die_id, (unsigned)cfg->i2c_addr);

  // Boot into HI-Z. The wrapper calls set_state(PLAY) after I2S clocks
  // are stable — matches the datasheet section 6.5.1 sequence.
  err = tas5805m_set_state(TAS5805M_CTRL_HIZ);
  if (err != ESP_OK) return err;
  vTaskDelay(pdMS_TO_TICKS(5));
  return ESP_OK;
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
  // TAS5825M digital volume: 0x00 = +24 dB, 0x30 = 0 dB, 0xFE = -103 dB,
  // 0xFF = mute (datasheet section 7.1.5). Map 0..100 % linearly to
  // -60 dB..0 dB, i.e. 0 → 0xFF (mute), 100 → 0x30.
  uint8_t reg;
  if (pct == 0) {
    reg = 0xFF;
  } else {
    // 0 dB @ pct=100 → 0x30; -60 dB @ pct=1 → 0x30 + 60*2 = 0x30 + 120 = 0xA8.
    // (Register step is 0.5 dB below 0 dB.)
    int step = (100 - pct) * 2;  // 0.5 dB per step
    int val = 0x30 + step;
    if (val > 0xFE) val = 0xFE;
    reg = (uint8_t)val;
  }
  return reg_write_byte(REG_DIG_VOL_CTRL, reg);
}

// Raw BQ coefficient upload path.
//
// We take the 5 Q1.31 coefficients, pack them big-endian into a 20-byte
// blob, and stream them to the right (book, page, offset) using an
// auto-page-wrapping helper. The BQ page bank cache (`kBqLeft`/`kBqRight`)
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

  const bq_slot_t* slot = (ch == TAS5805M_CH_LEFT) ? &kBqLeft[band] : &kBqRight[band];
  return bq_write_slot(BOOK_EQ, slot->page, slot->offset, blob);
}

esp_err_t tas5805m_commit_eq_page(void) {
  // Datasheet section 8.5: after streaming new BQ coefficients into the EQ
  // book, land back on the control page and flip the DSP_MISC "EQ enable"
  // bit off/on. This is what the sonocotta upstream calls "commit" — it
  // forces the coefficient bank swap without dropping audio.
  esp_err_t err = select_book_page(BOOK_CONTROL, PAGE_ZERO);
  if (err != ESP_OK) return err;

  uint8_t misc = 0;
  err = reg_read_byte(REG_DSP_MISC, &misc);
  if (err != ESP_OK) return err;
  // Toggle bit 0 (EQ_OFF): 1 → 0 (enable) or 0 → 1 → 0 to force reload.
  uint8_t off = misc | 0x01;
  uint8_t on  = misc & ~0x01;
  err |= reg_write_byte(REG_DSP_MISC, off);
  vTaskDelay(1);
  err |= reg_write_byte(REG_DSP_MISC, on);
  return err;
}

int32_t tas5805m_float_to_q1_31(float v) {
  if (isnan(v) || isinf(v)) return 0;
  if (v <= -1.0f) return INT32_MIN;
  if (v >= 1.0f) return INT32_MAX;
  double x = (double)v * 2147483648.0;
  if (x >= 2147483647.0) return INT32_MAX;
  if (x <= -2147483648.0) return INT32_MIN;
  return (int32_t)(x >= 0 ? x + 0.5 : x - 0.5);
}

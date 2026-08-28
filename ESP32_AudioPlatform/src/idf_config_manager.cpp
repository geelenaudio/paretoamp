// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// ESP-IDF ConfigManager implementation (NVS + esp_littlefs).

#include "idf_config_manager.h"
#include "idf_identity.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "esp_vfs.h"
#include "esp_spiffs.h"

#include <cstring>
#include <cstdio>
#include <cmath>
#include <sys/stat.h>

namespace geelen_idf {

static const char* TAG = "Config";

// NVS keys — keep names <=15 chars (NVS API limit).
static const char* NVS_NAMESPACE         = "geelen";
static const char* NVS_KEY_VOLUME        = "vol";
static const char* NVS_KEY_PRESET        = "preset";
static const char* NVS_KEY_EQ_LEGACY     = "eq";
static const char* NVS_KEY_SOURCE        = "src";
static const char* NVS_KEY_SNAP_HOST     = "snap_h";
static const char* NVS_KEY_SNAP_PORT     = "snap_p";
static const char* NVS_KEY_WIFI_SSID     = "wifi_ssid";
static const char* NVS_KEY_WIFI_PASS     = "wifi_pass";
static const char* NVS_KEY_WIFI_EVER     = "wifi_ever";
// Legacy NVS key retained so we can erase credentials from devices that previously
// booted with Spotify Connect enabled. Not written by the current firmware.
static const char* NVS_KEY_LEGACY_SPOTIFY = "spotify_js";

// Phase 1 additions.
static const char* NVS_KEY_VOL_L         = "vol_l";
static const char* NVS_KEY_VOL_R         = "vol_r";
static const char* NVS_KEY_MUTED         = "muted";
static const char* NVS_KEY_CH_MODE       = "ch_mode";
static const char* NVS_KEY_CH_L_ON       = "ch_l_on";
static const char* NVS_KEY_CH_R_ON       = "ch_r_on";
static const char* NVS_KEY_LED_R         = "led_r";
static const char* NVS_KEY_LED_G         = "led_g";
static const char* NVS_KEY_LED_B         = "led_b";
static const char* NVS_KEY_LED_BRI       = "led_bri";
static const char* NVS_KEY_LED_USER      = "led_user";
// Single-layer PEQ storage. Legacy dual-layer keys (`eq_cal*`, `eq_pref*`)
// are read on load and migrated into `eq_bands`; the writer only touches
// the new key set below.
static const char* NVS_KEY_EQ_BANDS      = "eq_bands";
static const char* NVS_KEY_EQ_ON         = "eq_on";
static const char* NVS_KEY_EQ_NB         = "eq_nb";
static const char* NVS_KEY_EQ_SLOT       = "eq_slot";
// Legacy keys (read-only after migration).
static const char* NVS_KEY_LEGACY_EQ_CAL        = "eq_cal";
static const char* NVS_KEY_LEGACY_EQ_CAL_ON     = "eq_cal_on";
static const char* NVS_KEY_LEGACY_EQ_PREF       = "eq_pref";
static const char* NVS_KEY_LEGACY_EQ_PREF_ON    = "eq_pref_on";
static const char* NVS_KEY_LEGACY_EQ_PREF_SLOT  = "eq_pref_sl";
static const char* NVS_KEY_LEGACY_EQ_CAL_NB     = "eq_cal_nb";
static const char* NVS_KEY_LEGACY_EQ_PREF_NB    = "eq_pref_nb";
static const char* NVS_KEY_EQ_HR_MODE    = "eq_hr_mode";
static const char* NVS_KEY_EQ_HR_MANUAL  = "eq_hr_man";
static const char* NVS_KEY_DEV_NAME      = "dev_name";
static const char* NVS_KEY_HOSTNAME      = "hostname";
static const char* NVS_KEY_STA_MAC       = "sta_mac";
static const char* NVS_KEY_BT_MAC        = "bt_mac";
static const char* NVS_KEY_DLNA_ON       = "dlna_on";
static const char* NVS_KEY_VOICE_FB     = "voice_fb";
static const char* NVS_KEY_WIFI_PS      = "wifi_ps";
static const char* NVS_KEY_TT_FREQ     = "tt_freq";
static const char* NVS_KEY_TT_AMP      = "tt_amp";
static const char* NVS_KEY_TT_PAN      = "tt_pan";
static const char* NVS_KEY_TT_MODE      = "tt_mode";
static const char* NVS_KEY_TT_SWEEP    = "tt_sweep";
static const char* NVS_KEY_TT_SWP_LO   = "tt_swp_lo";
static const char* NVS_KEY_TT_SWP_HI   = "tt_swp_hi";
static const char* NVS_KEY_TT_DF2      = "tt_df2";

namespace {
constexpr uint8_t D_EQ = 1;
constexpr uint8_t D_AUDIO = 2;
constexpr uint8_t D_NET = 4;
constexpr uint8_t D_TESTTONE = 8;

static unsigned long cfgWallMs() { return (unsigned long)(esp_timer_get_time() / 1000ULL); }
}  // namespace

static const char* MOUNT_POINT = "/spiffs";
static const char* PRESETS_DIR = "/spiffs/presets";

// Legacy factory presets, only used by the Phase-5 migration path to seed
// preset slot 0..3 if the user has never customised them.
static const char* FIXED_PRESET_NAMES[] = { "Flat", "Bass Boost", "Vocals", "Treble Clarity" };
static const int8_t FIXED_PRESET_EQ[][EQ_BANDS] = {
  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
  { 6, 4, 2, 0, 0, 0, -1, -2, -2, -2 },
  { -2, 0, 2, 3, 2, 0, -1, -1, 0, 0 },
  { -3, -2, -1, 0, 1, 2, 3, 4, 4, 5 },
};

// ISO-262 third-octave centres matching the old 10-band graphic EQ, extended
// out to 15 bands so a Louder-shaped preset gets sensible defaults for the
// extra slots the SPA exposes.
static constexpr size_t kLegacyIsoBaseCount = 10;
static constexpr float kLegacyIsoCentres[PARAMETRIC_BANDS] = {
  31.5f, 63.0f, 125.0f, 250.0f, 500.0f,
  1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f,
  // Louder-specific extras — placeholders above 16 kHz so the SPA renders
  // them at the top of the band list rather than colliding with existing
  // centres. Users typically re-place these on load.
  17000.0f, 18000.0f, 19000.0f, 20000.0f, 22050.0f,
};

static void mountSpiffsOnce() {
  static bool mounted = false;
  if (mounted) return;
  esp_vfs_spiffs_conf_t conf = {
    .base_path = MOUNT_POINT,
    .partition_label = "spiffs",
    .max_files = 8,
    .format_if_mount_failed = true,
  };
  esp_err_t err = esp_vfs_spiffs_register(&conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "SPIFFS mounted at %s", MOUNT_POINT);
  }
  mounted = true;
}

ConfigManager::ConfigManager() {
  snapcast_host_[0] = '\0';
  wifi_ssid_[0] = '\0';
  wifi_pass_[0] = '\0';
}

void ConfigManager::loadDefaultEqBands(ParametricBand out[PARAMETRIC_BANDS]) {
  for (size_t i = 0; i < PARAMETRIC_BANDS; ++i) {
    out[i].freq_hz = kLegacyIsoCentres[i];
    out[i].gain_db = 0.f;
    out[i].q = 1.2f;
    out[i].type = geelen::EqFilterType::Bell;
    out[i].slope_db_oct = 12;
    out[i]._pad[0] = out[i]._pad[1] = 0;
  }
}

// Migration helpers for the two on-disk formats we still expect to encounter:
//   - 12-byte-per-band (freq/gain/q as three floats, pre-Phase-1)
//   - 16-byte-per-band (full `ParametricBand`, dual-layer era)
// Both stored exactly 10 bands. The 10 → 15 band expansion pads the tail
// with defaults from `kLegacyIsoCentres`.

// Fill `out` with the same defaults as ConfigManager::loadDefaultEqBands.
// Duplicated as a free function so migration helpers below don't need
// access to the private static — keeps the class header narrower.
static void fillDefaultBands(ParametricBand out[PARAMETRIC_BANDS]) {
  for (size_t i = 0; i < PARAMETRIC_BANDS; ++i) {
    out[i].freq_hz = kLegacyIsoCentres[i];
    out[i].gain_db = 0.f;
    out[i].q = 1.2f;
    out[i].type = geelen::EqFilterType::Bell;
    out[i].slope_db_oct = 12;
    out[i]._pad[0] = out[i]._pad[1] = 0;
  }
}

static void migrateLegacy12ByteEqBlob(const uint8_t* src, size_t src_bands,
                                       ParametricBand out[PARAMETRIC_BANDS]) {
  fillDefaultBands(out);
  for (size_t i = 0; i < src_bands && i < PARAMETRIC_BANDS; ++i) {
    float f, g, q;
    memcpy(&f, src + i * 12, sizeof(float));
    memcpy(&g, src + i * 12 + 4, sizeof(float));
    memcpy(&q, src + i * 12 + 8, sizeof(float));
    out[i].freq_hz = f;
    out[i].gain_db = g;
    out[i].q = q;
    out[i].type = geelen::EqFilterType::Bell;
    out[i].slope_db_oct = 12;
    out[i]._pad[0] = out[i]._pad[1] = 0;
  }
}

static void migrateLegacy16ByteEqBlob(const ParametricBand* src, size_t src_bands,
                                       ParametricBand out[PARAMETRIC_BANDS]) {
  fillDefaultBands(out);
  for (size_t i = 0; i < src_bands && i < PARAMETRIC_BANDS; ++i) {
    out[i] = src[i];
  }
}

void ConfigManager::loadDefaults() {
  config_ = Config{};  // zero everything first
  config_.volume = VOLUME_DEFAULT;
  config_.vol_l = VOLUME_DEFAULT;
  config_.vol_r = VOLUME_DEFAULT;
  config_.muted = false;
  config_.stereo_mode = StereoMode::Stereo;
  config_.ch_l_on = true;
  config_.ch_r_on = true;
  config_.led_r = 255;
  config_.led_g = 255;
  config_.led_b = 255;
  config_.led_bri = 0;
  config_.led_user = false;
  config_.preset_id = 0;
  for (size_t i = 0; i < EQ_BANDS; i++) config_.eq_gains[i] = FIXED_PRESET_EQ[0][i];
  loadDefaultEqBands(config_.eq_bands);
  config_.eq_active_bands = (uint8_t)PARAMETRIC_BANDS;
  config_.eq_on = false;
  config_.eq_active_slot = 0xFF;  // "unsaved / modified"
  config_.eq_hr_mode = HeadroomMode::Auto;
  config_.eq_hr_manual_db = 0.0f;
  strncpy(config_.dev_name, "Geelen Audio", DEV_NAME_MAX - 1);
  config_.dev_name[DEV_NAME_MAX - 1] = '\0';
  hostname_[0] = '\0';
  sta_mac_set_ = false;
  bt_mac_set_ = false;
  memset(sta_mac_, 0, sizeof(sta_mac_));
  memset(bt_mac_, 0, sizeof(bt_mac_));
  config_.dlna_on = false;
  config_.voice_feedback = true;
  config_.wifi_ps = WifiPsMode::None;

  audio_source_ = AudioSource::Bluetooth;
  snapcast_host_[0] = '\0';
  snapcast_port_ = SNAPCAST_PORT_DEFAULT;
  wifi_ssid_[0] = '\0';
  wifi_pass_[0] = '\0';
  wifi_ever_ok_ = false;
  wifi_ever_flag_present_ = false;
  tt_freq_hz_ = 440.f;
  tt_amp_db_ = -12.f;
  tt_pan_ = 0.f;
  tt_mode_ = 0;
  tt_sweep_s_ = 10.f;
  tt_sweep_lo_hz_ = 20.f;
  tt_sweep_hi_hz_ = 20000.f;
  tt_dual_f2_hz_ = 19000.f;
}

void ConfigManager::ensurePresetsDir() {
  mountSpiffsOnce();
  (void)mkdir(PRESETS_DIR, 0777);
}

bool ConfigManager::begin() {
  if (initialized_) return true;

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    loadDefaults();
    initialized_ = true;
    return true;
  }

  mountSpiffsOnce();
  ensurePresetsDir();

  loadDefaults();
  if (!load()) {
    markDirtyAll();
    save();
  }

  if (dirty_) {
    if (!save()) ESP_LOGW(TAG, "post-load NVS flush failed");
  }
  last_change_ms_ = 0;
  initialized_ = true;
  return true;
}

// Helper: read a u8 NVS key into a bool with default.
static bool nvsGetBool(nvs_handle_t h, const char* key, bool fallback) {
  uint8_t v = 0;
  if (nvs_get_u8(h, key, &v) == ESP_OK) return v != 0;
  return fallback;
}

bool ConfigManager::load() {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

  bool ok = true;

  // Legacy scalar volume is always read first; vol_l / vol_r override it
  // below if they exist. This lets devices upgrading from pre-Phase-1
  // firmware keep their chosen level across the rewrite.
  uint8_t legacy_vol = VOLUME_DEFAULT;
  if (nvs_get_u8(h, NVS_KEY_VOLUME, &legacy_vol) == ESP_OK) {
    config_.volume = legacy_vol;
    config_.vol_l = legacy_vol;
    config_.vol_r = legacy_vol;
  } else {
    ok = false;
  }

  uint8_t tmp8 = 0;
  if (nvs_get_u8(h, NVS_KEY_VOL_L, &tmp8) == ESP_OK) config_.vol_l = tmp8;
  if (nvs_get_u8(h, NVS_KEY_VOL_R, &tmp8) == ESP_OK) config_.vol_r = tmp8;
  config_.muted = nvsGetBool(h, NVS_KEY_MUTED, false);

  if (nvs_get_u8(h, NVS_KEY_CH_MODE, &tmp8) == ESP_OK)
    config_.stereo_mode = (tmp8 == 1) ? StereoMode::Mono : StereoMode::Stereo;
  config_.ch_l_on = nvsGetBool(h, NVS_KEY_CH_L_ON, true);
  config_.ch_r_on = nvsGetBool(h, NVS_KEY_CH_R_ON, true);
  if (nvs_get_u8(h, NVS_KEY_LED_R, &tmp8) == ESP_OK) config_.led_r = tmp8;
  if (nvs_get_u8(h, NVS_KEY_LED_G, &tmp8) == ESP_OK) config_.led_g = tmp8;
  if (nvs_get_u8(h, NVS_KEY_LED_B, &tmp8) == ESP_OK) config_.led_b = tmp8;
  if (nvs_get_u8(h, NVS_KEY_LED_BRI, &tmp8) == ESP_OK) {
    config_.led_bri = tmp8 > 100 ? 100 : tmp8;
  }
  config_.led_user = nvsGetBool(h, NVS_KEY_LED_USER, false);

  if (nvs_get_u8(h, NVS_KEY_PRESET, &config_.preset_id) != ESP_OK) ok = false;

  // Legacy int8_t EQ — if present, migrate into eq_cal on first load.
  size_t len = EQ_BANDS * sizeof(int8_t);
  bool legacy_eq_present =
    nvs_get_blob(h, NVS_KEY_EQ_LEGACY, config_.eq_gains, &len) == ESP_OK &&
    len == EQ_BANDS * sizeof(int8_t);

  // Parametric EQ — single 15-band curve.
  //
  // Try the current 15-band blob first; if absent, fall back to the legacy
  // 10-band `eq_pref` / `eq_cal` blobs (in that order — preference is what
  // the user was actively editing before the refactor). Legacy blobs come
  // in two on-disk shapes:
  //   - 16 bytes/band × 10 = 160 bytes (dual-layer era)
  //   - 12 bytes/band × 10 = 120 bytes (pre-Phase-1)
  {
    uint8_t eq_blob[512];
    len = sizeof(eq_blob);
    constexpr size_t kBands15  = PARAMETRIC_BANDS * sizeof(ParametricBand);          // 15*16 = 240
    constexpr size_t kBands10  = 10u * sizeof(ParametricBand);                        // 10*16 = 160
    constexpr size_t kBands10_12 = 10u * 12u;                                         // 10*12 = 120

    if (nvs_get_blob(h, NVS_KEY_EQ_BANDS, eq_blob, &len) == ESP_OK && len == kBands15) {
      memcpy(config_.eq_bands, eq_blob, kBands15);
    } else {
      // Fall back to legacy `eq_pref` (user's active curve pre-refactor).
      bool migrated = false;
      len = sizeof(eq_blob);
      if (nvs_get_blob(h, NVS_KEY_LEGACY_EQ_PREF, eq_blob, &len) == ESP_OK) {
        if (len == kBands10) {
          ESP_LOGW(TAG, "Migrating legacy `eq_pref` (10 bands, dual-layer) to `eq_bands`");
          migrateLegacy16ByteEqBlob(reinterpret_cast<const ParametricBand*>(eq_blob), 10,
                                    config_.eq_bands);
          migrated = true;
        } else if (len == kBands10_12) {
          ESP_LOGW(TAG, "Migrating legacy `eq_pref` (10 bands, 12-byte) to `eq_bands`");
          migrateLegacy12ByteEqBlob(eq_blob, 10, config_.eq_bands);
          migrated = true;
        }
      }
      // If no `eq_pref`, try `eq_cal` — some devices only ever populated the
      // calibration layer (Snapclient's automatic RMS-matching pass).
      if (!migrated) {
        len = sizeof(eq_blob);
        if (nvs_get_blob(h, NVS_KEY_LEGACY_EQ_CAL, eq_blob, &len) == ESP_OK) {
          if (len == kBands10) {
            ESP_LOGW(TAG, "Migrating legacy `eq_cal` (10 bands, dual-layer) to `eq_bands`");
            migrateLegacy16ByteEqBlob(reinterpret_cast<const ParametricBand*>(eq_blob), 10,
                                      config_.eq_bands);
            migrated = true;
          } else if (len == kBands10_12) {
            ESP_LOGW(TAG, "Migrating legacy `eq_cal` (10 bands, 12-byte) to `eq_bands`");
            migrateLegacy12ByteEqBlob(eq_blob, 10, config_.eq_bands);
            migrated = true;
          }
        }
      }
      // Final fallback: graphic-EQ int8_t gains, migrated to bell filters.
      if (!migrated) {
        loadDefaultEqBands(config_.eq_bands);
        if (legacy_eq_present) {
          ESP_LOGW(TAG, "Migrating legacy graphic-EQ gains to `eq_bands`");
          for (size_t i = 0; i < 10 && i < PARAMETRIC_BANDS; ++i)
            config_.eq_bands[i].gain_db = (float)config_.eq_gains[i];
          config_.eq_on = true;
          config_.eq_active_bands = 10;
        }
      }
      if (migrated) markDirtyEq();
    }
  }

  // Enable flag: prefer the new key, fall back to whichever legacy layer was
  // enabled (`eq_pref_on` takes precedence over `eq_cal_on`).
  if (nvs_get_u8(h, NVS_KEY_EQ_ON, &tmp8) == ESP_OK) {
    config_.eq_on = (tmp8 != 0);
  } else {
    const bool pref_on = nvsGetBool(h, NVS_KEY_LEGACY_EQ_PREF_ON, false);
    const bool cal_on  = nvsGetBool(h, NVS_KEY_LEGACY_EQ_CAL_ON,  false);
    config_.eq_on = pref_on || cal_on;
  }

  // Active-band count: prefer the new key, else migrate from `eq_pref_nb`.
  // A previous WIP of this refactor could persist `eq_nb=1` while the
  // 15-band blob still held the user's full curve. Prefer a larger legacy
  // `eq_pref_nb` when present, and raise the count if later bands are
  // clearly in use.
  if (nvs_get_u8(h, NVS_KEY_EQ_NB, &tmp8) == ESP_OK && tmp8 >= 1 &&
      tmp8 <= (uint8_t)PARAMETRIC_BANDS) {
    config_.eq_active_bands = tmp8;
    uint8_t legacy_nb = 0;
    if (nvs_get_u8(h, NVS_KEY_LEGACY_EQ_PREF_NB, &legacy_nb) == ESP_OK &&
        legacy_nb > tmp8 && legacy_nb <= 10u) {
      ESP_LOGW(TAG, "Raising eq_nb %u -> %u from leftover eq_pref_nb",
               (unsigned)tmp8, (unsigned)legacy_nb);
      config_.eq_active_bands = legacy_nb;
      markDirtyEq();
    }
  } else if (nvs_get_u8(h, NVS_KEY_LEGACY_EQ_PREF_NB, &tmp8) == ESP_OK && tmp8 >= 1 &&
             tmp8 <= 10u) {
    config_.eq_active_bands = tmp8;
  } else if (nvs_get_u8(h, NVS_KEY_LEGACY_EQ_CAL_NB, &tmp8) == ESP_OK && tmp8 >= 1 &&
             tmp8 <= 10u) {
    config_.eq_active_bands = tmp8;
  } else {
    config_.eq_active_bands = 10;
  }
  {
    uint8_t last_used = 0;
    for (uint8_t i = 0; i < (uint8_t)PARAMETRIC_BANDS; ++i) {
      if (std::fabs(config_.eq_bands[i].gain_db) > 0.25f) last_used = (uint8_t)(i + 1);
    }
    if (last_used > config_.eq_active_bands) {
      ESP_LOGW(TAG, "Raising eq_nb %u -> %u (non-flat bands after stored count)",
               (unsigned)config_.eq_active_bands, (unsigned)last_used);
      config_.eq_active_bands = last_used;
      markDirtyEq();
    }
  }

  // Active preset slot.
  if (nvs_get_u8(h, NVS_KEY_EQ_SLOT, &tmp8) == ESP_OK) {
    config_.eq_active_slot = tmp8;
  } else if (nvs_get_u8(h, NVS_KEY_LEGACY_EQ_PREF_SLOT, &tmp8) == ESP_OK) {
    config_.eq_active_slot = tmp8;
  }

  if (nvs_get_u8(h, NVS_KEY_EQ_HR_MODE, &tmp8) == ESP_OK)
    config_.eq_hr_mode = (tmp8 == 1) ? HeadroomMode::Manual : HeadroomMode::Auto;
  size_t hr_len = sizeof(float);
  float hr_manual = 0.0f;
  if (nvs_get_blob(h, NVS_KEY_EQ_HR_MANUAL, &hr_manual, &hr_len) == ESP_OK &&
      hr_len == sizeof(float)) {
    if (hr_manual > 0.0f) hr_manual = 0.0f;
    config_.eq_hr_manual_db = hr_manual;
  }

  size_t name_len = DEV_NAME_MAX;
  if (nvs_get_str(h, NVS_KEY_DEV_NAME, config_.dev_name, &name_len) != ESP_OK) {
    strncpy(config_.dev_name, "Geelen Audio", DEV_NAME_MAX - 1);
    config_.dev_name[DEV_NAME_MAX - 1] = '\0';
  }

  {
    size_t hl = HOSTNAME_MAX;
    if (nvs_get_str(h, NVS_KEY_HOSTNAME, hostname_, &hl) != ESP_OK) {
      hostname_[0] = '\0';
    } else if (hostname_[0] && !identityHostnameValid(hostname_)) {
      ESP_LOGW(TAG, "ignoring invalid stored hostname");
      hostname_[0] = '\0';
    }
  }
  {
    size_t ml = 6;
    uint8_t blob[6] = {};
    if (nvs_get_blob(h, NVS_KEY_STA_MAC, blob, &ml) == ESP_OK && ml == 6 &&
        identitySanitizeCustomMac(blob)) {
      memcpy(sta_mac_, blob, 6);
      sta_mac_set_ = true;
    }
    ml = 6;
    if (nvs_get_blob(h, NVS_KEY_BT_MAC, blob, &ml) == ESP_OK && ml == 6 &&
        identitySanitizeCustomMac(blob)) {
      memcpy(bt_mac_, blob, 6);
      bt_mac_set_ = true;
    }
  }

  config_.dlna_on = nvsGetBool(h, NVS_KEY_DLNA_ON, false);
  config_.voice_feedback = nvsGetBool(h, NVS_KEY_VOICE_FB, true);
  if (nvs_get_u8(h, NVS_KEY_WIFI_PS, &tmp8) == ESP_OK && tmp8 <= 2u) {
    config_.wifi_ps = (WifiPsMode)tmp8;
  } else {
    config_.wifi_ps = WifiPsMode::None;
  }

  uint8_t src = 0;
  bool source_needs_migration = false;
  if (nvs_get_u8(h, NVS_KEY_SOURCE, &src) == ESP_OK) {
    if (src > (uint8_t)AudioSource::TestTone) {
      ESP_LOGW(TAG, "Migrating unknown audio source %u -> Bluetooth", (unsigned)src);
      audio_source_ = AudioSource::Bluetooth;
      source_needs_migration = true;
    } else {
      audio_source_ = (AudioSource)src;
    }
  }

  len = SNAPCAST_HOST_MAX;
  if (nvs_get_str(h, NVS_KEY_SNAP_HOST, snapcast_host_, &len) != ESP_OK) snapcast_host_[0] = '\0';
  uint16_t p = 0;
  if (nvs_get_u16(h, NVS_KEY_SNAP_PORT, &p) == ESP_OK) snapcast_port_ = p;

  len = WIFI_SSID_MAX;
  if (nvs_get_str(h, NVS_KEY_WIFI_SSID, wifi_ssid_, &len) != ESP_OK) wifi_ssid_[0] = '\0';
  len = WIFI_PASS_MAX;
  if (nvs_get_str(h, NVS_KEY_WIFI_PASS, wifi_pass_, &len) != ESP_OK) wifi_pass_[0] = '\0';
  {
    uint8_t ever = 0;
    if (nvs_get_u8(h, NVS_KEY_WIFI_EVER, &ever) == ESP_OK) {
      wifi_ever_flag_present_ = true;
      wifi_ever_ok_ = ever != 0;
    } else {
      wifi_ever_flag_present_ = false;
      wifi_ever_ok_ = false;
    }
  }

  {
    float f = tt_freq_hz_;
    size_t fl = sizeof(f);
    if (nvs_get_blob(h, NVS_KEY_TT_FREQ, &f, &fl) == ESP_OK && fl == sizeof(f) && f >= 20.f && f <= 20000.f) tt_freq_hz_ = f;
    fl = sizeof(f);
    if (nvs_get_blob(h, NVS_KEY_TT_AMP, &f, &fl) == ESP_OK && fl == sizeof(f) && f >= -60.f && f <= 0.f) tt_amp_db_ = f;
    fl = sizeof(f);
    if (nvs_get_blob(h, NVS_KEY_TT_PAN, &f, &fl) == ESP_OK && fl == sizeof(f) && f >= -1.f && f <= 1.f) tt_pan_ = f;
    if (nvs_get_u8(h, NVS_KEY_TT_MODE, &tmp8) == ESP_OK && tmp8 <= 10u) tt_mode_ = tmp8;
    fl = sizeof(f);
    if (nvs_get_blob(h, NVS_KEY_TT_SWEEP, &f, &fl) == ESP_OK && fl == sizeof(f) && f >= 1.f && f <= 60.f) tt_sweep_s_ = f;
    fl = sizeof(f);
    if (nvs_get_blob(h, NVS_KEY_TT_SWP_LO, &f, &fl) == ESP_OK && fl == sizeof(f) && f >= 20.f && f <= 20000.f) tt_sweep_lo_hz_ = f;
    fl = sizeof(f);
    if (nvs_get_blob(h, NVS_KEY_TT_SWP_HI, &f, &fl) == ESP_OK && fl == sizeof(f) && f >= 20.f && f <= 20000.f) tt_sweep_hi_hz_ = f;
    fl = sizeof(f);
    if (nvs_get_blob(h, NVS_KEY_TT_DF2, &f, &fl) == ESP_OK && fl == sizeof(f) && f >= 20.f && f <= 20000.f) tt_dual_f2_hz_ = f;
    if (tt_sweep_hi_hz_ <= tt_sweep_lo_hz_) tt_sweep_hi_hz_ = tt_sweep_lo_hz_ + 100.f;
  }

  nvs_close(h);

  if (source_needs_migration) {
    nvs_handle_t wh;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &wh) == ESP_OK) {
      (void)nvs_erase_key(wh, NVS_KEY_LEGACY_SPOTIFY);
      (void)nvs_commit(wh);
      nvs_close(wh);
    }
    markDirtyAll();
  }

  if (config_.volume > VOLUME_MAX) config_.volume = VOLUME_DEFAULT;
  if (config_.vol_l > VOLUME_MAX) config_.vol_l = VOLUME_DEFAULT;
  if (config_.vol_r > VOLUME_MAX) config_.vol_r = VOLUME_DEFAULT;
  if (config_.preset_id >= NUM_FIXED_PRESETS + NUM_CUSTOM_SLOTS) config_.preset_id = 0;
  return ok;
}

bool ConfigManager::save() {
  if (!dirty_) return true;
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;

  const uint8_t dm = dirty_domains_;
  if (dm == 0) {
    nvs_close(h);
    dirty_ = false;
    return true;
  }

  if (dm & D_EQ) {
    nvs_set_u8(h, NVS_KEY_PRESET, config_.preset_id);
    nvs_set_blob(h, NVS_KEY_EQ_LEGACY, config_.eq_gains, EQ_BANDS * sizeof(int8_t));
    // Single-layer PEQ. Legacy `eq_cal*` / `eq_pref*` keys are intentionally
    // NOT rewritten so a downgrade to old firmware falls back cleanly to its
    // defaults (better than partially-mangled state).
    nvs_set_blob(h, NVS_KEY_EQ_BANDS, config_.eq_bands, sizeof(config_.eq_bands));
    nvs_set_u8(h, NVS_KEY_EQ_ON, config_.eq_on ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_EQ_NB, config_.eq_active_bands);
    nvs_set_u8(h, NVS_KEY_EQ_SLOT, config_.eq_active_slot);
    nvs_set_u8(h, NVS_KEY_EQ_HR_MODE, config_.eq_hr_mode == HeadroomMode::Manual ? 1 : 0);
    nvs_set_blob(h, NVS_KEY_EQ_HR_MANUAL, &config_.eq_hr_manual_db, sizeof(float));
  }
  if (dm & D_AUDIO) {
    nvs_set_u8(h, NVS_KEY_VOLUME, config_.volume);
    nvs_set_u8(h, NVS_KEY_VOL_L, config_.vol_l);
    nvs_set_u8(h, NVS_KEY_VOL_R, config_.vol_r);
    nvs_set_u8(h, NVS_KEY_MUTED, config_.muted ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_CH_MODE, config_.stereo_mode == StereoMode::Mono ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_CH_L_ON, config_.ch_l_on ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_CH_R_ON, config_.ch_r_on ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_LED_R, config_.led_r);
    nvs_set_u8(h, NVS_KEY_LED_G, config_.led_g);
    nvs_set_u8(h, NVS_KEY_LED_B, config_.led_b);
    nvs_set_u8(h, NVS_KEY_LED_BRI, config_.led_bri);
    nvs_set_u8(h, NVS_KEY_LED_USER, config_.led_user ? 1 : 0);
  }
  if (dm & D_NET) {
    nvs_set_u8(h, NVS_KEY_SOURCE, (uint8_t)audio_source_);
    nvs_set_str(h, NVS_KEY_SNAP_HOST, snapcast_host_);
    nvs_set_u16(h, NVS_KEY_SNAP_PORT, snapcast_port_);
    nvs_set_str(h, NVS_KEY_WIFI_SSID, wifi_ssid_);
    nvs_set_str(h, NVS_KEY_WIFI_PASS, wifi_pass_);
    if (wifi_ever_flag_present_) {
      nvs_set_u8(h, NVS_KEY_WIFI_EVER, wifi_ever_ok_ ? 1 : 0);
    }
    nvs_set_str(h, NVS_KEY_DEV_NAME, config_.dev_name);
    if (hostname_[0]) {
      nvs_set_str(h, NVS_KEY_HOSTNAME, hostname_);
    } else {
      nvs_erase_key(h, NVS_KEY_HOSTNAME);
    }
    if (sta_mac_set_) {
      nvs_set_blob(h, NVS_KEY_STA_MAC, sta_mac_, 6);
    } else {
      nvs_erase_key(h, NVS_KEY_STA_MAC);
    }
    if (bt_mac_set_) {
      nvs_set_blob(h, NVS_KEY_BT_MAC, bt_mac_, 6);
    } else {
      nvs_erase_key(h, NVS_KEY_BT_MAC);
    }
    nvs_set_u8(h, NVS_KEY_DLNA_ON, config_.dlna_on ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_VOICE_FB, config_.voice_feedback ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_WIFI_PS, (uint8_t)config_.wifi_ps);
  }
  if (dm & D_TESTTONE) {
    nvs_set_blob(h, NVS_KEY_TT_FREQ, &tt_freq_hz_, sizeof(tt_freq_hz_));
    nvs_set_blob(h, NVS_KEY_TT_AMP, &tt_amp_db_, sizeof(tt_amp_db_));
    nvs_set_blob(h, NVS_KEY_TT_PAN, &tt_pan_, sizeof(tt_pan_));
    nvs_set_u8(h, NVS_KEY_TT_MODE, tt_mode_);
    nvs_set_blob(h, NVS_KEY_TT_SWEEP, &tt_sweep_s_, sizeof(tt_sweep_s_));
    nvs_set_blob(h, NVS_KEY_TT_SWP_LO, &tt_sweep_lo_hz_, sizeof(tt_sweep_lo_hz_));
    nvs_set_blob(h, NVS_KEY_TT_SWP_HI, &tt_sweep_hi_hz_, sizeof(tt_sweep_hi_hz_));
    nvs_set_blob(h, NVS_KEY_TT_DF2, &tt_dual_f2_hz_, sizeof(tt_dual_f2_hz_));
  }

  esp_err_t err = nvs_commit(h);
  nvs_close(h);
  if (err == ESP_OK) {
    dirty_ = false;
    dirty_domains_ = 0;
  }
  return err == ESP_OK;
}

void ConfigManager::tick(unsigned long now_ms) {
  if (!initialized_ || !dirty_) return;
  if (now_ms - last_change_ms_ >= SAVE_DEBOUNCE_MS) save();
}

void ConfigManager::markDirtyEq() {
  dirty_ = true;
  dirty_domains_ |= D_EQ;
  last_change_ms_ = cfgWallMs();
  state_version_++;
}
void ConfigManager::markDirtyAudio() {
  dirty_ = true;
  dirty_domains_ |= D_AUDIO;
  last_change_ms_ = cfgWallMs();
  state_version_++;
}
void ConfigManager::markDirtyNet() {
  dirty_ = true;
  dirty_domains_ |= D_NET;
  last_change_ms_ = cfgWallMs();
  state_version_++;
}
void ConfigManager::markDirtyTestTone() {
  dirty_ = true;
  dirty_domains_ |= D_TESTTONE;
  last_change_ms_ = cfgWallMs();
  state_version_++;
}
void ConfigManager::markDirtyAll() {
  dirty_ = true;
  dirty_domains_ = 0xFF;
  last_change_ms_ = cfgWallMs();
  state_version_++;
}

void ConfigManager::setTestToneParams(float freq_hz, float amp_db, float pan, uint8_t mode, float sweep_s,
                                      float sweep_lo_hz, float sweep_hi_hz, float dual_f2_hz) {
  if (freq_hz < 20.f) freq_hz = 20.f;
  if (freq_hz > 20000.f) freq_hz = 20000.f;
  if (amp_db < -60.f) amp_db = -60.f;
  if (amp_db > 0.f) amp_db = 0.f;
  if (pan < -1.f) pan = -1.f;
  if (pan > 1.f) pan = 1.f;
  if (mode > 10u) mode = 0;
  if (sweep_s < 1.f) sweep_s = 1.f;
  if (sweep_s > 60.f) sweep_s = 60.f;
  if (sweep_lo_hz < 20.f) sweep_lo_hz = 20.f;
  if (sweep_lo_hz > 20000.f) sweep_lo_hz = 20000.f;
  if (sweep_hi_hz < 20.f) sweep_hi_hz = 20.f;
  if (sweep_hi_hz > 20000.f) sweep_hi_hz = 20000.f;
  if (sweep_hi_hz <= sweep_lo_hz) sweep_hi_hz = sweep_lo_hz + 100.f;
  if (dual_f2_hz < 20.f) dual_f2_hz = 20.f;
  if (dual_f2_hz > 20000.f) dual_f2_hz = 20000.f;
  tt_freq_hz_ = freq_hz;
  tt_amp_db_ = amp_db;
  tt_pan_ = pan;
  tt_mode_ = mode;
  tt_sweep_s_ = sweep_s;
  tt_sweep_lo_hz_ = sweep_lo_hz;
  tt_sweep_hi_hz_ = sweep_hi_hz;
  tt_dual_f2_hz_ = dual_f2_hz;
  markDirtyTestTone();
}

// ---- Volume ---------------------------------------------------------------

void ConfigManager::setVolume(uint8_t v) {
  if (v > VOLUME_MAX) v = VOLUME_MAX;
  config_.volume = v;
  config_.vol_l = v;
  config_.vol_r = v;
  markDirtyAudio();
}

void ConfigManager::setVolumeLR(uint8_t l, uint8_t r) {
  if (l > VOLUME_MAX) l = VOLUME_MAX;
  if (r > VOLUME_MAX) r = VOLUME_MAX;
  config_.vol_l = l;
  config_.vol_r = r;
  // Legacy scalar: pick the greater of the two so the Snap ServerSettings
  // fallback path still reflects what the user asked for.
  config_.volume = (l > r) ? l : r;
  markDirtyAudio();
}

void ConfigManager::setMuted(bool m) {
  config_.muted = m;
  markDirtyAudio();
}

// ---- Channels -------------------------------------------------------------

void ConfigManager::setStereoMode(StereoMode m) {
  config_.stereo_mode = m;
  markDirtyAudio();
}

void ConfigManager::setChannels(bool l_on, bool r_on) {
  config_.ch_l_on = l_on;
  config_.ch_r_on = r_on;
  markDirtyAudio();
}

void ConfigManager::setLed(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
  config_.led_r = r;
  config_.led_g = g;
  config_.led_b = b;
  config_.led_bri = brightness > 100 ? 100 : brightness;
  config_.led_user = true;
  markDirtyAudio();
}

// ---- Preset (legacy) ------------------------------------------------------

void ConfigManager::setPresetId(uint8_t id) {
  if (id >= NUM_FIXED_PRESETS + NUM_CUSTOM_SLOTS) return;
  config_.preset_id = id;
  markDirtyEq();
}

void ConfigManager::getEQGains(int8_t out[EQ_BANDS]) const {
  for (size_t i = 0; i < EQ_BANDS; i++) out[i] = config_.eq_gains[i];
}

void ConfigManager::setEQGains(const int8_t gains[EQ_BANDS]) {
  for (size_t i = 0; i < EQ_BANDS; i++) {
    int8_t g = gains[i];
    if (g < EQ_GAIN_MIN) g = EQ_GAIN_MIN;
    if (g > EQ_GAIN_MAX) g = EQ_GAIN_MAX;
    config_.eq_gains[i] = g;
  }
  markDirtyEq();
}

// ---- Parametric EQ --------------------------------------------------------

static float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static void clampBand(ParametricBand& b) {
  b.freq_hz = clampf(b.freq_hz, 20.0f, 20000.0f);
  b.gain_db = clampf(b.gain_db, -18.0f, 18.0f);
  const float q_hi = (b.type == geelen::EqFilterType::Notch) ? 100.0f : 10.0f;
  b.q = clampf(b.q, 0.1f, q_hi);
  if (b.slope_db_oct != 6 && b.slope_db_oct != 12 && b.slope_db_oct != 24) b.slope_db_oct = 12;
  const uint8_t t = (uint8_t)b.type;
  if (t > (uint8_t)geelen::EqFilterType::HighPass) b.type = geelen::EqFilterType::Bell;
}

void ConfigManager::getEqBands(ParametricBand out[PARAMETRIC_BANDS]) const {
  for (size_t i = 0; i < PARAMETRIC_BANDS; ++i) out[i] = config_.eq_bands[i];
}
void ConfigManager::setEqBands(const ParametricBand bands[PARAMETRIC_BANDS]) {
  for (size_t i = 0; i < PARAMETRIC_BANDS; ++i) {
    config_.eq_bands[i] = bands[i];
    clampBand(config_.eq_bands[i]);
  }
  // Any live edit marks the slot "modified"; the preset-load handler re-sets
  // this to the slot index after assigning.
  config_.eq_active_slot = 0xFF;
  markDirtyEq();
}
void ConfigManager::setEqEnabled(bool on) {
  config_.eq_on = on;
  markDirtyEq();
}
void ConfigManager::setEqActiveSlot(uint8_t slot) {
  config_.eq_active_slot = slot;
  markDirtyEq();
}
void ConfigManager::setEqActiveBands(uint8_t n) {
  if (n < 1) n = 1;
  if (n > (uint8_t)PARAMETRIC_BANDS) n = (uint8_t)PARAMETRIC_BANDS;
  config_.eq_active_bands = n;
  markDirtyEq();
}

void ConfigManager::setHeadroom(HeadroomMode mode, float manual_db) {
  config_.eq_hr_mode = mode;
  if (manual_db > 0.0f) manual_db = 0.0f;
  if (manual_db < -60.0f) manual_db = -60.0f;
  config_.eq_hr_manual_db = manual_db;
  markDirtyEq();
}

// ---- Audio source / Snapcast / WiFi --------------------------------------

void ConfigManager::setAudioSource(AudioSource s) {
  audio_source_ = s;
  markDirtyNet();
}

bool ConfigManager::getSnapcastHost(char* out, size_t max_len) const {
  if (!out || max_len == 0) return false;
  strncpy(out, snapcast_host_, max_len - 1);
  out[max_len - 1] = '\0';
  return true;
}

void ConfigManager::setSnapcastHost(const char* host) {
  if (!host) return;
  strncpy(snapcast_host_, host, SNAPCAST_HOST_MAX - 1);
  snapcast_host_[SNAPCAST_HOST_MAX - 1] = '\0';
  markDirtyNet();
}

void ConfigManager::setSnapcastPort(uint16_t port) {
  snapcast_port_ = port;
  markDirtyNet();
}

bool ConfigManager::getWifiSsid(char* out, size_t max_len) const {
  if (!out || max_len == 0) return false;
  strncpy(out, wifi_ssid_, max_len - 1);
  out[max_len - 1] = '\0';
  return true;
}

void ConfigManager::setWifiSsid(const char* ssid) {
  if (!ssid) return;
  strncpy(wifi_ssid_, ssid, WIFI_SSID_MAX - 1);
  wifi_ssid_[WIFI_SSID_MAX - 1] = '\0';
  markDirtyNet();
}

bool ConfigManager::getWifiPass(char* out, size_t max_len) const {
  if (!out || max_len == 0) return false;
  strncpy(out, wifi_pass_, max_len - 1);
  out[max_len - 1] = '\0';
  return true;
}

void ConfigManager::setWifiPass(const char* pass) {
  if (!pass) return;
  strncpy(wifi_pass_, pass, WIFI_PASS_MAX - 1);
  wifi_pass_[WIFI_PASS_MAX - 1] = '\0';
  markDirtyNet();
}

void ConfigManager::setWifiEverConnectedOk(bool ok) {
  wifi_ever_ok_ = ok;
  wifi_ever_flag_present_ = true;
  markDirtyNet();
}

// ---- Device name / DLNA ---------------------------------------------------

bool ConfigManager::getDeviceName(char* out, size_t max_len) const {
  if (!out || max_len == 0) return false;
  strncpy(out, config_.dev_name, max_len - 1);
  out[max_len - 1] = '\0';
  return true;
}

void ConfigManager::setDeviceName(const char* name) {
  if (!name) return;
  strncpy(config_.dev_name, name, DEV_NAME_MAX - 1);
  config_.dev_name[DEV_NAME_MAX - 1] = '\0';
  markDirtyNet();
}

bool ConfigManager::getHostname(char* out, size_t max_len) const {
  if (!out || max_len == 0) return false;
  if (hostname_[0]) {
    strncpy(out, hostname_, max_len - 1);
    out[max_len - 1] = '\0';
    return true;
  }
  return identitySlugHostname(config_.dev_name, out, max_len);
}

bool ConfigManager::setHostname(const char* hostname) {
  if (!hostname || !hostname[0]) {
    hostname_[0] = '\0';
    markDirtyNet();
    return true;
  }
  char norm[HOSTNAME_MAX];
  if (!identityNormalizeHostname(hostname, norm, sizeof(norm))) return false;
  strncpy(hostname_, norm, HOSTNAME_MAX - 1);
  hostname_[HOSTNAME_MAX - 1] = '\0';
  markDirtyNet();
  return true;
}

bool ConfigManager::getStaMacOverride(uint8_t out[6]) const {
  if (!sta_mac_set_ || !out) return false;
  memcpy(out, sta_mac_, 6);
  return true;
}

bool ConfigManager::getBtMacOverride(uint8_t out[6]) const {
  if (!bt_mac_set_ || !out) return false;
  memcpy(out, bt_mac_, 6);
  return true;
}

bool ConfigManager::setStaMacOverride(const uint8_t mac[6]) {
  if (!mac) return false;
  uint8_t tmp[6];
  memcpy(tmp, mac, 6);
  if (!identitySanitizeCustomMac(tmp)) return false;
  memcpy(sta_mac_, tmp, 6);
  sta_mac_set_ = true;
  markDirtyNet();
  return true;
}

bool ConfigManager::setBtMacOverride(const uint8_t mac[6]) {
  if (!mac) return false;
  uint8_t tmp[6];
  memcpy(tmp, mac, 6);
  if (!identitySanitizeCustomMac(tmp)) return false;
  memcpy(bt_mac_, tmp, 6);
  bt_mac_set_ = true;
  markDirtyNet();
  return true;
}

void ConfigManager::clearStaMacOverride() {
  sta_mac_set_ = false;
  memset(sta_mac_, 0, sizeof(sta_mac_));
  markDirtyNet();
}

void ConfigManager::clearBtMacOverride() {
  bt_mac_set_ = false;
  memset(bt_mac_, 0, sizeof(bt_mac_));
  markDirtyNet();
}

void ConfigManager::setDlnaEnabled(bool on) {
  config_.dlna_on = on;
  markDirtyNet();
}

void ConfigManager::setVoiceFeedbackEnabled(bool on) {
  config_.voice_feedback = on;
  markDirtyNet();
}

void ConfigManager::setWifiPsMode(WifiPsMode mode) {
  if (config_.wifi_ps == mode) return;
  config_.wifi_ps = mode;
  markDirtyNet();
}

// ---- Reset scopes ---------------------------------------------------------

static void nvsErase(const char* key) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
  (void)nvs_erase_key(h, key);
  (void)nvs_commit(h);
  nvs_close(h);
}

void ConfigManager::factoryReset() {
  loadDefaults();
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    (void)nvs_erase_all(h);
    (void)nvs_commit(h);
    nvs_close(h);
  }
  markDirtyAll();
  save();
}

void ConfigManager::resetWifi() {
  wifi_ssid_[0] = '\0';
  wifi_pass_[0] = '\0';
  wifi_ever_ok_ = false;
  wifi_ever_flag_present_ = true;  // persist the "never verified" marker
  nvsErase(NVS_KEY_WIFI_SSID);
  nvsErase(NVS_KEY_WIFI_PASS);
  nvsErase(NVS_KEY_WIFI_EVER);
  markDirtyNet();
  save();
}

void ConfigManager::resetBluetoothPairings() {
  // Pairing bonds live in the Bluedroid NVS namespace, not ours; the proper
  // path is to call esp_bt_gap_remove_bond_device() for each bonded peer on
  // reboot. We just mark dirty so a `save()` sync happens and rely on the
  // caller to trigger a restart that does the actual bond wipe.
  markDirtyNet();
  save();
}

void ConfigManager::resetSnapcast() {
  snapcast_host_[0] = '\0';
  snapcast_port_ = SNAPCAST_PORT_DEFAULT;
  nvsErase(NVS_KEY_SNAP_HOST);
  nvsErase(NVS_KEY_SNAP_PORT);
  markDirtyNet();
  save();
}

void ConfigManager::resetDlna() {
  config_.dlna_on = false;
  nvsErase(NVS_KEY_DLNA_ON);
  markDirtyNet();
  save();
}

void ConfigManager::resetEq() {
  loadDefaultEqBands(config_.eq_bands);
  config_.eq_active_bands = (uint8_t)PARAMETRIC_BANDS;
  config_.eq_on = false;
  config_.eq_active_slot = 0xFF;
  config_.eq_hr_mode = HeadroomMode::Auto;
  config_.eq_hr_manual_db = 0.0f;
  for (size_t i = 0; i < EQ_BANDS; i++) config_.eq_gains[i] = 0;

  nvsErase(NVS_KEY_EQ_BANDS);
  nvsErase(NVS_KEY_EQ_ON);
  nvsErase(NVS_KEY_EQ_NB);
  nvsErase(NVS_KEY_EQ_SLOT);
  nvsErase(NVS_KEY_EQ_HR_MODE);
  nvsErase(NVS_KEY_EQ_HR_MANUAL);
  nvsErase(NVS_KEY_EQ_LEGACY);
  // Also purge legacy dual-layer keys so a factory reset doesn't leak old
  // state back through the load-time migration on next boot.
  nvsErase(NVS_KEY_LEGACY_EQ_CAL);
  nvsErase(NVS_KEY_LEGACY_EQ_CAL_ON);
  nvsErase(NVS_KEY_LEGACY_EQ_CAL_NB);
  nvsErase(NVS_KEY_LEGACY_EQ_PREF);
  nvsErase(NVS_KEY_LEGACY_EQ_PREF_ON);
  nvsErase(NVS_KEY_LEGACY_EQ_PREF_NB);
  nvsErase(NVS_KEY_LEGACY_EQ_PREF_SLOT);

  // Also clear all SPIFFS preset files so "reset presets" restores factory.
  for (size_t i = 0; i < PRESET_SLOT_COUNT; ++i) {
    char p[64];
    snprintf(p, sizeof(p), "%s/pref_%u.bin", PRESETS_DIR, (unsigned)i);
    (void)remove(p);
  }

  markDirtyEq();
  save();
}

void ConfigManager::resetAudio() {
  config_.volume = VOLUME_DEFAULT;
  config_.vol_l = VOLUME_DEFAULT;
  config_.vol_r = VOLUME_DEFAULT;
  config_.muted = false;
  config_.stereo_mode = StereoMode::Stereo;
  config_.ch_l_on = true;
  config_.ch_r_on = true;
  config_.led_r = 255;
  config_.led_g = 255;
  config_.led_b = 255;
  config_.led_bri = 0;
  config_.led_user = false;
  markDirtyAudio();
  save();
}

void configStartDebouncedSaveTask(ConfigManager* cfg) {
  if (!cfg) return;
  // Stack MUST stay in internal DRAM. tick() → save() → nvs_set_* →
  // spi_flash_read with the cache disabled. A PSRAM stack panics:
  //   esp_task_stack_is_sane_cache_disabled() at cache_utils.c:127
  xTaskCreatePinnedToCore(
      [](void* arg) {
        auto* c = static_cast<ConfigManager*>(arg);
        for (;;) {
          vTaskDelay(pdMS_TO_TICKS(200));
          c->tick(cfgWallMs());
        }
      },
      "cfg_save", 3072, cfg, 3, nullptr, 0);
}

}  // namespace geelen_idf

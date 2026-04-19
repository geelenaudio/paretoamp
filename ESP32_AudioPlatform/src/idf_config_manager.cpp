// ESP-IDF ConfigManager implementation (NVS + esp_littlefs).

#include "idf_config_manager.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "esp_vfs.h"
#include "esp_spiffs.h"

#include <cstring>
#include <cstdio>
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
static const char* NVS_KEY_EQ_CAL        = "eq_cal";
static const char* NVS_KEY_EQ_CAL_ON     = "eq_cal_on";
static const char* NVS_KEY_EQ_PREF       = "eq_pref";
static const char* NVS_KEY_EQ_PREF_ON    = "eq_pref_on";
static const char* NVS_KEY_EQ_PREF_SLOT  = "eq_pref_sl";
static const char* NVS_KEY_EQ_HR_MODE    = "eq_hr_mode";
static const char* NVS_KEY_EQ_HR_MANUAL  = "eq_hr_man";
static const char* NVS_KEY_DEV_NAME      = "dev_name";
static const char* NVS_KEY_DLNA_ON       = "dlna_on";

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

// ISO-262 third-octave centres approximately matching the old 10-band graphic EQ.
static constexpr float kLegacyIsoCentres[PARAMETRIC_BANDS] = {
  31.5f, 63.0f, 125.0f, 250.0f, 500.0f,
  1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f,
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

void ConfigManager::loadDefaultEqCalibration(ParametricBand out[PARAMETRIC_BANDS]) {
  for (size_t i = 0; i < PARAMETRIC_BANDS; ++i) {
    out[i] = { kLegacyIsoCentres[i], 0.0f, 1.2f };
  }
}

void ConfigManager::loadDefaultEqPreference(ParametricBand out[PARAMETRIC_BANDS]) {
  // Same shape as calibration but explicitly flat — the user builds this up
  // in the DSP page; the default is "do nothing".
  for (size_t i = 0; i < PARAMETRIC_BANDS; ++i) {
    out[i] = { kLegacyIsoCentres[i], 0.0f, 1.2f };
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
  config_.preset_id = 0;
  for (size_t i = 0; i < EQ_BANDS; i++) config_.eq_gains[i] = FIXED_PRESET_EQ[0][i];
  loadDefaultEqCalibration(config_.eq_cal);
  config_.eq_cal_on = false;
  loadDefaultEqPreference(config_.eq_pref);
  config_.eq_pref_on = false;
  config_.eq_pref_slot = 0xFF;  // "unsaved"
  config_.eq_hr_mode = HeadroomMode::Auto;
  config_.eq_hr_manual_db = 0.0f;
  strncpy(config_.dev_name, "Geelen Audio", DEV_NAME_MAX - 1);
  config_.dev_name[DEV_NAME_MAX - 1] = '\0';
  config_.dlna_on = false;

  audio_source_ = AudioSource::Bluetooth;
  snapcast_host_[0] = '\0';
  snapcast_port_ = SNAPCAST_PORT_DEFAULT;
  wifi_ssid_[0] = '\0';
  wifi_pass_[0] = '\0';
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
    save();
  }

  dirty_ = false;
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

  if (nvs_get_u8(h, NVS_KEY_PRESET, &config_.preset_id) != ESP_OK) ok = false;

  // Legacy int8_t EQ — if present, migrate into eq_cal on first load.
  size_t len = EQ_BANDS * sizeof(int8_t);
  bool legacy_eq_present =
    nvs_get_blob(h, NVS_KEY_EQ_LEGACY, config_.eq_gains, &len) == ESP_OK &&
    len == EQ_BANDS * sizeof(int8_t);

  // Parametric EQ curves.
  len = sizeof(config_.eq_cal);
  bool have_cal = nvs_get_blob(h, NVS_KEY_EQ_CAL, config_.eq_cal, &len) == ESP_OK &&
                  len == sizeof(config_.eq_cal);
  config_.eq_cal_on = nvsGetBool(h, NVS_KEY_EQ_CAL_ON, false);

  if (!have_cal && legacy_eq_present) {
    // One-time migration: lift the int8 gains into parametric form and
    // enable the calibration layer so the user's old graphic-EQ curve
    // carries forward without any manual step.
    ESP_LOGW(TAG, "Migrating legacy graphic-EQ gains to parametric calibration");
    loadDefaultEqCalibration(config_.eq_cal);
    for (size_t i = 0; i < PARAMETRIC_BANDS; ++i)
      config_.eq_cal[i].gain_db = (float)config_.eq_gains[i];
    config_.eq_cal_on = true;
    markDirty(0);
  } else if (!have_cal) {
    loadDefaultEqCalibration(config_.eq_cal);
  }

  len = sizeof(config_.eq_pref);
  if (nvs_get_blob(h, NVS_KEY_EQ_PREF, config_.eq_pref, &len) != ESP_OK ||
      len != sizeof(config_.eq_pref)) {
    loadDefaultEqPreference(config_.eq_pref);
  }
  config_.eq_pref_on = nvsGetBool(h, NVS_KEY_EQ_PREF_ON, false);
  if (nvs_get_u8(h, NVS_KEY_EQ_PREF_SLOT, &tmp8) == ESP_OK) config_.eq_pref_slot = tmp8;

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

  config_.dlna_on = nvsGetBool(h, NVS_KEY_DLNA_ON, false);

  uint8_t src = 0;
  bool source_needs_migration = false;
  if (nvs_get_u8(h, NVS_KEY_SOURCE, &src) == ESP_OK) {
    if (src > (uint8_t)AudioSource::Dlna) {
      // Pre-rescope firmware used src=2 for Spotify Connect; Dlna is now
      // the canonical src=2, so only src >= 3 is a stale Spotify pointer.
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

  nvs_close(h);

  if (source_needs_migration) {
    nvs_handle_t wh;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &wh) == ESP_OK) {
      (void)nvs_erase_key(wh, NVS_KEY_LEGACY_SPOTIFY);
      (void)nvs_commit(wh);
      nvs_close(wh);
    }
    markDirty(0);
  }

  if (config_.volume > VOLUME_MAX) config_.volume = VOLUME_DEFAULT;
  if (config_.vol_l > VOLUME_MAX) config_.vol_l = VOLUME_DEFAULT;
  if (config_.vol_r > VOLUME_MAX) config_.vol_r = VOLUME_DEFAULT;
  if (config_.preset_id >= NUM_FIXED_PRESETS + NUM_CUSTOM_SLOTS) config_.preset_id = 0;
  return ok;
}

bool ConfigManager::save() {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;

  nvs_set_u8(h, NVS_KEY_VOLUME, config_.volume);
  nvs_set_u8(h, NVS_KEY_PRESET, config_.preset_id);
  nvs_set_blob(h, NVS_KEY_EQ_LEGACY, config_.eq_gains, EQ_BANDS * sizeof(int8_t));
  nvs_set_u8(h, NVS_KEY_SOURCE, (uint8_t)audio_source_);
  nvs_set_str(h, NVS_KEY_SNAP_HOST, snapcast_host_);
  nvs_set_u16(h, NVS_KEY_SNAP_PORT, snapcast_port_);
  nvs_set_str(h, NVS_KEY_WIFI_SSID, wifi_ssid_);
  nvs_set_str(h, NVS_KEY_WIFI_PASS, wifi_pass_);

  nvs_set_u8(h, NVS_KEY_VOL_L, config_.vol_l);
  nvs_set_u8(h, NVS_KEY_VOL_R, config_.vol_r);
  nvs_set_u8(h, NVS_KEY_MUTED, config_.muted ? 1 : 0);
  nvs_set_u8(h, NVS_KEY_CH_MODE, config_.stereo_mode == StereoMode::Mono ? 1 : 0);
  nvs_set_u8(h, NVS_KEY_CH_L_ON, config_.ch_l_on ? 1 : 0);
  nvs_set_u8(h, NVS_KEY_CH_R_ON, config_.ch_r_on ? 1 : 0);

  nvs_set_blob(h, NVS_KEY_EQ_CAL, config_.eq_cal, sizeof(config_.eq_cal));
  nvs_set_u8(h, NVS_KEY_EQ_CAL_ON, config_.eq_cal_on ? 1 : 0);
  nvs_set_blob(h, NVS_KEY_EQ_PREF, config_.eq_pref, sizeof(config_.eq_pref));
  nvs_set_u8(h, NVS_KEY_EQ_PREF_ON, config_.eq_pref_on ? 1 : 0);
  nvs_set_u8(h, NVS_KEY_EQ_PREF_SLOT, config_.eq_pref_slot);

  nvs_set_u8(h, NVS_KEY_EQ_HR_MODE, config_.eq_hr_mode == HeadroomMode::Manual ? 1 : 0);
  nvs_set_blob(h, NVS_KEY_EQ_HR_MANUAL, &config_.eq_hr_manual_db, sizeof(float));

  nvs_set_str(h, NVS_KEY_DEV_NAME, config_.dev_name);
  nvs_set_u8(h, NVS_KEY_DLNA_ON, config_.dlna_on ? 1 : 0);

  esp_err_t err = nvs_commit(h);
  nvs_close(h);
  if (err == ESP_OK) dirty_ = false;
  return err == ESP_OK;
}

void ConfigManager::tick(unsigned long now_ms) {
  if (!initialized_ || !dirty_) return;
  if (now_ms - last_change_ms_ >= SAVE_DEBOUNCE_MS) save();
}

void ConfigManager::markDirty(unsigned long now_ms) {
  dirty_ = true;
  last_change_ms_ = now_ms;
}

// ---- Volume ---------------------------------------------------------------

void ConfigManager::setVolume(uint8_t v) {
  if (v > VOLUME_MAX) v = VOLUME_MAX;
  config_.volume = v;
  config_.vol_l = v;
  config_.vol_r = v;
  markDirty(0);
}

void ConfigManager::setVolumeLR(uint8_t l, uint8_t r) {
  if (l > VOLUME_MAX) l = VOLUME_MAX;
  if (r > VOLUME_MAX) r = VOLUME_MAX;
  config_.vol_l = l;
  config_.vol_r = r;
  // Legacy scalar: pick the greater of the two so the Snap ServerSettings
  // fallback path still reflects what the user asked for.
  config_.volume = (l > r) ? l : r;
  markDirty(0);
}

void ConfigManager::setMuted(bool m) {
  config_.muted = m;
  markDirty(0);
}

// ---- Channels -------------------------------------------------------------

void ConfigManager::setStereoMode(StereoMode m) {
  config_.stereo_mode = m;
  markDirty(0);
}

void ConfigManager::setChannels(bool l_on, bool r_on) {
  config_.ch_l_on = l_on;
  config_.ch_r_on = r_on;
  markDirty(0);
}

// ---- Preset (legacy) ------------------------------------------------------

void ConfigManager::setPresetId(uint8_t id) {
  if (id >= NUM_FIXED_PRESETS + NUM_CUSTOM_SLOTS) return;
  config_.preset_id = id;
  markDirty(0);
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
  markDirty(0);
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
  b.q       = clampf(b.q, 0.1f, 10.0f);
}

void ConfigManager::getEqCalibration(ParametricBand out[PARAMETRIC_BANDS]) const {
  for (size_t i = 0; i < PARAMETRIC_BANDS; ++i) out[i] = config_.eq_cal[i];
}
void ConfigManager::setEqCalibration(const ParametricBand bands[PARAMETRIC_BANDS]) {
  for (size_t i = 0; i < PARAMETRIC_BANDS; ++i) {
    config_.eq_cal[i] = bands[i];
    clampBand(config_.eq_cal[i]);
  }
  markDirty(0);
}
void ConfigManager::setEqCalibrationEnabled(bool on) {
  config_.eq_cal_on = on;
  markDirty(0);
}

void ConfigManager::getEqPreference(ParametricBand out[PARAMETRIC_BANDS]) const {
  for (size_t i = 0; i < PARAMETRIC_BANDS; ++i) out[i] = config_.eq_pref[i];
}
void ConfigManager::setEqPreference(const ParametricBand bands[PARAMETRIC_BANDS]) {
  for (size_t i = 0; i < PARAMETRIC_BANDS; ++i) {
    config_.eq_pref[i] = bands[i];
    clampBand(config_.eq_pref[i]);
  }
  // Any live edit marks the slot "modified"; the preset-load handler re-sets
  // this to the slot index after assigning.
  config_.eq_pref_slot = 0xFF;
  markDirty(0);
}
void ConfigManager::setEqPreferenceEnabled(bool on) {
  config_.eq_pref_on = on;
  markDirty(0);
}
void ConfigManager::setEqPreferenceSlot(uint8_t slot) {
  config_.eq_pref_slot = slot;
  markDirty(0);
}

void ConfigManager::setHeadroom(HeadroomMode mode, float manual_db) {
  config_.eq_hr_mode = mode;
  if (manual_db > 0.0f) manual_db = 0.0f;
  if (manual_db < -60.0f) manual_db = -60.0f;
  config_.eq_hr_manual_db = manual_db;
  markDirty(0);
}

// ---- Audio source / Snapcast / WiFi --------------------------------------

void ConfigManager::setAudioSource(AudioSource s) {
  audio_source_ = s;
  markDirty(0);
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
  markDirty(0);
}

void ConfigManager::setSnapcastPort(uint16_t port) {
  snapcast_port_ = port;
  markDirty(0);
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
  markDirty(0);
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
  markDirty(0);
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
  markDirty(0);
}

void ConfigManager::setDlnaEnabled(bool on) {
  config_.dlna_on = on;
  markDirty(0);
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
  save();
}

void ConfigManager::resetWifi() {
  wifi_ssid_[0] = '\0';
  wifi_pass_[0] = '\0';
  nvsErase(NVS_KEY_WIFI_SSID);
  nvsErase(NVS_KEY_WIFI_PASS);
  markDirty(0);
  save();
}

void ConfigManager::resetBluetoothPairings() {
  // Pairing bonds live in the Bluedroid NVS namespace, not ours; the proper
  // path is to call esp_bt_gap_remove_bond_device() for each bonded peer on
  // reboot. We just mark dirty so a `save()` sync happens and rely on the
  // caller to trigger a restart that does the actual bond wipe.
  markDirty(0);
  save();
}

void ConfigManager::resetSnapcast() {
  snapcast_host_[0] = '\0';
  snapcast_port_ = SNAPCAST_PORT_DEFAULT;
  nvsErase(NVS_KEY_SNAP_HOST);
  nvsErase(NVS_KEY_SNAP_PORT);
  markDirty(0);
  save();
}

void ConfigManager::resetDlna() {
  config_.dlna_on = false;
  nvsErase(NVS_KEY_DLNA_ON);
  markDirty(0);
  save();
}

void ConfigManager::resetEq() {
  loadDefaultEqCalibration(config_.eq_cal);
  config_.eq_cal_on = false;
  loadDefaultEqPreference(config_.eq_pref);
  config_.eq_pref_on = false;
  config_.eq_pref_slot = 0xFF;
  config_.eq_hr_mode = HeadroomMode::Auto;
  config_.eq_hr_manual_db = 0.0f;
  for (size_t i = 0; i < EQ_BANDS; i++) config_.eq_gains[i] = 0;

  nvsErase(NVS_KEY_EQ_CAL);
  nvsErase(NVS_KEY_EQ_CAL_ON);
  nvsErase(NVS_KEY_EQ_PREF);
  nvsErase(NVS_KEY_EQ_PREF_ON);
  nvsErase(NVS_KEY_EQ_PREF_SLOT);
  nvsErase(NVS_KEY_EQ_HR_MODE);
  nvsErase(NVS_KEY_EQ_HR_MANUAL);
  nvsErase(NVS_KEY_EQ_LEGACY);

  // Also clear all SPIFFS preset files so "reset presets" restores factory.
  for (size_t i = 0; i < PRESET_SLOT_COUNT; ++i) {
    char p[64];
    snprintf(p, sizeof(p), "%s/pref_%u.bin", PRESETS_DIR, (unsigned)i);
    (void)remove(p);
  }

  markDirty(0);
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
  markDirty(0);
  save();
}

}  // namespace geelen_idf

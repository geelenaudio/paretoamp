// ESP-IDF-specific ConfigManager implementation (NVS + esp_littlefs)

#include "idf_config_manager.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "esp_vfs.h"
#include "esp_spiffs.h"

#include <cstring>
#include <cstdio>

namespace geelen_idf {

static const char* TAG = "Config";

static const char* NVS_NAMESPACE = "geelen";
static const char* NVS_KEY_VOLUME = "vol";
static const char* NVS_KEY_PRESET = "preset";
static const char* NVS_KEY_EQ = "eq";
static const char* NVS_KEY_SOURCE = "src";
static const char* NVS_KEY_SNAP_HOST = "snap_h";
static const char* NVS_KEY_SNAP_PORT = "snap_p";
static const char* NVS_KEY_WIFI_SSID = "wifi_ssid";
static const char* NVS_KEY_WIFI_PASS = "wifi_pass";
static const char* NVS_KEY_SPOTIFY = "spotify_js";

static const char* MOUNT_POINT = "/spiffs";
static const char* PRESETS_DIR = "/spiffs/presets";

static const char* FIXED_PRESET_NAMES[] = { "Flat", "Bass Boost", "Vocals", "Treble Clarity" };
static const int8_t FIXED_PRESET_EQ[][EQ_BANDS] = {
  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
  { 6, 4, 2, 0, 0, 0, -1, -2, -2, -2 },
  { -2, 0, 2, 3, 2, 0, -1, -1, 0, 0 },
  { -3, -2, -1, 0, 1, 2, 3, 4, 4, 5 },
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
  spotify_cred_json_[0] = '\0';
}

void ConfigManager::loadDefaults() {
  config_.volume = VOLUME_DEFAULT;
  config_.preset_id = 0;
  for (size_t i = 0; i < EQ_BANDS; i++) config_.eq_gains[i] = FIXED_PRESET_EQ[0][i];
  audio_source_ = AudioSource::Bluetooth;
  snapcast_host_[0] = '\0';
  snapcast_port_ = SNAPCAST_PORT_DEFAULT;
  wifi_ssid_[0] = '\0';
  wifi_pass_[0] = '\0';
  spotify_cred_json_[0] = '\0';
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

  if (!load()) {
    loadDefaults();
    save();
  }

  dirty_ = false;
  last_change_ms_ = 0;
  initialized_ = true;
  return true;
}

bool ConfigManager::load() {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

  bool ok = true;
  if (nvs_get_u8(h, NVS_KEY_VOLUME, &config_.volume) != ESP_OK) ok = false;
  if (nvs_get_u8(h, NVS_KEY_PRESET, &config_.preset_id) != ESP_OK) ok = false;

  size_t len = EQ_BANDS * sizeof(int8_t);
  if (nvs_get_blob(h, NVS_KEY_EQ, config_.eq_gains, &len) != ESP_OK || len != EQ_BANDS * sizeof(int8_t)) ok = false;

  uint8_t src = 0;
  if (nvs_get_u8(h, NVS_KEY_SOURCE, &src) == ESP_OK) audio_source_ = (AudioSource)src;

  len = SNAPCAST_HOST_MAX;
  if (nvs_get_str(h, NVS_KEY_SNAP_HOST, snapcast_host_, &len) != ESP_OK) snapcast_host_[0] = '\0';
  uint16_t p = 0;
  if (nvs_get_u16(h, NVS_KEY_SNAP_PORT, &p) == ESP_OK) snapcast_port_ = p;

  len = WIFI_SSID_MAX;
  if (nvs_get_str(h, NVS_KEY_WIFI_SSID, wifi_ssid_, &len) != ESP_OK) wifi_ssid_[0] = '\0';
  len = WIFI_PASS_MAX;
  if (nvs_get_str(h, NVS_KEY_WIFI_PASS, wifi_pass_, &len) != ESP_OK) wifi_pass_[0] = '\0';

  len = SPOTIFY_CRED_JSON_MAX;
  if (nvs_get_str(h, NVS_KEY_SPOTIFY, spotify_cred_json_, &len) != ESP_OK) spotify_cred_json_[0] = '\0';

  nvs_close(h);

  if (config_.volume > VOLUME_MAX) config_.volume = VOLUME_DEFAULT;
  if (config_.preset_id >= NUM_FIXED_PRESETS + NUM_CUSTOM_SLOTS) config_.preset_id = 0;
  return ok;
}

bool ConfigManager::save() {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;

  nvs_set_u8(h, NVS_KEY_VOLUME, config_.volume);
  nvs_set_u8(h, NVS_KEY_PRESET, config_.preset_id);
  nvs_set_blob(h, NVS_KEY_EQ, config_.eq_gains, EQ_BANDS * sizeof(int8_t));
  nvs_set_u8(h, NVS_KEY_SOURCE, (uint8_t)audio_source_);
  nvs_set_str(h, NVS_KEY_SNAP_HOST, snapcast_host_);
  nvs_set_u16(h, NVS_KEY_SNAP_PORT, snapcast_port_);
  nvs_set_str(h, NVS_KEY_WIFI_SSID, wifi_ssid_);
  nvs_set_str(h, NVS_KEY_WIFI_PASS, wifi_pass_);
  nvs_set_str(h, NVS_KEY_SPOTIFY, spotify_cred_json_);

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

void ConfigManager::setVolume(uint8_t v) {
  if (v > VOLUME_MAX) v = VOLUME_MAX;
  config_.volume = v;
  markDirty(0);
}

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

bool ConfigManager::getSpotifyCredentials(char* out, size_t max_len) const {
  if (!out || max_len == 0) return false;
  strncpy(out, spotify_cred_json_, max_len - 1);
  out[max_len - 1] = '\0';
  return spotify_cred_json_[0] != '\0';
}

void ConfigManager::setSpotifyCredentials(const char* json) {
  if (!json) {
    spotify_cred_json_[0] = '\0';
    markDirty(0);
    return;
  }
  strncpy(spotify_cred_json_, json, SPOTIFY_CRED_JSON_MAX - 1);
  spotify_cred_json_[SPOTIFY_CRED_JSON_MAX - 1] = '\0';
  markDirty(0);
}

void ConfigManager::clearSpotifyCredentials() {
  spotify_cred_json_[0] = '\0';
  markDirty(0);
}

void ConfigManager::factoryReset() {
  loadDefaults();
  clearSpotifyCredentials();
  save();
}

}  // namespace geelen_idf


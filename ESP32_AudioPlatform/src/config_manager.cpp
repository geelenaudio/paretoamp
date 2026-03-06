/**
 * Config Manager – implementation (NVS + LittleFS presets).
 */

#include "config_manager.h"
#include <LittleFS.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <Arduino.h>
#include <cstring>

namespace geelen {

static const char* NVS_NAMESPACE = "geelen";
static const char* NVS_KEY_VOLUME = "vol";
static const char* NVS_KEY_PRESET = "preset";
static const char* NVS_KEY_EQ = "eq";
static const char* NVS_KEY_LAST_BT = "last_bt";
static const char* NVS_KEY_SOURCE = "src";
static const char* NVS_KEY_SNAP_HOST = "snap_h";
static const char* NVS_KEY_SNAP_PORT = "snap_p";
static const char* NVS_KEY_WIFI_SSID = "wifi_ssid";
static const char* NVS_KEY_WIFI_PASS = "wifi_pass";
static const char* PRESETS_DIR = "/presets";

static const char* FIXED_PRESET_NAMES[] = { "Flat", "Bass Boost", "Vocals", "Treble Clarity" };

static const int8_t FIXED_PRESET_EQ[][EQ_BANDS] = {
  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
  { 6, 4, 2, 0, 0, 0, -1, -2, -2, -2 },
  { -2, 0, 2, 3, 2, 0, -1, -1, 0, 0 },
  { -3, -2, -1, 0, 1, 2, 3, 4, 4, 5 },
};

static void makePresetPath(char* buf, size_t buf_size, size_t slot_index, const char* suffix) {
  snprintf(buf, buf_size, "%s/custom_%u%s", PRESETS_DIR, (unsigned)slot_index, suffix);
}

void ConfigManager::loadDefaults() {
  config_.volume = VOLUME_DEFAULT;
  config_.preset_id = 0;
  for (size_t i = 0; i < EQ_BANDS; i++) {
    config_.eq_gains[i] = FIXED_PRESET_EQ[0][i];
  }
  audio_source_ = AudioSource::Bluetooth;
  snapcast_host_[0] = '\0';
  snapcast_port_ = SNAPCAST_PORT_DEFAULT;
  wifi_ssid_[0] = '\0';
  wifi_pass_[0] = '\0';
}

void ConfigManager::ensurePresetsDir() {
  if (!LittleFS.exists(PRESETS_DIR)) {
    LittleFS.mkdir(PRESETS_DIR);
  }
}

bool ConfigManager::begin() {
  if (initialized_) return true;

  if (!LittleFS.begin(true)) {
    Serial.println("[Config] LittleFS mount failed, using NVS only");
  } else {
    ensurePresetsDir();
  }

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    Serial.printf("[Config] NVS init failed: %d\n", err);
    loadDefaults();
    initialized_ = true;
    return true;
  }

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

  size_t len;
  bool ok = true;
  if (nvs_get_u8(h, NVS_KEY_VOLUME, &config_.volume) != ESP_OK) ok = false;
  if (nvs_get_u8(h, NVS_KEY_PRESET, &config_.preset_id) != ESP_OK) ok = false;
  len = EQ_BANDS * sizeof(int8_t);
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

  esp_err_t err = nvs_commit(h);
  nvs_close(h);
  if (err == ESP_OK) dirty_ = false;
  return err == ESP_OK;
}

void ConfigManager::tick(unsigned long now_ms) {
  if (!initialized_ || !dirty_) return;
  if (now_ms - last_change_ms_ >= SAVE_DEBOUNCE_MS) {
    save();
  }
}

void ConfigManager::markDirty() {
  dirty_ = true;
  last_change_ms_ = millis();
}

void ConfigManager::setVolume(uint8_t v) {
  if (v > VOLUME_MAX) v = VOLUME_MAX;
  config_.volume = v;
  markDirty();
}

void ConfigManager::setPresetId(uint8_t id) {
  if (id >= NUM_FIXED_PRESETS + NUM_CUSTOM_SLOTS) return;
  config_.preset_id = id;
  markDirty();
}

void ConfigManager::getEQGains(int8_t out[EQ_BANDS]) const {
  for (size_t i = 0; i < EQ_BANDS; i++) out[i] = config_.eq_gains[i];
}

void ConfigManager::setEQGain(size_t band, int8_t gain_db) {
  if (band >= EQ_BANDS) return;
  if (gain_db < EQ_GAIN_MIN) gain_db = EQ_GAIN_MIN;
  if (gain_db > EQ_GAIN_MAX) gain_db = EQ_GAIN_MAX;
  config_.eq_gains[band] = gain_db;
  markDirty();
}

void ConfigManager::setEQGains(const int8_t gains[EQ_BANDS]) {
  for (size_t i = 0; i < EQ_BANDS; i++) {
    int8_t g = gains[i];
    if (g < EQ_GAIN_MIN) g = EQ_GAIN_MIN;
    if (g > EQ_GAIN_MAX) g = EQ_GAIN_MAX;
    config_.eq_gains[i] = g;
  }
  markDirty();
}

bool ConfigManager::getLastBtDevice(char* out, size_t max_len) const {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  size_t len = max_len;
  esp_err_t err = nvs_get_str(h, NVS_KEY_LAST_BT, out, &len);
  nvs_close(h);
  if (err != ESP_OK) { if (max_len) out[0] = '\0'; return false; }
  return true;
}

void ConfigManager::setLastBtDevice(const char* addr) {
  if (!addr) return;
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, NVS_KEY_LAST_BT, addr);
  nvs_commit(h);
  nvs_close(h);
}

const char* ConfigManager::getFixedPresetName(size_t index) const {
  if (index >= NUM_FIXED_PRESETS) return "";
  return FIXED_PRESET_NAMES[index];
}

void ConfigManager::loadPreset(uint8_t id) {
  if (id < NUM_FIXED_PRESETS) {
    for (size_t i = 0; i < EQ_BANDS; i++) config_.eq_gains[i] = FIXED_PRESET_EQ[id][i];
    config_.preset_id = id;
  } else if (id < NUM_FIXED_PRESETS + NUM_CUSTOM_SLOTS) {
    size_t slot = id - NUM_FIXED_PRESETS;
    int8_t loaded[EQ_BANDS];
    if (getCustomPresetEQ(slot, loaded)) {
      for (size_t i = 0; i < EQ_BANDS; i++) config_.eq_gains[i] = loaded[i];
      config_.preset_id = id;
    }
  }
  markDirty();
}

bool ConfigManager::getCustomPresetName(size_t slot_index, char* out, size_t max_len) const {
  if (slot_index >= NUM_CUSTOM_SLOTS || max_len == 0) return false;
  char path[64];
  makePresetPath(path, sizeof(path), slot_index, "_name.txt");
  File f = LittleFS.open(path, "r");
  if (!f) { out[0] = '\0'; return false; }
  size_t n = f.read((uint8_t*)out, max_len - 1);
  f.close();
  out[n < max_len - 1 ? n : max_len - 1] = '\0';
  return true;
}

void ConfigManager::setCustomPresetName(size_t slot_index, const char* name) {
  if (slot_index >= NUM_CUSTOM_SLOTS || !name) return;
  ensurePresetsDir();
  char path[64];
  makePresetPath(path, sizeof(path), slot_index, "_name.txt");
  File f = LittleFS.open(path, "w");
  if (f) {
    size_t len = strnlen(name, MAX_PRESET_NAME);
    f.write((const uint8_t*)name, len);
    f.close();
  }
}

bool ConfigManager::getCustomPresetEQ(size_t slot_index, int8_t out[EQ_BANDS]) const {
  if (slot_index >= NUM_CUSTOM_SLOTS) return false;
  char path[64];
  makePresetPath(path, sizeof(path), slot_index, "_eq.bin");
  File f = LittleFS.open(path, "r");
  if (!f || f.size() != (int)(EQ_BANDS * sizeof(int8_t))) { f.close(); return false; }
  f.read((uint8_t*)out, EQ_BANDS * sizeof(int8_t));
  f.close();
  return true;
}

bool ConfigManager::saveCustomPreset(size_t slot_index, const char* name, const int8_t eq[EQ_BANDS]) {
  if (slot_index >= NUM_CUSTOM_SLOTS || !eq) return false;
  ensurePresetsDir();
  char path[64];
  makePresetPath(path, sizeof(path), slot_index, "_eq.bin");
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  f.write((const uint8_t*)eq, EQ_BANDS * sizeof(int8_t));
  f.close();
  if (name) setCustomPresetName(slot_index, name);
  return true;
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
  markDirty();
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
  markDirty();
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
  markDirty();
}

void ConfigManager::factoryReset() {
  loadDefaults();
  save();
}

}  // namespace geelen

/**
 * Config Manager – Geelen Audio Amp
 * LittleFS (presets, custom EQ slots) + NVS (volume, preset, last BT device).
 * Save-on-change with debounce.
 */

#ifndef GEELEN_CONFIG_MANAGER_H
#define GEELEN_CONFIG_MANAGER_H

#include <cstdint>
#include <cstddef>

namespace geelen {

constexpr size_t EQ_BANDS = 10;
constexpr int8_t EQ_GAIN_MIN = -12;
constexpr int8_t EQ_GAIN_MAX = 12;
constexpr uint8_t VOLUME_MIN = 0;
constexpr uint8_t VOLUME_MAX = 100;
constexpr uint8_t VOLUME_DEFAULT = 80;

constexpr size_t NUM_FIXED_PRESETS = 4;
constexpr size_t NUM_CUSTOM_SLOTS = 4;
constexpr size_t MAX_PRESET_NAME = 32;
constexpr unsigned long SAVE_DEBOUNCE_MS = 2000;
constexpr size_t LAST_BT_DEVICE_MAX = 18;
constexpr size_t SNAPCAST_HOST_MAX = 64;
constexpr size_t WIFI_SSID_MAX = 32;
constexpr size_t WIFI_PASS_MAX = 64;
constexpr uint16_t SNAPCAST_PORT_DEFAULT = 1705;

enum class AudioSource : uint8_t { Bluetooth = 0, Network = 1 };

struct Config {
  uint8_t volume;
  uint8_t preset_id;
  int8_t eq_gains[EQ_BANDS];
};

class ConfigManager {
 public:
  ConfigManager() : initialized_(false), dirty_(false), last_change_ms_(0),
                    audio_source_(AudioSource::Bluetooth), snapcast_port_(SNAPCAST_PORT_DEFAULT) {
    snapcast_host_[0] = '\0';
    wifi_ssid_[0] = '\0';
    wifi_pass_[0] = '\0';
  }

  bool begin();
  bool isInitialized() const { return initialized_; }

  void tick(unsigned long now_ms);

  uint8_t getVolume() const { return config_.volume; }
  void setVolume(uint8_t v);
  uint8_t getPresetId() const { return config_.preset_id; }
  void setPresetId(uint8_t id);
  void getEQGains(int8_t out[EQ_BANDS]) const;
  void setEQGain(size_t band, int8_t gain_db);
  void setEQGains(const int8_t gains[EQ_BANDS]);

  bool save();
  void markDirty();

  const Config& getConfig() const { return config_; }

  bool getLastBtDevice(char* out, size_t max_len) const;
  void setLastBtDevice(const char* addr);

  const char* getFixedPresetName(size_t index) const;
  void loadPreset(uint8_t id);
  bool getCustomPresetName(size_t slot_index, char* out, size_t max_len) const;
  void setCustomPresetName(size_t slot_index, const char* name);
  bool getCustomPresetEQ(size_t slot_index, int8_t out[EQ_BANDS]) const;
  bool saveCustomPreset(size_t slot_index, const char* name, const int8_t eq[EQ_BANDS]);

  AudioSource getAudioSource() const { return audio_source_; }
  void setAudioSource(AudioSource s) { audio_source_ = s; markDirty(); }
  bool getSnapcastHost(char* out, size_t max_len) const;
  void setSnapcastHost(const char* host);
  uint16_t getSnapcastPort() const { return snapcast_port_; }
  void setSnapcastPort(uint16_t port) { snapcast_port_ = port; markDirty(); }
  bool getWifiSsid(char* out, size_t max_len) const;
  void setWifiSsid(const char* ssid);
  bool getWifiPass(char* out, size_t max_len) const;
  void setWifiPass(const char* pass);

  void factoryReset();

 private:
  void loadDefaults();
  bool load();
  void ensurePresetsDir();
  Config config_;
  bool initialized_;
  bool dirty_;
  unsigned long last_change_ms_;
  AudioSource audio_source_;
  char snapcast_host_[SNAPCAST_HOST_MAX];
  uint16_t snapcast_port_;
  char wifi_ssid_[WIFI_SSID_MAX];
  char wifi_pass_[WIFI_PASS_MAX];
};

}  // namespace geelen

#endif  // GEELEN_CONFIG_MANAGER_H

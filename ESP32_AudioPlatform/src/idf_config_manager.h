// ESP-IDF-specific ConfigManager implementation.
// Keeps the same public surface as the Arduino version, but uses ESP-IDF NVS + esp_littlefs.

#pragma once

#include <cstdint>
#include <cstddef>

namespace geelen_idf {

// Keep constants aligned with Arduino build
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
/** Max JSON length for Spotify reusable credentials (LoginBlob / getCredentialsJson). */
constexpr size_t SPOTIFY_CRED_JSON_MAX = 2048;
constexpr size_t WIFI_SSID_MAX = 32;
constexpr size_t WIFI_PASS_MAX = 64;
constexpr uint16_t SNAPCAST_PORT_DEFAULT = 1705;

enum class AudioSource : uint8_t { Bluetooth = 0, Network = 1, Spotify = 2 };

struct Config {
  uint8_t volume;
  uint8_t preset_id;
  int8_t eq_gains[EQ_BANDS];
};

class ConfigManager {
 public:
  ConfigManager();
  bool begin();
  void tick(unsigned long now_ms);

  uint8_t getVolume() const { return config_.volume; }
  void setVolume(uint8_t v);
  uint8_t getPresetId() const { return config_.preset_id; }
  void setPresetId(uint8_t id);
  void getEQGains(int8_t out[EQ_BANDS]) const;
  void setEQGains(const int8_t gains[EQ_BANDS]);

  AudioSource getAudioSource() const { return audio_source_; }
  void setAudioSource(AudioSource s);

  bool getSnapcastHost(char* out, size_t max_len) const;
  void setSnapcastHost(const char* host);
  uint16_t getSnapcastPort() const { return snapcast_port_; }
  void setSnapcastPort(uint16_t port);

  bool getWifiSsid(char* out, size_t max_len) const;
  void setWifiSsid(const char* ssid);
  bool getWifiPass(char* out, size_t max_len) const;
  void setWifiPass(const char* pass);

  /** Stored Spotify credentials JSON (after successful auth); empty if none. */
  bool getSpotifyCredentials(char* out, size_t max_len) const;
  void setSpotifyCredentials(const char* json);
  void clearSpotifyCredentials();

  bool save();
  void factoryReset();

 private:
  void loadDefaults();
  bool load();
  void markDirty(unsigned long now_ms);
  void ensurePresetsDir();

  Config config_{};
  bool initialized_{false};
  bool dirty_{false};
  unsigned long last_change_ms_{0};
  AudioSource audio_source_{AudioSource::Bluetooth};
  char snapcast_host_[SNAPCAST_HOST_MAX]{};
  uint16_t snapcast_port_{SNAPCAST_PORT_DEFAULT};
  char wifi_ssid_[WIFI_SSID_MAX]{};
  char wifi_pass_[WIFI_PASS_MAX]{};
  char spotify_cred_json_[SPOTIFY_CRED_JSON_MAX]{};
};

}  // namespace geelen_idf


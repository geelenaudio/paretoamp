// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// ESP-IDF ConfigManager (NVS + esp_littlefs).
//
// Post-refactor state:
//   - AudioSource: Bluetooth / Network / Dlna / TestTone
//   - Per-channel volume (vol_l / vol_r) + muted.
//   - StereoMode + channel enables.
//   - **Single-layer** parametric EQ, up to `geelen::EQ_MAX_BANDS` (=15)
//     stored bands. The runtime cap is board-specific
//     (`HAL_EQ_MAX_BANDS_ACTIVE` — 10 on Amped, 15 on Louder).
//   - Device name, DLNA enable, voice feedback, WiFi power-save.
//
// The old two-layer (calibration + preference) split was collapsed as part
// of the Phase-2 refactor; on-disk NVS blobs from pre-refactor firmware are
// migrated on first boot (see `load()` in the .cpp).
//
// Storage discipline: every mutator marks dirty; `tick()` debounces the
// actual NVS write by SAVE_DEBOUNCE_MS. API handlers that need a guaranteed
// persist can call `save()` directly (e.g. reset scopes, factory reset).

#pragma once

#include "eq_types.h"

#include <cstdint>
#include <cstddef>

namespace geelen_idf {

// Legacy EQ constants — retained only for one-shot migration of the old
// int8_t gain array into parametric bands. Phase 5 removes these entirely.
constexpr size_t EQ_BANDS = 10;
constexpr int8_t EQ_GAIN_MIN = -12;
constexpr int8_t EQ_GAIN_MAX = 12;

constexpr uint8_t VOLUME_MIN = 0;
constexpr uint8_t VOLUME_MAX = 100;
constexpr uint8_t VOLUME_DEFAULT = 80;

// Parametric-EQ storage capacity. Matches `geelen::EQ_MAX_BANDS`; this
// symbol is kept as a namespaced alias so callers in this translation unit
// don't need to reach into `geelen::` for every array-size expression.
constexpr size_t PARAMETRIC_BANDS = geelen::EQ_MAX_BANDS;
constexpr size_t PRESET_SLOT_COUNT = 10;
constexpr size_t PRESET_NAME_MAX = 32;

// Legacy fixed-preset count (pre-Phase-5 preset_id used 4 factory + 4 custom).
// Kept as a constant so code that still references it builds; factory-preset
// data moved into presets/ factory files in Phase 5.
constexpr size_t NUM_FIXED_PRESETS = 4;
constexpr size_t NUM_CUSTOM_SLOTS = 4;
constexpr size_t MAX_PRESET_NAME = 32;

constexpr unsigned long SAVE_DEBOUNCE_MS = 2000;
constexpr size_t LAST_BT_DEVICE_MAX = 18;
constexpr size_t SNAPCAST_HOST_MAX = 64;
constexpr size_t WIFI_SSID_MAX = 32;
constexpr size_t WIFI_PASS_MAX = 64;
constexpr size_t DEV_NAME_MAX = 32;
// DHCP / mDNS LDH label (null included). Empty stored value means "slug
// from device_name".
constexpr size_t HOSTNAME_MAX = 32;
constexpr size_t BT_PIN_MAX = 5;  // 4 digits + NUL
// Snapcast STREAM port. 1704 is the Snapcast audio stream; 1705 is the
// JSON-RPC control channel used by snapweb / CLI clients, and connecting a
// Snapclient there yields silence (server waits for Hello which never
// unlocks the stream). Pre-0.2.0 firmware defaulted to 1705 — that was a bug.
constexpr uint16_t SNAPCAST_PORT_DEFAULT = 1704;

enum class AudioSource : uint8_t {
  Bluetooth = 0,
  Network = 1,   // Snapclient
  Dlna = 2,      // Phase 9: native DLNA renderer
  TestTone = 3,  // Built-in sine / test signal (reboot to enter/exit)
};

enum class HeadroomMode : uint8_t { Auto = 0, Manual = 1 };
enum class StereoMode : uint8_t { Stereo = 0, Mono = 1 };

// Phase 5 — Wi-Fi power-save selector. Default `None` matches pre-Phase-5
// behaviour (best for streaming, highest power draw). Users that operate
// the device on battery / off-grid can flip this to MinModem to enable
// the modem-sleep policy at the cost of a few ms of extra latency on
// inbound packets — fine for the SPA but undesirable for Snapcast / DLNA.
enum class WifiPsMode : uint8_t { None = 0, MinModem = 1, MaxModem = 2 };

/** Wire format matches `geelen::EqBand` (16 bytes). */
using ParametricBand = geelen::EqBand;

struct Config {
  // Legacy scalars — kept as compatibility for older callers (Snap
  // ServerSettings handler, Phase 0 UI). vol_l / vol_r are the source of
  // truth; `volume` is updated in lockstep as max(vol_l, vol_r) so legacy
  // readers see a sensible scalar.
  uint8_t volume;
  uint8_t preset_id;                  // deprecated — zeroed after Phase 5 migration
  int8_t eq_gains[EQ_BANDS];          // deprecated — read only during migration

  // Phase 1 additions.
  uint8_t vol_l;
  uint8_t vol_r;
  bool muted;
  StereoMode stereo_mode;
  bool ch_l_on;
  bool ch_r_on;

  // WS2812 user colour. Brightness 0 = off. `led_user` is false until the
  // first POST /api/led so boot status animations can finish, then the LED
  // stays dark unless the user picked a colour.
  uint8_t led_r;
  uint8_t led_g;
  uint8_t led_b;
  uint8_t led_bri;
  bool led_user;

  // Single-layer PEQ. Storage is `PARAMETRIC_BANDS` (=15); the runtime cap
  // (`HAL_EQ_MAX_BANDS_ACTIVE`) is 10 on Amped and 15 on Louder. `eq_active_bands`
  // is the user-visible count in 1..HAL_EQ_MAX_BANDS_ACTIVE and is clamped on
  // set. Preset slot 0..9 or 0xFF ("modified / unsaved").
  ParametricBand eq_bands[PARAMETRIC_BANDS];
  uint8_t eq_active_bands;
  bool eq_on;
  uint8_t eq_active_slot;

  HeadroomMode eq_hr_mode;
  float eq_hr_manual_db;              // always <= 0.0f

  char dev_name[DEV_NAME_MAX];
  bool dlna_on;
  bool voice_feedback;  // spoken SPIFFS prompts (mode / Wi‑Fi / BT)
  WifiPsMode wifi_ps;   // Phase 5: Wi-Fi power-save selector
};

class ConfigManager {
 public:
  ConfigManager();
  bool begin();
  void tick(unsigned long now_ms);

  // ---- Volume -----------------------------------------------------------
  uint8_t getVolume() const { return config_.volume; }
  uint8_t getVolumeL() const { return config_.vol_l; }
  uint8_t getVolumeR() const { return config_.vol_r; }
  bool isMuted() const { return config_.muted; }
  // Legacy: sets both channels to `v`. Still called by the Snap ServerSettings
  // handler; new code should prefer setVolumeLR.
  void setVolume(uint8_t v);
  void setVolumeLR(uint8_t l, uint8_t r);
  void setMuted(bool muted);

  // ---- Channels ---------------------------------------------------------
  StereoMode getStereoMode() const { return config_.stereo_mode; }
  bool getChannelLOn() const { return config_.ch_l_on; }
  bool getChannelROn() const { return config_.ch_r_on; }
  void setStereoMode(StereoMode m);
  void setChannels(bool l_on, bool r_on);

  uint8_t getLedR() const { return config_.led_r; }
  uint8_t getLedG() const { return config_.led_g; }
  uint8_t getLedB() const { return config_.led_b; }
  uint8_t getLedBrightness() const { return config_.led_bri; }
  bool getLedUser() const { return config_.led_user; }
  void setLed(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);

  // ---- Preset (legacy) --------------------------------------------------
  uint8_t getPresetId() const { return config_.preset_id; }
  void setPresetId(uint8_t id);
  void getEQGains(int8_t out[EQ_BANDS]) const;
  void setEQGains(const int8_t gains[EQ_BANDS]);

  // ---- Parametric EQ (single layer) -------------------------------------
  // `bands` receives up to `PARAMETRIC_BANDS` (=15) band descriptors.
  // Only the first `getEqActiveBands()` are meaningful; the rest hold the
  // last-known/default values and are ignored by the DSP.
  void getEqBands(ParametricBand out[PARAMETRIC_BANDS]) const;
  void setEqBands(const ParametricBand bands[PARAMETRIC_BANDS]);
  uint8_t getEqActiveBands() const { return config_.eq_active_bands; }
  void setEqActiveBands(uint8_t n);
  bool getEqEnabled() const { return config_.eq_on; }
  void setEqEnabled(bool on);
  uint8_t getEqActiveSlot() const { return config_.eq_active_slot; }
  void setEqActiveSlot(uint8_t slot);

  HeadroomMode getHeadroomMode() const { return config_.eq_hr_mode; }
  float getHeadroomManualDb() const { return config_.eq_hr_manual_db; }
  void setHeadroom(HeadroomMode mode, float manual_db);

  // ---- Source / Snapcast / WiFi ----------------------------------------
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

  // Set after the first successful STA IP (or a successful validate/commit).
  // When the flag is present and false, `WifiManager::begin` skips STA and
  // goes straight to AP so unverified credentials cannot cycle the radio.
  bool getWifiEverConnectedOk() const { return wifi_ever_ok_; }
  bool wifiEverConnectedFlagPresent() const { return wifi_ever_flag_present_; }
  void setWifiEverConnectedOk(bool ok);

  // ---- Device name / hostname / MAC / DLNA ------------------------------
  bool getDeviceName(char* out, size_t max_len) const;
  void setDeviceName(const char* name);
  // Effective hostname (custom, or slug of device_name, or "geelen-audio").
  bool getHostname(char* out, size_t max_len) const;
  bool hostnameIsCustom() const { return hostname_[0] != '\0'; }
  // Empty / null clears the override (slug from device name). Returns false
  // if `hostname` is non-empty and not a valid LDH label.
  bool setHostname(const char* hostname);

  bool hasStaMacOverride() const { return sta_mac_set_; }
  bool hasBtMacOverride() const { return bt_mac_set_; }
  bool getStaMacOverride(uint8_t out[6]) const;
  bool getBtMacOverride(uint8_t out[6]) const;
  // Copies 6 bytes, forces locally-administered unicast. False if invalid.
  bool setStaMacOverride(const uint8_t mac[6]);
  bool setBtMacOverride(const uint8_t mac[6]);
  void clearStaMacOverride();
  void clearBtMacOverride();

  bool getDlnaEnabled() const { return config_.dlna_on; }
  void setDlnaEnabled(bool on);

  bool getVoiceFeedbackEnabled() const { return config_.voice_feedback; }
  void setVoiceFeedbackEnabled(bool on);

  // Classic PIN for legacy PIN_REQ (SSP phones often never ask). Four digits.
  bool getBtPin(char* out, size_t max_len) const;
  bool setBtPin(const char* pin);
  bool consumeBtWipeBonds();
  void requestBtWipeBonds();

  // ---- Wi-Fi power-save (Phase 5) ---------------------------------------
  // Persisted in NVS. The radio is reconfigured on next boot; we don't
  // hot-apply because flipping ps mid-stream interacts badly with active
  // BT coex.
  WifiPsMode getWifiPsMode() const { return config_.wifi_ps; }
  void setWifiPsMode(WifiPsMode mode);

  // ---- Test tone (NVS when in TestTone mode; live via /api/test-tone) ----
  float getTestToneFreqHz() const { return tt_freq_hz_; }
  float getTestToneAmpDb() const { return tt_amp_db_; }
  float getTestTonePan() const { return tt_pan_; }
  uint8_t getTestToneMode() const { return tt_mode_; }
  float getTestToneSweepS() const { return tt_sweep_s_; }
  float getTestToneSweepLoHz() const { return tt_sweep_lo_hz_; }
  float getTestToneSweepHiHz() const { return tt_sweep_hi_hz_; }
  float getTestToneDualF2Hz() const { return tt_dual_f2_hz_; }
  void setTestToneParams(float freq_hz, float amp_db, float pan, uint8_t mode, float sweep_s,
                         float sweep_lo_hz, float sweep_hi_hz, float dual_f2_hz);

  // ---- ETag / debounced NVS ---------------------------------------------
  uint32_t stateVersion() const { return state_version_; }
  void markDirtyEq();
  void markDirtyAudio();
  void markDirtyNet();
  void markDirtyTestTone();
  /** Marks all NVS domains dirty (factory reset, migration). */
  void markDirtyAll();

  // ---- Reset scopes (Phase 6) ------------------------------------------
  bool save();
  void factoryReset();
  // Each scope wipes the corresponding NVS keys and restores defaults for
  // those fields. No scope triggers a reboot — callers decide whether to
  // restart afterwards (WiFi/BT typically do; Snap/DLNA don't).
  void resetWifi();
  void resetBluetoothPairings();     // flags a Bluedroid bond wipe (applied in
                                     // A2dpSink::begin / before reboot)
  void resetSnapcast();
  void resetDlna();
  void resetEq();                    // both curves + all preset slots
  void resetAudio();                 // volume/channels/LED, leaves EQ

 private:
  void loadDefaults();
  bool load();
  void ensurePresetsDir();
  static void loadDefaultEqBands(ParametricBand out[PARAMETRIC_BANDS]);

  Config config_{};
  bool initialized_{false};
  bool dirty_{false};
  unsigned long last_change_ms_{0};
  uint8_t dirty_domains_{0};
  uint32_t state_version_{1};
  float tt_freq_hz_{440.f};
  float tt_amp_db_{-12.f};
  float tt_pan_{0.f};
  uint8_t tt_mode_{0};
  float tt_sweep_s_{10.f};
  float tt_sweep_lo_hz_{20.f};
  float tt_sweep_hi_hz_{20000.f};
  float tt_dual_f2_hz_{19000.f};
  AudioSource audio_source_{AudioSource::Bluetooth};
  char snapcast_host_[SNAPCAST_HOST_MAX]{};
  uint16_t snapcast_port_{SNAPCAST_PORT_DEFAULT};
  char wifi_ssid_[WIFI_SSID_MAX]{};
  char wifi_pass_[WIFI_PASS_MAX]{};
  bool wifi_ever_ok_{false};
  bool wifi_ever_flag_present_{false};
  char hostname_[HOSTNAME_MAX]{};
  char bt_pin_[BT_PIN_MAX]{};
  bool bt_wipe_bonds_{false};
  uint8_t sta_mac_[6]{};
  uint8_t bt_mac_[6]{};
  bool sta_mac_set_{false};
  bool bt_mac_set_{false};
};

/** Background NVS writer; call once after ConfigManager::begin(). */
void configStartDebouncedSaveTask(ConfigManager* cfg);

}  // namespace geelen_idf

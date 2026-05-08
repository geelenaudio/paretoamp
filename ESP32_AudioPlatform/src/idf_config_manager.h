// ESP-IDF ConfigManager (NVS + esp_littlefs).
//
// Phase 1 expanded this significantly:
//   - AudioSource gained `Dlna` (widened for Phase 3).
//   - Per-channel volume (vol_l / vol_r) + muted replace the scalar `vol`.
//   - ChannelConfig + stereo/mono mode.
//   - Dual 10-band parametric EQ (calibration + preference) with enables,
//     preset-slot indicator, auto-headroom + manual headroom cap.
//   - Device name (user-friendly, shown in snapweb / DLNA / BT).
//   - DLNA renderer enabled flag.
//
// Storage discipline: every mutator marks dirty; `tick()` debounces the actual
// NVS write by SAVE_DEBOUNCE_MS. API handlers that need a guaranteed persist
// can call `save()` directly (e.g. reset scopes, factory reset).

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

// Phase 5 preset model.
constexpr size_t PARAMETRIC_BANDS = 10;
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

  ParametricBand eq_cal[PARAMETRIC_BANDS];
  uint8_t eq_cal_active_bands;
  bool eq_cal_on;
  ParametricBand eq_pref[PARAMETRIC_BANDS];
  uint8_t eq_pref_active_bands;
  bool eq_pref_on;
  uint8_t eq_pref_slot;               // 0..9 or 0xFF ("unsaved / modified")

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

  // ---- Preset (legacy) --------------------------------------------------
  uint8_t getPresetId() const { return config_.preset_id; }
  void setPresetId(uint8_t id);
  void getEQGains(int8_t out[EQ_BANDS]) const;
  void setEQGains(const int8_t gains[EQ_BANDS]);

  // ---- Parametric EQ ----------------------------------------------------
  // `curve[out]` receives a copy of the 10-band curve.
  void getEqCalibration(ParametricBand out[PARAMETRIC_BANDS]) const;
  void setEqCalibration(const ParametricBand bands[PARAMETRIC_BANDS]);
  uint8_t getEqCalibrationActiveBands() const { return config_.eq_cal_active_bands; }
  void setEqCalibrationActiveBands(uint8_t n);
  bool getEqCalibrationEnabled() const { return config_.eq_cal_on; }
  void setEqCalibrationEnabled(bool on);

  void getEqPreference(ParametricBand out[PARAMETRIC_BANDS]) const;
  void setEqPreference(const ParametricBand bands[PARAMETRIC_BANDS]);
  uint8_t getEqPreferenceActiveBands() const { return config_.eq_pref_active_bands; }
  void setEqPreferenceActiveBands(uint8_t n);
  bool getEqPreferenceEnabled() const { return config_.eq_pref_on; }
  void setEqPreferenceEnabled(bool on);
  uint8_t getEqPreferenceSlot() const { return config_.eq_pref_slot; }
  void setEqPreferenceSlot(uint8_t slot);

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

  // ---- Device name / DLNA -----------------------------------------------
  bool getDeviceName(char* out, size_t max_len) const;
  void setDeviceName(const char* name);
  bool getDlnaEnabled() const { return config_.dlna_on; }
  void setDlnaEnabled(bool on);

  bool getVoiceFeedbackEnabled() const { return config_.voice_feedback; }
  void setVoiceFeedbackEnabled(bool on);

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
  void resetBluetoothPairings();     // only erases our `last_bt` hint; actual
                                     // BT bond storage is in the BT stack's
                                     // own NVS namespace and needs a reboot
                                     // after an `esp_bt_gap_remove_bond_*`
                                     // sweep to fully take effect.
  void resetSnapcast();
  void resetDlna();
  void resetEq();                    // both curves + all preset slots
  void resetAudio();                 // volume/channels only, leaves EQ

 private:
  void loadDefaults();
  bool load();
  void ensurePresetsDir();
  static void loadDefaultEqCalibration(ParametricBand out[PARAMETRIC_BANDS]);
  static void loadDefaultEqPreference(ParametricBand out[PARAMETRIC_BANDS]);

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
};

/** Background NVS writer; call once after ConfigManager::begin(). */
void configStartDebouncedSaveTask(ConfigManager* cfg);

}  // namespace geelen_idf

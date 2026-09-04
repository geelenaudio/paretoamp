// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// ESP-IDF Bluetooth A2DP sink feeding PCM into AudioEngine.
//
// Only defined when Bluetooth Classic is enabled in sdkconfig. On S3 (BLE-only)
// this header is empty; every caller wraps A2dpSink usage in
// `#if CONFIG_BT_ENABLED`. Re-implementing A2DP on top of BLE Audio profiles
// is tracked in docs/roadmap.md (Board / HAL section).
//
// PCM from the Bluedroid data callback is pushed into a StreamBuffer; a
// dedicated `a2dp_pump` task on Core 1 (prio 12, above httpd at 10)
// resamples and calls `feedPCM`. That keeps linear-interp CPU off the
// Bluedroid/Core-0 path and prevents SPA/httpd work from starving PCM.
// AVRCP absolute-volume is only stashed on the BTC thread; the pump
// applies engine gain after PCM and debounces NVS/SSE so volume rocker
// spam cannot preempt the stream.
//
// Discoverability is a 180 s pairing window (begin, A2DP disconnect,
// encoder long-press via a2dpForcePairingWindow, or
// POST /api/bluetooth {discoverable:true}). While connected the sink is
// connectable but not inquiry-visible (ACL=1). Quiet mode after the window
// still lets the last bonded phone reconnect. Long-press disconnects the
// ACL first if needed; it does not wipe NVS bonds.

#pragma once

#include "sdkconfig.h"

#include <cstdint>

namespace geelen_idf {

class ConfigManager;

/** True while A2DP audio is in the STARTED state. Always false when BT is off. */
#if CONFIG_BT_ENABLED
bool a2dpPlaybackActive();
bool a2dpIsUp();
bool a2dpIsConnected();
bool a2dpIsDiscoverable();
uint32_t a2dpPairingWindowMsLeft();
bool a2dpStartPairingWindow();
/** Disconnect the ACL if needed, then open the 180 s pairing window. Does not wipe bonds. */
bool a2dpForcePairingWindow();
bool a2dpApplyPin(const char* pin);
void a2dpWipeBonds();
void a2dpNotifyLocalVolume();
void a2dpSetConfig(ConfigManager* cfg);
void a2dpSetAdvertisedName(const char* name);
/** Latest phone volume 0–100, or -1 if none pending (use ConfigManager). */
int a2dpLiveVolumePct();
#else
inline bool a2dpPlaybackActive() { return false; }
inline bool a2dpIsUp() { return false; }
inline bool a2dpIsConnected() { return false; }
inline bool a2dpIsDiscoverable() { return false; }
inline uint32_t a2dpPairingWindowMsLeft() { return 0; }
inline bool a2dpStartPairingWindow() { return false; }
inline bool a2dpForcePairingWindow() { return false; }
inline bool a2dpApplyPin(const char*) { return false; }
inline void a2dpWipeBonds() {}
inline void a2dpNotifyLocalVolume() {}
inline void a2dpSetConfig(ConfigManager*) {}
inline void a2dpSetAdvertisedName(const char*) {}
inline int a2dpLiveVolumePct() { return -1; }
#endif

}  // namespace geelen_idf

#if CONFIG_BT_ENABLED

#include "esp_a2dp_api.h"

namespace geelen {
class AudioEngine;
}

namespace geelen_idf {

class A2dpSink {
 public:
  A2dpSink() = default;
  void setEngine(geelen::AudioEngine* eng) { engine_ = eng; }
  bool begin(const char* device_name);
  /** Update GAP advertised name if the sink is already up. */
  void setAdvertisedName(const char* device_name);

 private:
  static void a2dpCb_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param);
  static void a2dpDataCb_(const uint8_t* data, uint32_t len);
  static void pumpTask_(void* arg);
  static void resampleAndFeed_(const uint8_t* data, uint32_t len);

  static geelen::AudioEngine* engine_;
};

}  // namespace geelen_idf

#endif  // CONFIG_BT_ENABLED

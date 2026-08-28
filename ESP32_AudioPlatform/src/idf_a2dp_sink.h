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

#pragma once

#include "sdkconfig.h"

namespace geelen_idf {

/** True while A2DP audio is in the STARTED state. Always false when BT is off. */
#if CONFIG_BT_ENABLED
bool a2dpPlaybackActive();
#else
inline bool a2dpPlaybackActive() { return false; }
#endif

}  // namespace geelen_idf

#if CONFIG_BT_ENABLED

#include <cstdint>
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

#if CONFIG_BT_ENABLED
namespace geelen_idf {
void a2dpSetAdvertisedName(const char* name);
}
#else
namespace geelen_idf {
inline void a2dpSetAdvertisedName(const char*) {}
}
#endif

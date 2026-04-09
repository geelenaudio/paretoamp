// ESP-IDF Bluetooth A2DP sink feeding PCM into AudioEngine.

#pragma once

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

 private:
  static void a2dpCb_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param);
  static void a2dpDataCb_(const uint8_t* data, uint32_t len);

  static geelen::AudioEngine* engine_;
};

}  // namespace geelen_idf


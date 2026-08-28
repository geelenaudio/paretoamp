// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors

#include "idf_led.h"

#include "hal/led.h"
#include "idf_a2dp_sink.h"
#include "idf_config_manager.h"
#include "idf_events.h"
#include "idf_state_stream.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include <atomic>
#include <cmath>

namespace geelen_idf {

namespace {

constexpr const char* TAG = "led";
constexpr uint32_t kPulsePeriodTicks = 50;  // 50 × 40 ms ≈ 2.0 s
constexpr TickType_t kPulseTick = pdMS_TO_TICKS(40);

enum class Phase : uint8_t { Boot = 0, Wifi = 1, Pairing = 2, User = 3 };

std::atomic<Phase> s_phase{Phase::Boot};
std::atomic<bool> s_wifi_up{false};
std::atomic<bool> s_a2dp_connected{false};
std::atomic<bool> s_user_takeover{false};
ConfigManager* s_cfg = nullptr;

// 0 at t=0, 255 at half period, 0 at period. HAL then scales by max duty.
uint8_t pulse_u8(uint32_t t) {
  const float ph = (t % kPulsePeriodTicks) / (float)kPulsePeriodTicks;
  const float s = 0.5f - 0.5f * cosf(ph * 6.2831853f);
  return (uint8_t)(s * 255.f);
}

void apply_user_from_cfg() {
  if (!s_cfg || !s_cfg->getLedUser()) {
    geelen_hal::ledClear();
    return;
  }
  const uint8_t bri = s_cfg->getLedBrightness();
  geelen_hal::ledSetRgb((uint8_t)((s_cfg->getLedR() * bri) / 100),
                        (uint8_t)((s_cfg->getLedG() * bri) / 100),
                        (uint8_t)((s_cfg->getLedB() * bri) / 100));
}

bool want_bt_pairing() {
  if (!s_cfg) return false;
  if (s_cfg->getAudioSource() != AudioSource::Bluetooth) return false;
  return a2dpIsDiscoverable();
}

void set_phase(Phase p) {
  if (s_phase.load(std::memory_order_relaxed) == p) return;
  s_phase.store(p, std::memory_order_relaxed);
  stateStreamMarkDirty();
}

void enter_user() {
  set_phase(Phase::User);
  apply_user_from_cfg();
}

void on_wifi_up(void*, esp_event_base_t, int32_t, void*) {
  s_wifi_up.store(true, std::memory_order_relaxed);
}

void on_a2dp_connected(void*, esp_event_base_t, int32_t, void*) {
  s_a2dp_connected.store(true, std::memory_order_relaxed);
}

void on_a2dp_disconnected(void*, esp_event_base_t, int32_t, void*) {
  s_a2dp_connected.store(false, std::memory_order_relaxed);
}

void led_task(void*) {
  uint32_t pulse_t = 0;
  bool green_done = false;
  for (;;) {
    if (s_user_takeover.load(std::memory_order_relaxed)) {
      enter_user();
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    const Phase p = s_phase.load(std::memory_order_relaxed);
    switch (p) {
      case Phase::Boot: {
        const uint8_t v = pulse_u8(pulse_t++);
        geelen_hal::ledSetRgb(v, 0, 0);
        vTaskDelay(kPulseTick);
        if (s_wifi_up.load(std::memory_order_relaxed)) {
          pulse_t = 0;
          set_phase(Phase::Wifi);
        }
        break;
      }
      case Phase::Wifi:
        if (!green_done) {
          const uint8_t v = pulse_u8(pulse_t++);
          geelen_hal::ledSetRgb(0, v, 0);
          vTaskDelay(kPulseTick);
          if (pulse_t >= kPulsePeriodTicks) {
            green_done = true;
            geelen_hal::ledClear();
          }
          break;
        }
        if (want_bt_pairing()) {
          pulse_t = 0;
          set_phase(Phase::Pairing);
        } else {
          enter_user();
        }
        break;
      case Phase::Pairing:
        if (s_a2dp_connected.load(std::memory_order_relaxed) || !want_bt_pairing()) {
          enter_user();
          break;
        }
        {
          const uint8_t v = pulse_u8(pulse_t++);
          geelen_hal::ledSetRgb(0, 0, v);
          vTaskDelay(kPulseTick);
        }
        break;
      case Phase::User:
        if (!s_user_takeover.load(std::memory_order_relaxed) && want_bt_pairing()) {
          pulse_t = 0;
          set_phase(Phase::Pairing);
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
        break;
    }
  }
}

}  // namespace

void ledBegin(ConfigManager* cfg) {
  s_cfg = cfg;
  if (!geelen_hal::ledPresent()) {
    ESP_LOGI(TAG, "no WS2812 on this build");
    return;
  }
  if (!geelen_hal::ledInit()) return;

  eventRegister(EV_WIFI_UP, &on_wifi_up, nullptr);
  eventRegister(EV_A2DP_CONNECTED, &on_a2dp_connected, nullptr);
  eventRegister(EV_A2DP_DISCONNECTED, &on_a2dp_disconnected, nullptr);

  xTaskCreatePinnedToCoreWithCaps(&led_task, "led", 3072, nullptr, 3, nullptr,
                                  0, MALLOC_CAP_SPIRAM);
  ESP_LOGI(TAG, "animator up (core 0, prio 3, PSRAM stack, count=%u)",
           (unsigned)geelen_hal::ledPixelCount());
}

const char* ledPhaseName() {
  switch (s_phase.load(std::memory_order_relaxed)) {
    case Phase::Boot:     return "boot";
    case Phase::Wifi:     return "wifi";
    case Phase::Pairing:  return "pairing";
    case Phase::User:     return "user";
  }
  return "boot";
}

void ledTakeoverUser() {
  s_user_takeover.store(true, std::memory_order_relaxed);
  enter_user();
}

}  // namespace geelen_idf

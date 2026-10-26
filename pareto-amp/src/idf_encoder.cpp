// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// Polls a Bourns PEC11R (or compatible) on Core 0. One detent is ±1% ganged
// volume. Switch release under 1 s toggles mute; hold ≥ 1 s opens pairing
// (disconnect ACL if needed) and ignores the later release.

#include "idf_encoder.h"

#include "hal/hal_board.h"

#if HAL_HAS_ENCODER

#include "idf_a2dp_sink.h"
#include "idf_config_manager.h"
#include "idf_led.h"

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include <cstdint>
#include <atomic>

extern "C" void on_volume_changed();
extern "C" void on_encoder_volume_live(uint8_t pct, bool muted);

namespace geelen_idf {

namespace {

constexpr const char* TAG = "encoder";
constexpr int64_t kPersistUs = 250000;
constexpr int64_t kPairHoldUs = 1000000;
// CONFIG_FREERTOS_HZ=100, so pdMS_TO_TICKS(2)==0. vTaskDelay(0) never
// runs IDLE0 and trips the task WDT. Wake from a 2 ms esp_timer instead.
constexpr uint64_t kPollUs = 2000;

// 4 Gray-code edges per mechanical detent (PEC11R 24 PPR).
constexpr int8_t kQuadTable[16] = {
    0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

ConfigManager* s_cfg = nullptr;
TaskHandle_t s_task = nullptr;
esp_timer_handle_t s_poll_timer = nullptr;
std::atomic<bool> s_have_timer{false};

void poll_timer_cb(void*) {
  if (s_task) xTaskNotifyGive(s_task);
}

uint8_t read_ab() {
  const uint8_t a = gpio_get_level(HAL_PIN_ENC_A) ? 1 : 0;
  const uint8_t b = gpio_get_level(HAL_PIN_ENC_B) ? 1 : 0;
  return (uint8_t)((a << 1) | b);
}

bool switch_down() { return gpio_get_level(HAL_PIN_ENC_SW) == 0; }

uint8_t ganged_pct_from_cfg() {
  const uint8_t l = s_cfg->getVolumeL();
  const uint8_t r = s_cfg->getVolumeR();
  return l > r ? l : r;
}

void persist_now(uint8_t pct, bool muted) {
  if (!s_cfg) return;
  s_cfg->setVolume(pct);
  s_cfg->setMuted(muted);
  on_volume_changed();
}

void encoder_task(void*) {
  uint8_t live_pct = s_cfg ? ganged_pct_from_cfg() : 0;
  bool live_muted = s_cfg && s_cfg->isMuted();
  uint8_t prev_ab = read_ab();
  int8_t accum = 0;
  bool sw_was_down = switch_down();
  int64_t press_start_us = 0;
  bool pairing_fired = sw_was_down;
  bool dirty = false;
  int64_t persist_at_us = 0;

  for (;;) {
    const int64_t now = esp_timer_get_time();

    if (!dirty && s_cfg) {
      live_pct = ganged_pct_from_cfg();
      live_muted = s_cfg->isMuted();
    }

    const uint8_t ab = read_ab();
    if (ab != prev_ab) {
      const int8_t step = kQuadTable[(prev_ab << 2) | ab];
      prev_ab = ab;
      accum = (int8_t)(accum + step);
      int8_t detents = 0;
      while (accum >= 4) {
        accum = (int8_t)(accum - 4);
        detents++;
      }
      while (accum <= -4) {
        accum = (int8_t)(accum + 4);
        detents--;
      }
      if (detents != 0) {
        int v = (int)live_pct + (int)detents;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        live_pct = (uint8_t)v;
        on_encoder_volume_live(live_pct, live_muted);
        dirty = true;
        persist_at_us = now + kPersistUs;
      }
    }

    const bool sw_down = switch_down();
    if (sw_down && !sw_was_down) {
      press_start_us = now;
      pairing_fired = false;
    }
    if (sw_down && !pairing_fired && (now - press_start_us) >= kPairHoldUs) {
      pairing_fired = true;
      if (a2dpForcePairingWindow()) {
        ESP_LOGI(TAG, "long-press pairing window");
      } else {
        ESP_LOGW(TAG, "long-press pairing failed");
      }
    }
    if (!sw_down && sw_was_down && !pairing_fired) {
      live_muted = !live_muted;
      on_encoder_volume_live(live_pct, live_muted);
      persist_now(live_pct, live_muted);
      dirty = false;
    }
    sw_was_down = sw_down;

    if (dirty && now >= persist_at_us) {
      persist_now(live_pct, live_muted);
      dirty = false;
    }

    if (s_have_timer.load(std::memory_order_relaxed)) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    } else {
      vTaskDelay(1);
    }
  }
}

}  // namespace

void encoderBegin(ConfigManager* cfg) {
  s_cfg = cfg;
  gpio_reset_pin(HAL_PIN_ENC_A);
  gpio_reset_pin(HAL_PIN_ENC_B);
  gpio_reset_pin(HAL_PIN_ENC_SW);

  gpio_config_t io = {};
  io.pin_bit_mask = (1ULL << (unsigned)HAL_PIN_ENC_A) |
                    (1ULL << (unsigned)HAL_PIN_ENC_B) |
                    (1ULL << (unsigned)HAL_PIN_ENC_SW);
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  esp_err_t err = gpio_config(&io);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
    return;
  }

  BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
      &encoder_task, "encoder", 3072, nullptr, 4, &s_task, 0, MALLOC_CAP_SPIRAM);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "task create failed");
    s_task = nullptr;
    return;
  }

  esp_timer_create_args_t args{};
  args.callback = &poll_timer_cb;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "enc_poll";
  if (esp_timer_create(&args, &s_poll_timer) == ESP_OK &&
      esp_timer_start_periodic(s_poll_timer, kPollUs) == ESP_OK) {
    s_have_timer.store(true, std::memory_order_relaxed);
  } else {
    ESP_LOGW(TAG, "poll timer failed; 10 ms vTaskDelay fallback");
    s_have_timer.store(false, std::memory_order_relaxed);
  }

  ESP_LOGI(TAG, "up (A=%d B=%d SW=%d, core 0, prio 4, poll=%s)",
           (int)HAL_PIN_ENC_A, (int)HAL_PIN_ENC_B, (int)HAL_PIN_ENC_SW,
           s_have_timer.load(std::memory_order_relaxed) ? "2ms timer" : "1 tick");
}

}  // namespace geelen_idf

#else  // !HAL_HAS_ENCODER

namespace geelen_idf {

void encoderBegin(ConfigManager*) {}

}  // namespace geelen_idf

#endif

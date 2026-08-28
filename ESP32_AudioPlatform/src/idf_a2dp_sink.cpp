// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// ESP-IDF Bluetooth A2DP sink implementation. Whole file collapses to empty
// when BT Classic is disabled (S3 targets); see idf_a2dp_sink.h.

#include "sdkconfig.h"

#if CONFIG_BT_ENABLED

#include "idf_a2dp_sink.h"

#include "audio_engine.h"
#include "idf_events.h"
#include "idf_voice_feedback.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"

#include <atomic>
#include <vector>
#include <cstring>

namespace geelen_idf {

static const char* TAG = "A2DP";
geelen::AudioEngine* A2dpSink::engine_ = nullptr;
static uint32_t s_bt_pcm_hz = 44100;
static uint32_t s_resample_step_q16 = 0;  // in_rate/out_rate in Q16.16
static bool s_have_carry_frame = false;
static int16_t s_carry_frame[2] = {0, 0};  // stereo
static uint32_t s_pos_q16 = 0;
static uint32_t s_step_adj_q16 = 0;  // multiplicative adjustment (Q16.16), around 1.0
static StreamBufferHandle_t s_pcm_sb = nullptr;
static std::atomic<bool> s_a2dp_playing{false};
static bool s_a2dp_up = false;

bool a2dpPlaybackActive() {
  return s_a2dp_playing.load(std::memory_order_relaxed);
}

static uint32_t sbcSampleRateFromMask(uint8_t mask) {
  // Prefer higher rates if multiple bits are set.
  if (mask & ESP_A2D_SBC_CIE_SF_48K) return 48000;
  if (mask & ESP_A2D_SBC_CIE_SF_44K) return 44100;
  if (mask & ESP_A2D_SBC_CIE_SF_32K) return 32000;
  if (mask & ESP_A2D_SBC_CIE_SF_16K) return 16000;
  return 44100;
}

static void btAppGapCb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
  (void)param;
  if (event == ESP_BT_GAP_AUTH_CMPL_EVT) {
    ESP_LOGI(TAG, "GAP auth complete");
  } else if (event == ESP_BT_GAP_PIN_REQ_EVT) {
    // Post-only: the supervisor handles pairing-clip enqueue + debounce so
    // BT host-task callbacks stay decoupled from the voice serializer.
    eventPost(EV_A2DP_PAIRING, nullptr, 0);
  }
}

bool A2dpSink::begin(const char* device_name) {
  if (!device_name) device_name = "Geelen Audio";

  // Bluedroid's host-side init (gatt_init / btu_init) allocates ~210 B
  // queues + several KB of working memory from the BT-OSI heap, which is
  // backed by the internal-DRAM `MALLOC_CAP_INTERNAL` pool. On this build
  // (Wi-Fi up, A2DP, no BLE released) that pool is genuinely tight — the
  // observed failure mode when it dips too low is a NULL `fixed_queue`
  // returned silently by `osi_malloc` and a panic at the first enqueue
  // (`assert fixed_queue_enqueue (queue != NULL)`). Log free + largest
  // contiguous block here so future regressions are diagnosable from
  // the boot UART log without rebuilding for heap_caps tracing.
  const size_t free_int =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t lblk_int =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
  ESP_LOGI(TAG, "Bluedroid pre-init heap: int8=%u B free, %u B largest; dma=%u B free",
           (unsigned)free_int, (unsigned)lblk_int, (unsigned)free_dma);

  esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
  if (err != ESP_OK) {
    // Not fatal if already in use; log only.
    ESP_LOGW(TAG, "bt_controller_mem_release(BLE): %s", esp_err_to_name(err));
  }

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
  ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

  // Phase 5 NOTE: an earlier iteration called
  // `esp_coex_preference_set(ESP_COEX_PREFER_BT)` between
  // `esp_bt_controller_enable` and `esp_bluedroid_init`. On IDF 5.5 with
  // Classic-BT-only (BLE memory released above), that runtime call left
  // Bluedroid in a state where `gatt_init`'s host-side queue setup hit
  // a NULL `fixed_queue` and the device panicked on boot. The runtime
  // function is also deprecated on recent IDFs in favour of sdkconfig
  // tuning. Coex bias should now be configured via:
  //   CONFIG_ESP_COEX_SW_COEXIST_PREFER_BT (or _BALANCE / _WIFI)
  // in sdkconfig.amped-esp32-idf if we want to bias the arbiter.

  ESP_ERROR_CHECK(esp_bluedroid_init());
  ESP_ERROR_CHECK(esp_bluedroid_enable());

  ESP_ERROR_CHECK(esp_bt_gap_register_callback(btAppGapCb));
  ESP_ERROR_CHECK(esp_bt_gap_set_device_name(device_name));

  // Allow legacy PIN pairing if the phone requests it.
  // (Many modern phones use SSP and won't need this, but it doesn't hurt.)
  esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
  esp_bt_pin_code_t pin_code;
  pin_code[0] = '1';
  pin_code[1] = '2';
  pin_code[2] = '3';
  pin_code[3] = '4';
  ESP_ERROR_CHECK(esp_bt_gap_set_pin(pin_type, 4, pin_code));

  // Set device discoverable/connectable
  ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE));

  ESP_ERROR_CHECK(esp_a2d_register_callback(&A2dpSink::a2dpCb_));
  ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(&A2dpSink::a2dpDataCb_));
  ESP_ERROR_CHECK(esp_a2d_sink_init());

  // Both the PCM bounce buffer and the pump task stack live in PSRAM.
  // Putting them in internal DRAM (the IDF default) left ~3.7 KiB free
  // with a 736-byte largest block after Bluedroid init — too small for
  // lwip pbufs, so the SPA at :80 hung after boot. The data callback is
  // not an ISR, so PSRAM copies are safe.
  if (!s_pcm_sb) {
    s_pcm_sb = xStreamBufferCreateWithCaps(8192, 4, MALLOC_CAP_SPIRAM);
    if (!s_pcm_sb) {
      ESP_LOGE(TAG, "a2dp pcm StreamBuffer create failed");
      return false;
    }
  }
  if (xTaskCreatePinnedToCoreWithCaps(&A2dpSink::pumpTask_, "a2dp_pump", 4096,
                                      nullptr, 12, nullptr, 1,
                                      MALLOC_CAP_SPIRAM) != pdPASS) {
    ESP_LOGE(TAG, "a2dp_pump task create failed");
    return false;
  }

  const size_t free_after =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t lblk_after =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  ESP_LOGI(TAG, "A2DP sink started as '%s' (post-init int8=%u B free, %u B largest)",
           device_name, (unsigned)free_after, (unsigned)lblk_after);
  s_a2dp_up = true;
  return true;
}

void A2dpSink::setAdvertisedName(const char* device_name) {
  a2dpSetAdvertisedName(device_name);
}

void a2dpSetAdvertisedName(const char* name) {
  if (!s_a2dp_up || !name || !name[0]) return;
  esp_err_t err = esp_bt_gap_set_device_name(name);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_bt_gap_set_device_name(%s) failed: %s", name,
             esp_err_to_name(err));
    return;
  }
  ESP_LOGI(TAG, "GAP name %s", name);
}

void A2dpSink::a2dpCb_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param) {
  switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
      if (param) {
        ESP_LOGI(TAG, "Conn state: %d", (int)param->conn_stat.state);
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
          eventPost(EV_A2DP_CONNECTED, nullptr, 0);
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
          eventPost(EV_A2DP_DISCONNECTED, nullptr, 0);
        }
      }
      break;
    case ESP_A2D_AUDIO_STATE_EVT:
      if (param) {
        ESP_LOGI(TAG, "Audio state: %d", (int)param->audio_stat.state);
        s_a2dp_playing.store(param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED,
                             std::memory_order_relaxed);
      }
      break;
    case ESP_A2D_AUDIO_CFG_EVT:
      if (param) {
        if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
          const uint32_t hz = sbcSampleRateFromMask(param->audio_cfg.mcc.cie.sbc_info.samp_freq);
          s_bt_pcm_hz = hz;
          // Prepare resampler for output at the engine's fixed 44.1kHz.
          s_resample_step_q16 = (uint32_t)(((uint64_t)s_bt_pcm_hz << 16) / (uint64_t)geelen::SAMPLE_RATE);
          s_pos_q16 = 0;
          s_have_carry_frame = false;
          ESP_LOGI(TAG, "SBC PCM rate: %u Hz (resample -> %u Hz)", (unsigned)s_bt_pcm_hz,
                   (unsigned)geelen::SAMPLE_RATE);
        } else {
          ESP_LOGI(TAG, "Audio cfg evt (codec=%u)", (unsigned)param->audio_cfg.mcc.type);
        }
      } else {
        ESP_LOGI(TAG, "Audio cfg evt");
      }
      break;
    default:
      break;
  }
}

void A2dpSink::a2dpDataCb_(const uint8_t* data, uint32_t len) {
  if (!data || len == 0) return;
  if (geelen_idf::voiceFeedbackActive()) return;
  if (!s_pcm_sb) return;
  size_t sent = xStreamBufferSend(s_pcm_sb, data, len, 0);
  if (sent < len) {
    static uint64_t s_last_drop_log = 0;
    uint64_t now = (uint64_t)xTaskGetTickCount();
    if (now - s_last_drop_log > pdMS_TO_TICKS(1000)) {
      s_last_drop_log = now;
      ESP_LOGW(TAG, "a2dp_pump StreamBuffer full, dropped %u bytes", (unsigned)(len - sent));
    }
  }
}

void A2dpSink::pumpTask_(void* /*arg*/) {
  uint8_t buf[1024];
  ESP_LOGI(TAG, "a2dp_pump up (core 1, prio 12)");
  for (;;) {
    size_t n = xStreamBufferReceive(s_pcm_sb, buf, sizeof(buf), portMAX_DELAY);
    if (n > 0) resampleAndFeed_(buf, (uint32_t)n);
  }
}

void A2dpSink::resampleAndFeed_(const uint8_t* data, uint32_t len) {
  if (!engine_ || !data || len == 0) return;
  if (geelen_idf::voiceFeedbackActive()) return;

  const int16_t* in = reinterpret_cast<const int16_t*>(data);
  const size_t in_samples = len / sizeof(int16_t);
  if (in_samples < 2) return;
  const size_t in_frames = in_samples / 2;

  const size_t fill = engine_->bufferedFrames();
  const size_t cap = geelen::RING_BUF_SAMPLES;
  const size_t target = geelen::SAMPLE_RATE;
  if (cap > 0) {
    int32_t err = (int32_t)fill - (int32_t)target;
    int32_t ppm = (err * 20000) / (int32_t)cap;
    if (ppm > 20000) ppm = 20000;
    if (ppm < -20000) ppm = -20000;
    s_step_adj_q16 = (uint32_t)((1 << 16) + (int32_t)((int64_t)ppm * (1 << 16) / 1000000LL));
  } else {
    s_step_adj_q16 = (1 << 16);
  }

  static std::vector<int16_t> window;
  window.resize((in_frames + (s_have_carry_frame ? 1 : 0)) * 2);
  size_t w_frames = 0;
  if (s_have_carry_frame) {
    window[0] = s_carry_frame[0];
    window[1] = s_carry_frame[1];
    w_frames = 1;
  }
  memcpy(window.data() + w_frames * 2, in, in_frames * 2 * sizeof(int16_t));
  w_frames += in_frames;

  if (w_frames < 2) return;

  const uint32_t base_step = (uint32_t)(((uint64_t)s_bt_pcm_hz << 16) / (uint64_t)geelen::SAMPLE_RATE);
  const uint32_t step = (uint32_t)(((uint64_t)base_step * (uint64_t)s_step_adj_q16) >> 16);
  const uint32_t end_q16 = (uint32_t)((w_frames - 1) << 16);

  static std::vector<int16_t> out;
  out.clear();
  out.reserve((size_t)((uint64_t)w_frames * geelen::SAMPLE_RATE / s_bt_pcm_hz + 4) * 2);

  while (s_pos_q16 < end_q16) {
    const uint32_t idx = s_pos_q16 >> 16;
    const uint32_t frac = s_pos_q16 & 0xFFFF;
    const uint32_t inv = 0x10000 - frac;

    const int16_t* f0 = &window[idx * 2];
    const int16_t* f1 = &window[(idx + 1) * 2];

    for (int ch = 0; ch < 2; ch++) {
      int32_t s0 = f0[ch];
      int32_t s1 = f1[ch];
      int32_t v = (int32_t)((s0 * (int32_t)inv + s1 * (int32_t)frac) >> 16);
      out.push_back((int16_t)v);
    }

    s_pos_q16 += step;
  }

  s_carry_frame[0] = window[(w_frames - 1) * 2];
  s_carry_frame[1] = window[(w_frames - 1) * 2 + 1];
  s_have_carry_frame = true;
  s_pos_q16 -= end_q16;

  if (!out.empty()) {
    engine_->feedPCM(reinterpret_cast<const uint8_t*>(out.data()), out.size() * sizeof(int16_t));
  }
}

}  // namespace geelen_idf

#endif  // CONFIG_BT_ENABLED

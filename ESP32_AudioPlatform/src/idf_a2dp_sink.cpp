// ESP-IDF Bluetooth A2DP sink implementation.

#include "idf_a2dp_sink.h"

#include "audio_engine.h"
#include "idf_mp3_streamer.h"
#include "idf_voice_feedback.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"

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
    voiceFeedbackOnBtPairing();
  }
}

bool A2dpSink::begin(const char* device_name) {
  if (!device_name) device_name = "Geelen Audio";

  esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
  if (err != ESP_OK) {
    // Not fatal if already in use; log only.
    ESP_LOGW(TAG, "bt_controller_mem_release(BLE): %s", esp_err_to_name(err));
  }

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
  ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

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

  ESP_LOGI(TAG, "A2DP sink started as '%s'", device_name);
  return true;
}

void A2dpSink::a2dpCb_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param) {
  switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
      if (param) {
        ESP_LOGI(TAG, "Conn state: %d", (int)param->conn_stat.state);
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
          voiceFeedbackOnA2dpConnected();
        }
      }
      break;
    case ESP_A2D_AUDIO_STATE_EVT:
      if (param) {
        ESP_LOGI(TAG, "Audio state: %d", (int)param->audio_stat.state);
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
  if (!engine_ || !data || len == 0) return;
  // Voice prompts use the same ring via `feedPCM`. SBC callbacks would splice
  // music PCM into the middle of a clip (missing Wi‑Fi / "… Mode" tails, no
  // "Bluetooth connected" over the prompt). Drop BT PCM until SPIFFS MP3 ends.
  if (geelen_idf::mp3::streamer()->isPlayingLocalFile() || geelen_idf::voiceFeedbackBootPromptsActive()) {
    return;
  }

  // Data is stereo 16-bit PCM decoded from SBC by Bluedroid.
  const int16_t* in = reinterpret_cast<const int16_t*>(data);
  const size_t in_samples = len / sizeof(int16_t);
  if (in_samples < 2) return;
  const size_t in_frames = in_samples / 2;

  // Drift control: keep the engine ring around ~1.0s filled to absorb BT jitter without
  // accumulating latency. If the I2S clock is slightly off, this also prevents long-term
  // buffer growth/shrink which causes artifacts.
  const size_t fill = engine_->bufferedFrames();
  const size_t cap = geelen::RING_BUF_SAMPLES;
  const size_t target = geelen::SAMPLE_RATE;  // ~1s
  if (cap > 0) {
    int32_t err = (int32_t)fill - (int32_t)target;
    // Map error to a small rate tweak (±2%). Positive err => buffer too full => speed up.
    int32_t ppm = (err * 20000) / (int32_t)cap;  // about ±20k ppm = ±2%
    if (ppm > 20000) ppm = 20000;
    if (ppm < -20000) ppm = -20000;
    s_step_adj_q16 = (uint32_t)((1 << 16) + (int32_t)((int64_t)ppm * (1 << 16) / 1000000LL));
  } else {
    s_step_adj_q16 = (1 << 16);
  }

  // Resample to 44.1kHz (engine/I2S) using linear interpolation.
  // Build a contiguous input window: optional carry frame + current buffer.
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

  // Always run through the resampler so we can apply the drift adjustment even when
  // input is nominally 44.1kHz.
  const uint32_t base_step = (uint32_t)(((uint64_t)s_bt_pcm_hz << 16) / (uint64_t)geelen::SAMPLE_RATE);
  const uint32_t step = (uint32_t)(((uint64_t)base_step * (uint64_t)s_step_adj_q16) >> 16);
  const uint32_t end_q16 = (uint32_t)((w_frames - 1) << 16);  // need i1 within range

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

  // Keep last frame for interpolation continuity across callbacks.
  s_carry_frame[0] = window[(w_frames - 1) * 2];
  s_carry_frame[1] = window[(w_frames - 1) * 2 + 1];
  s_have_carry_frame = true;

  // Wrap phase to keep it small.
  s_pos_q16 -= end_q16;

  if (!out.empty()) {
    engine_->feedPCM(reinterpret_cast<const uint8_t*>(out.data()), out.size() * sizeof(int16_t));
  }
}

}  // namespace geelen_idf


#include "idf_test_tone.h"

#include "audio_engine.h"
#include "idf_config_manager.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cmath>

namespace {

const char* TAG = "TestTone";

TaskHandle_t s_task = nullptr;
geelen::AudioEngine* s_eng = nullptr;
geelen_idf::ConfigManager* s_cfg = nullptr;
volatile bool s_params_dirty = true;

// ---- Pink noise (Paul Kellet / musicdsp-style 7-pole on white) --------------
struct PinkKellet {
  float b0{0}, b1{0}, b2{0}, b3{0}, b4{0}, b5{0}, b6{0};
  float step(float white) {
    b0 = 0.99886f * b0 + white * 0.0555179f;
    b1 = 0.99332f * b1 + white * 0.0750759f;
    b2 = 0.96900f * b2 + white * 0.1538520f;
    b3 = 0.86650f * b3 + white * 0.3104856f;
    b4 = 0.55000f * b4 + white * 0.5329522f;
    b5 = -0.7616f * b5 - white * 0.0168980f;
    const float out = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f;
    b6 = white * 0.115926f;
    return out * 0.11f;
  }
};

// ---- Generator state (continuous across chunks) ---------------------------
struct GenState {
  double phase{0.0};
  double phase2{0.0};
  double sweep_t{0.0};
  /** Instantaneous sweep frequency (Hz); log sweep advances by `mult` per sample. */
  double sweep_f_hz{0.0};
  uint32_t lcg{0xC0FFEEu};
  PinkKellet pinkL;
  PinkKellet pinkR;
  uint64_t frame_idx{0};
};

static GenState s_gen;

inline float white_u32(uint32_t& s) {
  s = s * 1664525u + 1013904223u;
  const int32_t x = (int32_t)s;
  return (float)x * (1.f / 2147483648.f);
}

inline void wrap_phase(double& ph) {
  constexpr double k2pi = 6.28318530717958647692;
  while (ph >= k2pi) ph -= k2pi;
  while (ph < 0.0) ph += k2pi;
}

inline float tri_from_phase(double ph) {
  constexpr double kPi = 3.14159265358979323846;
  const double x = ph / kPi;
  if (x < 1.0) return (float)(-1.0 + 2.0 * x);
  return (float)(3.0 - 2.0 * x);
}

inline float saw_from_phase(double ph) {
  constexpr double kPi = 3.14159265358979323846;
  return (float)(ph / kPi - 1.0);
}

void toneTask(void*) {
  // Match `AudioEngine::process()` (128 frames) so pacing lines up with the consumer.
  constexpr unsigned kChunkFrames = 128;
  int16_t pcm[kChunkFrames * 2];
  // Same ceiling as `idf_mp3_streamer.cpp`: avoid overflowing the ring (drops sound like
  // pulsing) while staying above the engine prefill/low-water marks.
  constexpr size_t kHighWaterFrames =
      (size_t)((uint64_t)geelen::SAMPLE_RATE * 1100 / 1000);

  while (true) {
    if (!s_eng || !s_cfg) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (s_params_dirty) {
      s_params_dirty = false;
      s_gen = GenState{};
    }

    const float f_hz = s_cfg->getTestToneFreqHz();
    const float amp_db = s_cfg->getTestToneAmpDb();
    const float pan = s_cfg->getTestTonePan();
    const uint8_t mode = s_cfg->getTestToneMode();
    const float sweep_s = s_cfg->getTestToneSweepS();
    const float lo_hz = s_cfg->getTestToneSweepLoHz();
    const float hi_hz = s_cfg->getTestToneSweepHiHz();
    const float f2_hz = s_cfg->getTestToneDualF2Hz();

    const float sr = (float)geelen::SAMPLE_RATE;
    const float lin = powf(10.f, amp_db / 20.f) * 0.45f;
    const float ang = (pan + 1.f) * (3.14159265f * 0.25f);
    const float g_l = cosf(ang) * lin;
    const float g_r = sinf(ang) * lin;

    const float w_main = 2.f * 3.14159265f * f_hz / sr;
    const float w2 = 2.f * 3.14159265f * f2_hz / sr;
    const double sweep_T = (double)sweep_s;
    const double f0d = (double)lo_hz;
    const double f1d = (double)hi_hz;
    const double log_ratio = (f1d > f0d && f0d > 0.0) ? (log(f1d / f0d) / sweep_T) : 0.0;

    const uint64_t sr_u = (uint64_t)geelen::SAMPLE_RATE;
    const uint64_t half_polarity = 3u * sr_u;
    const uint64_t period_polarity = 6u * sr_u;
    const uint64_t half_channel = sr_u;

    const double sr_d = (double)geelen::SAMPLE_RATE;
    // One multiply per sample instead of exp() — avoids starving the feed loop (audible stutter).
    const double sweep_mult =
        (mode == 6 && sweep_T > 0.0 && log_ratio != 0.0) ? exp(log_ratio / sr_d) : 1.0;
    if (mode == 6 && sweep_T > 0.0 && log_ratio != 0.0) {
      s_gen.sweep_f_hz = f0d * exp(log_ratio * s_gen.sweep_t);
    }

    for (unsigned i = 0; i < kChunkFrames; ++i) {
      float sL = 0.f, sR = 0.f;

      switch (mode) {
        case 0: {  // sine
          sL = sinf((float)s_gen.phase);
          sR = sL;
          s_gen.phase += (double)w_main;
          wrap_phase(s_gen.phase);
          break;
        }
        case 1: {  // square
          sL = ((s_gen.phase < 3.14159265358979323846) ? 1.f : -1.f);
          sR = sL;
          s_gen.phase += (double)w_main;
          wrap_phase(s_gen.phase);
          break;
        }
        case 2: {  // triangle
          sL = tri_from_phase(s_gen.phase);
          sR = sL;
          s_gen.phase += (double)w_main;
          wrap_phase(s_gen.phase);
          break;
        }
        case 3: {  // saw
          sL = saw_from_phase(s_gen.phase);
          sR = sL;
          s_gen.phase += (double)w_main;
          wrap_phase(s_gen.phase);
          break;
        }
        case 4: {  // white
          sL = white_u32(s_gen.lcg);
          sR = white_u32(s_gen.lcg);
          break;
        }
        case 5: {  // pink
          sL = s_gen.pinkL.step(white_u32(s_gen.lcg));
          sR = s_gen.pinkR.step(white_u32(s_gen.lcg));
          break;
        }
        case 6: {  // log sweep mono
          if (sweep_T > 0.0 && log_ratio != 0.0) {
            const double w = 6.28318530717958647692 * s_gen.sweep_f_hz / sr_d;
            sL = sinf((float)s_gen.phase);
            sR = sL;
            s_gen.phase += w;
            wrap_phase(s_gen.phase);
            s_gen.sweep_f_hz *= sweep_mult;
            s_gen.sweep_t += 1.0 / sr_d;
            if (s_gen.sweep_t >= sweep_T) {
              s_gen.sweep_t = 0.0;
              s_gen.sweep_f_hz = f0d;
              s_gen.phase = 0.0;
            }
          } else {
            sL = sR = 0.f;
          }
          break;
        }
        case 7: {  // channel ID: L then R
          const float s = sinf((float)s_gen.phase);
          const uint64_t p = s_gen.frame_idx % (2u * sr_u);
          if (p < half_channel) {
            sL = s;
            sR = 0.f;
          } else {
            sL = 0.f;
            sR = s;
          }
          s_gen.phase += (double)w_main;
          wrap_phase(s_gen.phase);
          s_gen.frame_idx++;
          break;
        }
        case 8: {  // polarity: in-phase then R inverted
          const float s = sinf((float)s_gen.phase);
          const uint64_t p = s_gen.frame_idx % period_polarity;
          sL = s;
          sR = (p < half_polarity) ? s : -s;
          s_gen.phase += (double)w_main;
          wrap_phase(s_gen.phase);
          s_gen.frame_idx++;
          break;
        }
        case 9: {  // dual-tone IMD
          const float s1 = sinf((float)s_gen.phase);
          const float s2 = sinf((float)s_gen.phase2);
          sL = sR = 0.5f * s1 + 0.5f * s2;
          s_gen.phase += (double)w_main;
          wrap_phase(s_gen.phase);
          s_gen.phase2 += (double)w2;
          wrap_phase(s_gen.phase2);
          break;
        }
        case 10:  // silence
        default:
          sL = sR = 0.f;
          break;
      }

      pcm[i * 2] = (int16_t)(sL * g_l * 32767.f);
      pcm[i * 2 + 1] = (int16_t)(sR * g_r * 32767.f);
    }

    while (s_eng->bufferedFrames() > kHighWaterFrames) {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_eng->feedPCM(reinterpret_cast<const uint8_t*>(pcm), sizeof(pcm));
    taskYIELD();
  }
}

}  // namespace

extern "C" void testToneNotifyParamsUpdated(void) { s_params_dirty = true; }

namespace geelen_idf {

void testToneStart(geelen::AudioEngine* eng, ConfigManager* cfg) {
  s_eng = eng;
  s_cfg = cfg;
  if (s_task) return;
  if (!eng || !cfg) return;
  s_gen = GenState{};
  const BaseType_t ok =
      xTaskCreatePinnedToCore(toneTask, "test_tone", 4096, nullptr, 5, &s_task, 1);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "task create failed");
    s_task = nullptr;
  } else {
    ESP_LOGI(TAG, "generator started");
  }
}

void testToneStop() {
  if (s_task) {
    vTaskDelete(s_task);
    s_task = nullptr;
  }
  s_eng = nullptr;
  s_cfg = nullptr;
}

}  // namespace geelen_idf

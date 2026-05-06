#include "idf_voice_feedback.h"

#include "audio_engine.h"
#include "idf_config_manager.h"
#include "idf_mp3_streamer.h"
#include "idf_wifi_manager.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <atomic>

namespace geelen_idf {

namespace {

constexpr const char* TAG = "voice-fb";

constexpr const char* kPathWifiConnected = "/spiffs/audio/WifiConnected.mp3";
constexpr const char* kPathWifiDisconnected = "/spiffs/audio/WifiDisconnected.mp3";
constexpr const char* kPathBluetoothMode = "/spiffs/audio/BluetoothMode.mp3";
constexpr const char* kPathSnapcastMode = "/spiffs/audio/SnapcastMode.mp3";
constexpr const char* kPathDlnaMode = "/spiffs/audio/DLNAMode.mp3";
constexpr const char* kPathBluetoothConnected = "/spiffs/audio/BluetoothConnected.mp3";
constexpr const char* kPathPairing = "/spiffs/audio/Pairing.mp3";

ConfigManager* s_cfg = nullptr;
WifiManager* s_wifi = nullptr;
geelen::AudioEngine* s_eng = nullptr;

/** Posted once when `voice_boot_task` is done (or skipped) so test-tone can start without interrupting prompts. */
static SemaphoreHandle_t s_boot_done_sem = nullptr;

/** Keeps A2DP/Snapcast from feeding the ring between boot clips and during tail drain. */
static std::atomic<bool> s_boot_voice_active{false};
/** Set from A2DP connect/stream — `voice_boot_task` exits early and stops MP3. */
static std::atomic<bool> s_boot_abort{false};

// Wait until the previous prompt has actually played out, not just decoded.
// `isPlaying()` flips false the moment the streamer task finishes reading the
// MP3, but the engine's ring still holds the streamer's high-water mark
// (~1.1 s) of PCM at that moment. We need to drain that before queuing the
// next clip; otherwise back-pressure stalls clip 2's first frames and we'd
// hear "Wifi Connected → silence → Bluetooth Mode" with audible mid-clip gaps.
//
//   1. Decoder stops feeding (`isPlaying()` -> false).
//   2. Engine ring drains to just above `AUDIO_LOW_WATER_MS` (300 ms) so the
//      audio task stays in the playing state — this avoids triggering a 1 s
//      prefill/rebuffer cycle between clips while still leaving ~800 ms of
//      headroom for the next clip's PCM.
static void waitMp3Finished() {
  // (1) decoder finished
  for (int i = 0; i < 600; ++i) {
    if (s_boot_abort.load(std::memory_order_relaxed)) return;
    if (!geelen_idf::mp3::streamer()->isPlaying()) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  // (2) engine ring drained to a small tail (just above the 300 ms low-water
  //     so the engine doesn't slip into rebuffer between clips).
  if (!s_eng) return;
  constexpr size_t kTailFrames =
      (size_t)((uint64_t)geelen::SAMPLE_RATE * 400 / 1000);  // ~400 ms
  for (int i = 0; i < 600; ++i) {  // up to ~12 s safety cap
    if (s_boot_abort.load(std::memory_order_relaxed)) return;
    if (s_eng->bufferedFrames() <= kTailFrames) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void finish_boot_voice_task() {
  s_boot_abort.store(false, std::memory_order_relaxed);
  s_boot_voice_active.store(false, std::memory_order_relaxed);
  if (s_boot_done_sem) xSemaphoreGive(s_boot_done_sem);
  vTaskDelete(nullptr);
}

static void voice_boot_task(void*) {
  if (!s_cfg || !s_wifi || !s_eng) {
    finish_boot_voice_task();
    return;
  }
  // If prompts are off, do not hold the audio path — release waiters immediately
  // so test-tone / other sources can start without a 3.5 s dead wait.
  if (!s_cfg->getVoiceFeedbackEnabled()) {
    finish_boot_voice_task();
    return;
  }

  // Wait until BT/Wi-Fi bring-up + I2S prefill have settled. Empirically
  // 1600ms races with the audio task's first `[Audio] start` log on Core 1
  // and BT controller chatter on Core 0; a longer settle removes both.
  vTaskDelay(pdMS_TO_TICKS(3500));

  if (s_boot_abort.load(std::memory_order_relaxed)) {
    finish_boot_voice_task();
    return;
  }

  // Hold A2DP/Snap off for the whole boot sequence (including gaps between clips).
  s_boot_voice_active.store(true, std::memory_order_relaxed);

  const bool has_ip = s_wifi->status().sta_ip != 0;
  const char* wifi_clip = has_ip ? kPathWifiConnected : kPathWifiDisconnected;
  if (!geelen_idf::mp3::streamer()->playFile(wifi_clip)) {
    ESP_LOGW(TAG, "playFile failed: %s", wifi_clip);
  } else {
    waitMp3Finished();
  }
  if (s_boot_abort.load(std::memory_order_relaxed)) {
    finish_boot_voice_task();
    return;
  }

  const char* mode_clip = nullptr;
  switch (s_cfg->getAudioSource()) {
    case AudioSource::Bluetooth:
      mode_clip = kPathBluetoothMode;
      break;
    case AudioSource::Network:
      mode_clip = kPathSnapcastMode;
      break;
    case AudioSource::Dlna:
      mode_clip = kPathDlnaMode;
      break;
    case AudioSource::TestTone:
      break;
  }
  if (mode_clip && !geelen_idf::mp3::streamer()->playFile(mode_clip)) {
    ESP_LOGW(TAG, "playFile failed: %s", mode_clip);
  } else if (mode_clip) {
    waitMp3Finished();
  }

  if (s_boot_abort.load(std::memory_order_relaxed)) {
    finish_boot_voice_task();
    return;
  }

  finish_boot_voice_task();
}

}  // namespace

bool voiceFeedbackBootPromptsActive() {
  return s_boot_voice_active.load(std::memory_order_relaxed);
}

void voiceFeedbackAbortBootForUserInteraction() {
  s_boot_abort.store(true, std::memory_order_relaxed);
  s_boot_voice_active.store(false, std::memory_order_relaxed);
  if (s_eng) {
    geelen_idf::mp3::streamer()->setEngine(s_eng);
    geelen_idf::mp3::streamer()->stop();
  }
}

void voiceFeedbackSetContext(ConfigManager* cfg, WifiManager* wifi, geelen::AudioEngine* eng) {
  s_cfg = cfg;
  s_wifi = wifi;
  s_eng = eng;
}

void voiceFeedbackScheduleBoot() {
  if (!s_cfg || !s_wifi || !s_eng) return;
  s_boot_abort.store(false, std::memory_order_relaxed);
  if (!s_boot_done_sem) {
    s_boot_done_sem = xSemaphoreCreateBinary();
    if (!s_boot_done_sem) {
      ESP_LOGW(TAG, "boot-done sem create failed");
      return;
    }
  }
  // Pinned to Core 1 (audio side) and given a generous stack so logging,
  // SPIFFS path strings, and FreeRTOS book-keeping have margin even under
  // the worst-case BT/Wi-Fi storm on Core 0 during the first seconds.
  BaseType_t ok = xTaskCreatePinnedToCore(voice_boot_task, "voice-boot",
                                          6144, nullptr, 3, nullptr, 1);
  if (ok != pdPASS) {
    ESP_LOGW(TAG, "voice boot task create failed");
    if (s_boot_done_sem) xSemaphoreGive(s_boot_done_sem);
  }
}

void voiceFeedbackWaitForBootDone(TickType_t timeout_ticks) {
  if (!s_boot_done_sem) return;
  (void)xSemaphoreTake(s_boot_done_sem, timeout_ticks);
}

void voiceFeedbackPlayFileIfEnabled(const char* abs_path) {
  if (!abs_path || !s_cfg || !s_eng) return;
  if (!s_cfg->getVoiceFeedbackEnabled()) return;
  geelen_idf::mp3::streamer()->setEngine(s_eng);
  if (!geelen_idf::mp3::streamer()->playFile(abs_path)) {
    ESP_LOGW(TAG, "playFile failed: %s", abs_path);
  }
}

void voiceFeedbackOnA2dpConnected() {
  // Stop any in-flight boot Wi‑Fi / mode clips so we never splice two voice
  // prompts, then play "Bluetooth connected". A2DP music stays gated by
  // `mp3::streamer()->isPlayingLocalFile()` until that clip decodes and the
  // ring tail drains (see idf_mp3_streamer.cpp).
  voiceFeedbackAbortBootForUserInteraction();
  if (!s_cfg || !s_cfg->getVoiceFeedbackEnabled()) return;
  voiceFeedbackPlayFileIfEnabled(kPathBluetoothConnected);
}

void voiceFeedbackOnBtPairing() {
  if (!s_cfg || !s_cfg->getVoiceFeedbackEnabled()) return;
  static uint64_t last_us = 0;
  const uint64_t now = esp_timer_get_time();
  if (now - last_us < 3000000ULL) return;
  last_us = now;
  voiceFeedbackPlayFileIfEnabled(kPathPairing);
}

}  // namespace geelen_idf

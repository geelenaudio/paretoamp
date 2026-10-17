// See idf_voice_feedback.h for the design.
//
// Phase 2: the prompt path is pure PCM. No Helix, no command mailbox, no
// 12 s safety caps. The voice task pulls a `voice_clip_id_t` off the queue,
// looks up the pre-decoded PCM in `idf_voice_cache`, and feeds it to the
// engine ring in 1024-frame chunks paced just above realtime. The last
// queued clip is followed by ~400 ms of silence so the engine's low-water
// rebuffer abandons zeros rather than the last syllable. After every clip
// we post `EV_VOICE_PROMPT_FINISHED`; after the last queued boot clip we
// post `EV_VOICE_BOOT_DONE` AND give the legacy boot-done semaphore so
// existing waiters (DLNA SOAP `Play`, test-tone start) keep working without
// a rewrite of those callsites.

#include "idf_voice_feedback.h"

#include "audio_engine.h"
#include "idf_config_manager.h"
#include "idf_events.h"
#include "idf_voice_cache.h"
#include "idf_wifi_manager.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>

namespace geelen_idf {

namespace {

constexpr const char* TAG = "voice-fb";

ConfigManager* s_cfg = nullptr;
WifiManager* s_wifi = nullptr;
geelen::AudioEngine* s_eng = nullptr;

// One queued clip. `is_boot` flags the boot-pair entries so the serializer
// knows when to give `s_boot_done_sem` and post `EV_VOICE_BOOT_DONE`.
struct PromptItem {
  voice_clip_id_t id;
  bool is_boot;
};

constexpr size_t kPromptQueueDepth = 8;

// Producer-side gate. Bumped synchronously when a clip is enqueued so
// A2DP / Snap producers see "voice owns the ring" the instant we decide
// to play one. Dropped by the serializer after the engine ring has
// drained back below the rebuffer threshold + a small margin.
std::atomic<int> s_voice_owners{0};

// Tracks how many boot-flagged clips are still in flight. When this hits
// zero we post EV_VOICE_BOOT_DONE and give s_boot_done_sem.
std::atomic<int> s_boot_prompts_remaining{0};

SemaphoreHandle_t s_boot_done_sem = nullptr;
QueueHandle_t s_prompt_queue = nullptr;
TaskHandle_t s_voice_task_handle = nullptr;

// Feed pacing shared by clip PCM and the last-clip silence pad. 1024 frames
// ≈ 23 ms at 44.1 kHz; sleeping 18 ms between chunks is ~1.27× realtime —
// fast enough that a 1.5 s clip finishes feeding in ~1.2 s wallclock
// without the ring (~2 s capacity) ever filling past ~80 %.
constexpr size_t kFeedChunkFrames = 1024;
constexpr int kFeedChunkDelayMs = 18;

// Extra silence after the last queued clip. The engine rebuffers below
// AUDIO_LOW_WATER_MS and abandons whatever is still in the ring, so this
// pad must be longer than that abandoned zone. Drain then stops at
// low-water + 40 ms (~340 ms) with only silence remaining.
constexpr uint32_t kLastClipSilenceMs = geelen::AUDIO_LOW_WATER_MS + 100;

// Block until the engine ring has drained back close to its low-water mark
// before dropping the producer gate. Without this, A2DP/Snap PCM would
// splice in during the rebuffer-margin window and the clip's tail would
// be replaced by music PCM.
void drainEngineTail() {
  if (!s_eng) return;
  constexpr size_t kTailFrames =
      (size_t)((uint64_t)geelen::SAMPLE_RATE * (geelen::AUDIO_LOW_WATER_MS + 40) / 1000);
  // ~12 s safety cap (800 × 15 ms) — same as before, but we no longer expect
  // to ever hit it because there is no decoder back-pressure path.
  for (int i = 0; i < 800; ++i) {
    if (s_eng->bufferedFrames() <= kTailFrames) break;
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

void feedPcmFrames(const int16_t* pcm, size_t frame_count) {
  if (!s_eng || !pcm || frame_count == 0) return;
  size_t pos = 0;
  while (pos < frame_count) {
    const size_t n = (kFeedChunkFrames < frame_count - pos) ? kFeedChunkFrames
                                                            : (frame_count - pos);
    s_eng->feedPCM(reinterpret_cast<const uint8_t*>(pcm + pos * 2),
                   n * 2 * sizeof(int16_t));
    pos += n;
    if (pos < frame_count) {
      vTaskDelay(pdMS_TO_TICKS(kFeedChunkDelayMs));
    }
  }
}

void feedClipToEngine(const VoiceClip& c) {
  if (!c.pcm || c.frame_count == 0) return;
  feedPcmFrames(c.pcm, c.frame_count);
}

// Paced zeros so the last clip's speech is fully consumed past low-water
// before drainEngineTail returns. The engine then abandons ~300 ms of
// silence rather than the last syllable. Same chunk/delay as feedPcmFrames;
// kZeros is only one chunk long so we loop here instead of one call.
void feedSilence(size_t frame_count) {
  static const int16_t kZeros[kFeedChunkFrames * 2] = {};
  if (!s_eng || frame_count == 0) return;
  size_t pos = 0;
  while (pos < frame_count) {
    const size_t n = (kFeedChunkFrames < frame_count - pos) ? kFeedChunkFrames
                                                            : (frame_count - pos);
    s_eng->feedPCM(reinterpret_cast<const uint8_t*>(kZeros),
                   n * 2 * sizeof(int16_t));
    pos += n;
    if (pos < frame_count) {
      vTaskDelay(pdMS_TO_TICKS(kFeedChunkDelayMs));
    }
  }
}

void notifyBootPromptDrained() {
  // Fetch_sub returns the previous value, so 1 means we just hit zero
  // (= last boot prompt fully drained).
  int prev = s_boot_prompts_remaining.fetch_sub(1, std::memory_order_acq_rel);
  if (prev <= 1) {
    s_boot_prompts_remaining.store(0, std::memory_order_relaxed);
    if (s_boot_done_sem) xSemaphoreGive(s_boot_done_sem);
    eventPost(EV_VOICE_BOOT_DONE, nullptr, 0);
  }
}

void voice_task(void*) {
  ESP_LOGI(TAG, "voice task started (PCM-cache mode)");
  for (;;) {
    PromptItem item{};
    if (xQueueReceive(s_prompt_queue, &item, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    const VoiceClip* clip = voiceCacheGet(item.id);
    if (clip) {
      ESP_LOGI(TAG, "play %s (id=%u boot=%d owners=%d)", clip->name,
               (unsigned)item.id, (int)item.is_boot,
               s_voice_owners.load(std::memory_order_relaxed));
      feedClipToEngine(*clip);
      // Last clip in the queue: pad past the engine's rebuffer threshold so
      // the abandoned ring tail is silence, not the last syllable. Mid-sequence
      // clips skip this — the next clip tops the ring up and their tails play.
      if (uxQueueMessagesWaiting(s_prompt_queue) == 0) {
        const size_t silence_frames =
            (size_t)((uint64_t)geelen::SAMPLE_RATE * kLastClipSilenceMs / 1000);
        feedSilence(silence_frames);
      }
      drainEngineTail();
    } else {
      ESP_LOGW(TAG, "skipping unloaded clip id=%u", (unsigned)item.id);
    }

    // Drop the producer gate AFTER the tail drain so A2DP/Snap don't slip
    // music PCM in during the rebuffer-margin window.
    int prev = s_voice_owners.fetch_sub(1, std::memory_order_acq_rel);
    if (prev <= 0) {
      s_voice_owners.store(0, std::memory_order_relaxed);
    }

    // Notify listeners. EV_VOICE_PROMPT_FINISHED carries the same payload
    // the supervisor uses to enqueue runtime prompts, so handlers can
    // chain follow-up clips deterministically.
    ev_voice_prompt_t payload{};
    payload.prompt_id = (uint8_t)item.id;
    payload.is_boot = item.is_boot ? 1 : 0;
    eventPost(EV_VOICE_PROMPT_FINISHED, &payload, sizeof(payload));

    if (item.is_boot) {
      notifyBootPromptDrained();
    }
  }
}

bool ensureVoiceTask() {
  if (!s_boot_done_sem) {
    s_boot_done_sem = xSemaphoreCreateBinary();
    if (!s_boot_done_sem) {
      ESP_LOGW(TAG, "boot-done sem create failed");
      return false;
    }
  }
  if (!s_prompt_queue) {
    s_prompt_queue = xQueueCreate(kPromptQueueDepth, sizeof(PromptItem));
    if (!s_prompt_queue) {
      ESP_LOGW(TAG, "prompt queue create failed");
      return false;
    }
  }
  if (!s_voice_task_handle) {
    // 4 KiB stack is plenty: the task body is a memcpy loop + log
    // formatting (no Helix decoder locals — those left with Phase 2).
    //
    // Stack lives in PSRAM (`MALLOC_CAP_SPIRAM`) deliberately: the
    // internal-DRAM pool is contended with Bluedroid's L2CAP/SDP CCB
    // allocations during phone connect (each CCB needs a ~210 B
    // `xmit_hold_q` from the BT-OSI heap), and on this build that pool
    // is tight enough that an extra 4 KiB stack chunk in DRAM was
    // tipping `osi_malloc` into a silent NULL return → panic at the
    // first L2CAP enqueue. PSRAM stacks are safe here: voice_task does
    // no FPU work, no flash-write critical sections, and ISRs run on
    // the per-CPU ISR stack on Xtensa.
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        voice_task, "voice", /*stack*/ 4096, nullptr, /*prio*/ 3,
        &s_voice_task_handle, /*core*/ 1, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
      ESP_LOGW(TAG, "voice task create failed");
      s_voice_task_handle = nullptr;
      return false;
    }
  }
  return true;
}

void enqueueImpl(voice_clip_id_t id, bool is_boot) {
  if (!ensureVoiceTask()) return;
  s_voice_owners.fetch_add(1, std::memory_order_acq_rel);
  if (is_boot) s_boot_prompts_remaining.fetch_add(1, std::memory_order_acq_rel);
  PromptItem item{id, is_boot};
  if (xQueueSend(s_prompt_queue, &item, 0) != pdTRUE) {
    ESP_LOGW(TAG, "prompt queue full, dropping id=%u", (unsigned)id);
    s_voice_owners.fetch_sub(1, std::memory_order_acq_rel);
    if (is_boot) {
      // Clean up the boot remaining counter and notify if we just made it
      // through "no boot prompts left" — otherwise waiters would hang.
      int prev = s_boot_prompts_remaining.fetch_sub(1, std::memory_order_acq_rel);
      if (prev <= 1) {
        s_boot_prompts_remaining.store(0, std::memory_order_relaxed);
        if (s_boot_done_sem) xSemaphoreGive(s_boot_done_sem);
        eventPost(EV_VOICE_BOOT_DONE, nullptr, 0);
      }
    }
  }
}

}  // namespace

void voiceFeedbackSetContext(ConfigManager* cfg, WifiManager* wifi, geelen::AudioEngine* eng) {
  s_cfg = cfg;
  s_wifi = wifi;
  s_eng = eng;
}

bool voiceFeedbackActive() {
  return s_voice_owners.load(std::memory_order_acquire) > 0;
}

void voiceFeedbackScheduleBoot() {
  if (!s_cfg || !s_wifi || !s_eng) {
    ESP_LOGW(TAG, "scheduleBoot: missing context (cfg=%p wifi=%p eng=%p)", s_cfg, s_wifi, s_eng);
    if (s_boot_done_sem) xSemaphoreGive(s_boot_done_sem);
    eventPost(EV_VOICE_BOOT_DONE, nullptr, 0);
    return;
  }

  if (!ensureVoiceTask()) {
    if (s_boot_done_sem) xSemaphoreGive(s_boot_done_sem);
    eventPost(EV_VOICE_BOOT_DONE, nullptr, 0);
    return;
  }

  if (!s_cfg->getVoiceFeedbackEnabled()) {
    ESP_LOGI(TAG, "voice feedback disabled in NVS — skipping boot prompts");
    if (s_boot_done_sem) xSemaphoreGive(s_boot_done_sem);
    eventPost(EV_VOICE_BOOT_DONE, nullptr, 0);
    return;
  }

  const bool has_ip = s_wifi->status().sta_ip != 0;
  enqueueImpl(has_ip ? VOICE_CLIP_WIFI_CONNECTED : VOICE_CLIP_WIFI_DISCONNECTED, true);

  voice_clip_id_t mode_clip = VOICE_CLIP_COUNT_;  // sentinel = none
  switch (s_cfg->getAudioSource()) {
    case AudioSource::Bluetooth: mode_clip = VOICE_CLIP_BLUETOOTH_MODE; break;
    case AudioSource::Network:   mode_clip = VOICE_CLIP_SNAPCAST_MODE; break;
    case AudioSource::Dlna:      mode_clip = VOICE_CLIP_DLNA_MODE; break;
    case AudioSource::Usb:       mode_clip = VOICE_CLIP_USB_MODE; break;
    case AudioSource::TestTone:
      // Test-tone has no spoken mode clip — the Wi-Fi clip alone marks
      // "boot pair done".
      break;
  }
  if (mode_clip != VOICE_CLIP_COUNT_) {
    enqueueImpl(mode_clip, true);
  }

  ESP_LOGI(TAG, "scheduleBoot: queued %d boot prompt(s) (has_ip=%d, source=%u)",
           s_boot_prompts_remaining.load(std::memory_order_relaxed), (int)has_ip,
           (unsigned)s_cfg->getAudioSource());

  if (s_boot_prompts_remaining.load(std::memory_order_acquire) == 0) {
    if (s_boot_done_sem) xSemaphoreGive(s_boot_done_sem);
    eventPost(EV_VOICE_BOOT_DONE, nullptr, 0);
  }
}

void voiceFeedbackWaitForBootDone(TickType_t timeout_ticks) {
  if (!s_boot_done_sem) return;
  if (xSemaphoreTake(s_boot_done_sem, timeout_ticks) == pdTRUE) {
    // Re-give so subsequent waiters succeed immediately. The semaphore
    // becomes a sticky "boot done" latch once it has fired.
    xSemaphoreGive(s_boot_done_sem);
  }
}

void voiceFeedbackEnqueueClip(voice_clip_id_t id) {
  if (!s_cfg) return;
  if (!s_cfg->getVoiceFeedbackEnabled()) return;
  if ((int)id < 0 || (int)id >= (int)VOICE_CLIP_COUNT_) return;
  enqueueImpl(id, /*is_boot=*/false);
}

}  // namespace geelen_idf

// Voice prompt serializer.
//
// Phase 2 rewrite: prompts are pre-decoded PCM in PSRAM (see idf_voice_cache.h),
// so the serializer is a tiny memcpy-paced feeder rather than the multi-gate
// streamer-borrowing dance the original implementation needed. The public
// surface kept by this header is the minimum that callers outside this module
// still reach for:
//
//   voiceFeedbackSetContext(cfg, wifi, eng)   // once at boot, in app_main
//   voiceFeedbackScheduleBoot()                // queues Wi-Fi + mode clips
//   voiceFeedbackEnqueueClip(id)               // enqueue any cached clip
//   voiceFeedbackActive()                      // producer-side gate (BT/Snap)
//   voiceFeedbackWaitForBootDone(timeout)      // legacy wait — backed by EV_VOICE_BOOT_DONE
//
// The path-based prompt API and the BT-side "On*" hooks are gone: BT callbacks
// post `EV_A2DP_CONNECTED` / `EV_A2DP_PAIRING` to the supervisor, which decides
// whether to enqueue the matching cached clip. This removes the cross-module
// coupling between BT host-task callbacks and the voice serializer.

#pragma once

#include "freertos/FreeRTOS.h"

#include "idf_voice_cache.h"  // voice_clip_id_t

namespace geelen {
class AudioEngine;
}
namespace geelen_idf {
class ConfigManager;
class WifiManager;

void voiceFeedbackSetContext(ConfigManager* cfg, WifiManager* wifi, geelen::AudioEngine* eng);

/** Queue boot Wi-Fi + mode clips and bump the producer gate synchronously. */
void voiceFeedbackScheduleBoot();

/** Block until the boot pair drained (or returns immediately if voice is off / scheduling failed). */
void voiceFeedbackWaitForBootDone(TickType_t timeout_ticks = portMAX_DELAY);

/** Enqueue a cached voice clip. No-op if voice feedback is disabled or the
 *  clip failed to decode at boot. Safe to call from any task (incl. BT host
 *  callback context). */
void voiceFeedbackEnqueueClip(voice_clip_id_t id);

/** True while any prompt PCM is still in the engine ring (or about to be).
 *  Producers (A2DP, Snapcast) gate on this so they cannot splice music PCM
 *  into a clip's tail. */
bool voiceFeedbackActive();

}  // namespace geelen_idf

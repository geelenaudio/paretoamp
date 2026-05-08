// SPIFFS voice prompts decoded once at boot into PSRAM as raw PCM.
//
// Phase 2 of the event-driven rebuild. Replaces the per-prompt Helix decode
// + streamer-singleton borrow with a small read-only table of pre-decoded
// stereo s16 buffers. The voice task in idf_voice_feedback.cpp feeds these
// directly to AudioEngine::feedPCM, paced to roughly realtime.
//
// Why decode at boot:
//   - Eliminates the ~7 timing constants tuned to paper over MP3 streamer
//     races: kBootSettleMs, drainEngineTail margin, waitMp3DecoderFinished
//     12 s cap, pumpOnce 1100 ms high-water, etc.
//   - Boot prompts no longer race the engine prefill: PCM lands directly in
//     the ring, and ~1.5 s of clip IS more than enough prefill on its own.
//   - DLNA `Play` no longer contends with the prompt path for the streamer
//     singleton, so the 15 s SOAP boot-wait can become an event subscription.
//
// Memory cost: ~1.5 MB of PSRAM total for the seven clips at 44.1 kHz
// stereo s16 (well under the 8 MB available on the WROVER-N8R8). Numbers
// are exposed via `voiceCacheTotalPsramBytes()` for /api/state.
//
// Lifetime: `voiceCacheInit()` runs once at boot, AFTER SPIFFS is mounted
// (i.e. after `ConfigManager::begin()`) and BEFORE the voice serializer is
// asked for any clip. The cache is then read-only for the rest of the
// firmware lifetime; clip pointers never move.

#ifndef GEELEN_IDF_VOICE_CACHE_H
#define GEELEN_IDF_VOICE_CACHE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Stable IDs of the voice prompts shipped under data/audio/. Producers use
// these in `ev_voice_prompt_t.prompt_id` so the wire format stays small;
// the supervisor / voice task look up the actual `VoiceClip` via
// `voiceCacheGet`. Adding a new clip MUST also extend `kClipManifest` in
// idf_voice_cache.cpp.
typedef enum {
  VOICE_CLIP_WIFI_CONNECTED = 0,
  VOICE_CLIP_WIFI_DISCONNECTED,
  VOICE_CLIP_BLUETOOTH_MODE,
  VOICE_CLIP_SNAPCAST_MODE,
  VOICE_CLIP_DLNA_MODE,
  VOICE_CLIP_BLUETOOTH_CONNECTED,
  VOICE_CLIP_PAIRING,
  VOICE_CLIP_COUNT_,
} voice_clip_id_t;

#ifdef __cplusplus
}  // extern "C"

namespace geelen_idf {

// One decoded clip. `pcm` is interleaved stereo s16 at the engine sample
// rate (44.1 kHz). The cache owns the buffer for the firmware lifetime;
// callers must NOT free it.
struct VoiceClip {
  const char* name;        // basename without path, for logs ("WifiConnected.mp3")
  voice_clip_id_t id;      // index into the manifest
  const int16_t* pcm;      // PSRAM-resident, never null after a successful decode
  size_t frame_count;      // stereo frames at 44.1 kHz
  uint32_t source_sr;      // original MP3 sample rate (informational)
  uint8_t source_channels; // 1 (mono) or 2 (stereo) before normalisation
};

// Decode every clip in `kClipManifest` into PSRAM. Idempotent. Safe to
// call multiple times; subsequent calls are no-ops. Returns true if every
// non-missing clip decoded successfully; logs a WARN and skips clips whose
// MP3 file is absent or unparsable (the firmware still boots — those
// prompts are simply silent).
//
// Must be called after SPIFFS is mounted and before the voice serializer
// is asked to play anything. Cost: ~1–2 s of synchronous decoding on a
// cold boot, run on the calling task (typically `app_main`).
bool voiceCacheInit();

// Look up a clip by id. Returns nullptr if `id` is out of range or the
// clip failed to decode at boot. Safe to call from any task once
// `voiceCacheInit()` has returned; the table is read-only thereafter.
const VoiceClip* voiceCacheGet(voice_clip_id_t id);

// Total bytes of PSRAM held by the decoded PCM buffers. Reported via
// `/api/state` once Phase 4 lands. Returns 0 before init.
size_t voiceCacheTotalPsramBytes();

}  // namespace geelen_idf

#endif  // __cplusplus

#endif  // GEELEN_IDF_VOICE_CACHE_H

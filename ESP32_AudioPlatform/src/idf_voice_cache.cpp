// See idf_voice_cache.h for the design.

#include "idf_voice_cache.h"

#include "audio_engine.h"  // geelen::SAMPLE_RATE

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

extern "C" {
#include "mp3dec.h"
}

namespace geelen_idf {

namespace {

constexpr const char* TAG = "voice-cache";

// Mirror of idf_mp3_streamer.cpp constants — same Helix limits apply.
constexpr size_t kInBufBytes = 8 * 1024;     // largest layer III frame fits ~2.9 KB
constexpr size_t kOutBufSamples = 2304;       // 1152 frames × 2 ch max

// Initial PSRAM buffer per clip. We `realloc` upward in chunks if the
// decoder produces more frames than this; ~5 s of stereo s16 at 44.1 kHz
// covers every clip we ship comfortably without a realloc.
constexpr size_t kInitialClipBytes = 5 * 44100 * 2 * sizeof(int16_t);

// Manifest of clips to load. Adding a new clip means appending a row here
// AND adding the matching VOICE_CLIP_* enum value in the header.
struct ClipSpec {
  voice_clip_id_t id;
  const char* path;
  const char* name;
};

constexpr ClipSpec kClipManifest[] = {
    {VOICE_CLIP_WIFI_CONNECTED,      "/spiffs/audio/WifiConnected.mp3",      "WifiConnected.mp3"},
    {VOICE_CLIP_WIFI_DISCONNECTED,   "/spiffs/audio/WifiDisconnected.mp3",   "WifiDisconnected.mp3"},
    {VOICE_CLIP_BLUETOOTH_MODE,      "/spiffs/audio/BluetoothMode.mp3",      "BluetoothMode.mp3"},
    {VOICE_CLIP_SNAPCAST_MODE,       "/spiffs/audio/SnapcastMode.mp3",       "SnapcastMode.mp3"},
    {VOICE_CLIP_DLNA_MODE,           "/spiffs/audio/DLNAMode.mp3",           "DLNAMode.mp3"},
    {VOICE_CLIP_BLUETOOTH_CONNECTED, "/spiffs/audio/BluetoothConnected.mp3", "BluetoothConnected.mp3"},
    {VOICE_CLIP_PAIRING,             "/spiffs/audio/Pairing.mp3",            "Pairing.mp3"},
};
static_assert(sizeof(kClipManifest) / sizeof(kClipManifest[0]) == (size_t)VOICE_CLIP_COUNT_,
              "kClipManifest out of sync with voice_clip_id_t");

// Cache table. `pcm == nullptr` means the clip is unavailable (file missing,
// decode failed, or init not yet run).
VoiceClip s_clips[VOICE_CLIP_COUNT_]{};
bool s_init_done = false;
size_t s_total_psram_bytes = 0;

// Append a chunk of stereo-s16 PCM (already at the engine sample rate) to
// `*buf` of capacity `*cap_bytes`, growing via realloc when needed. Returns
// true on success, false on PSRAM exhaustion.
bool appendPcm(int16_t** buf, size_t* size_bytes, size_t* cap_bytes,
               const int16_t* src, size_t src_bytes) {
  if (*size_bytes + src_bytes > *cap_bytes) {
    size_t new_cap = (*cap_bytes) ? (*cap_bytes) * 2 : kInitialClipBytes;
    while (new_cap < *size_bytes + src_bytes) new_cap *= 2;
    // realloc may relocate; PSRAM realloc is supported via heap_caps_realloc.
    int16_t* new_buf = (int16_t*)heap_caps_realloc(*buf, new_cap, MALLOC_CAP_SPIRAM);
    if (!new_buf) {
      ESP_LOGE(TAG, "PSRAM realloc to %u bytes failed", (unsigned)new_cap);
      return false;
    }
    *buf = new_buf;
    *cap_bytes = new_cap;
  }
  memcpy(reinterpret_cast<uint8_t*>(*buf) + *size_bytes, src, src_bytes);
  *size_bytes += src_bytes;
  return true;
}

// Linear-interp resample + mono->stereo expansion. Mirrors the streamer's
// `feedFrame` strategy; see comments there. We deliberately don't share
// code with the streamer — the cache resampler runs once at boot, the
// streamer's runs per HTTP frame at music rate, and tying their lifetimes
// together complicates Phase 3 (when the streamer becomes a producer that
// can be stopped).
//
// Writes to `*buf` via `appendPcm`. Returns false on PSRAM exhaustion.
bool resampleAndAppend(const int16_t* src, const MP3FrameInfo& info,
                       int16_t** buf, size_t* size_bytes, size_t* cap_bytes) {
  const int src_sr = info.samprate > 0 ? info.samprate : 44100;
  const int src_ch = info.nChans;
  const int frames = info.outputSamps / src_ch;
  const int dst_sr = geelen::SAMPLE_RATE;

  // Fast path: native rate + already stereo.
  if (src_sr == dst_sr && src_ch == 2) {
    return appendPcm(buf, size_bytes, cap_bytes, src,
                     (size_t)frames * 2 * sizeof(int16_t));
  }

  const uint32_t step = (uint32_t)(((uint64_t)src_sr << 16) / (uint64_t)dst_sr);
  const int dst_frames = (int)(((uint64_t)frames * dst_sr) / src_sr);

  constexpr int kChunk = 256;
  int16_t tmp[kChunk * 2];
  int remaining = dst_frames;
  uint32_t pos = 0;
  while (remaining > 0) {
    const int n = remaining < kChunk ? remaining : kChunk;
    for (int i = 0; i < n; ++i) {
      const uint32_t idx = pos >> 16;
      const uint32_t frac = pos & 0xFFFF;
      int32_t l0, r0, l1, r1;
      if (src_ch == 2) {
        l0 = src[2 * idx + 0];
        r0 = src[2 * idx + 1];
        if ((int)idx + 1 < frames) {
          l1 = src[2 * (idx + 1) + 0];
          r1 = src[2 * (idx + 1) + 1];
        } else { l1 = l0; r1 = r0; }
      } else {
        l0 = src[idx];
        r0 = l0;
        if ((int)idx + 1 < frames) { l1 = src[idx + 1]; r1 = l1; }
        else { l1 = l0; r1 = r0; }
      }
      const int32_t l = l0 + (((l1 - l0) * (int32_t)frac) >> 16);
      const int32_t r = r0 + (((r1 - r0) * (int32_t)frac) >> 16);
      tmp[2 * i + 0] = (int16_t)l;
      tmp[2 * i + 1] = (int16_t)r;
      pos += step;
    }
    if (!appendPcm(buf, size_bytes, cap_bytes,
                   tmp, (size_t)n * 2 * sizeof(int16_t))) {
      return false;
    }
    remaining -= n;
  }
  return true;
}

// Decode a single MP3 file fully into `out_clip`. Returns true on success;
// on failure logs a warning, frees any partial PSRAM allocation, and
// leaves `out_clip->pcm == nullptr`.
bool decodeOneClip(const ClipSpec& spec, VoiceClip* out_clip,
                   uint8_t* in_buf, int16_t* out_buf, HMP3Decoder decoder) {
  out_clip->name = spec.name;
  out_clip->id = spec.id;
  out_clip->pcm = nullptr;
  out_clip->frame_count = 0;
  out_clip->source_sr = 0;
  out_clip->source_channels = 0;

  struct stat st{};
  if (stat(spec.path, &st) != 0) {
    ESP_LOGW(TAG, "missing: %s", spec.path);
    return false;
  }

  FILE* f = fopen(spec.path, "rb");
  if (!f) {
    ESP_LOGW(TAG, "fopen failed: %s", spec.path);
    return false;
  }

  int16_t* clip_pcm = nullptr;
  size_t clip_bytes = 0;
  size_t clip_cap = 0;

  size_t in_fill = 0;
  bool eof = false;
  bool ok = true;
  uint32_t source_sr = 0;
  uint8_t source_channels = 0;

  // Streamer-style fill+sync+decode loop, but synchronous and bounded by
  // the file size (no socket, no back-pressure). Each iteration produces
  // at most one frame.
  while (true) {
    if (!eof && in_fill < 2048) {
      size_t n = fread(in_buf + in_fill, 1, kInBufBytes - in_fill, f);
      if (n == 0) {
        eof = true;
      } else {
        in_fill += n;
      }
    }
    if (eof && in_fill < 64) break;

    int sync = MP3FindSyncWord(in_buf, in_fill);
    if (sync < 0) {
      // No sync in whole buffer — slide last 512 bytes forward and refill.
      if (in_fill > 512) {
        memmove(in_buf, in_buf + in_fill - 512, 512);
        in_fill = 512;
      } else {
        in_fill = 0;
      }
      continue;
    }
    if (sync > 0) {
      memmove(in_buf, in_buf + sync, in_fill - sync);
      in_fill -= sync;
    }

    unsigned char* ptr = in_buf;
    int bytes_left = (int)in_fill;
    int ret = MP3Decode(decoder, &ptr, &bytes_left, out_buf, 0);
    if (ret != ERR_MP3_NONE) {
      // Skip 1 byte and resync.
      if (in_fill >= 1) {
        memmove(in_buf, in_buf + 1, in_fill - 1);
        in_fill -= 1;
      }
      continue;
    }
    const size_t consumed = in_fill - (size_t)bytes_left;
    memmove(in_buf, in_buf + consumed, bytes_left);
    in_fill = (size_t)bytes_left;

    MP3FrameInfo info{};
    MP3GetLastFrameInfo(decoder, &info);
    if (info.outputSamps <= 0 || info.nChans < 1 || info.nChans > 2) continue;

    if (source_sr == 0) {
      source_sr = (uint32_t)(info.samprate > 0 ? info.samprate : 44100);
      source_channels = (uint8_t)info.nChans;
    }
    if (!resampleAndAppend(out_buf, info, &clip_pcm, &clip_bytes, &clip_cap)) {
      ok = false;
      break;
    }
  }

  fclose(f);

  if (!ok || clip_pcm == nullptr || clip_bytes == 0) {
    if (clip_pcm) heap_caps_free(clip_pcm);
    ESP_LOGW(TAG, "decode failed or empty: %s", spec.path);
    return false;
  }

  // Trim the PSRAM allocation to the exact size to avoid carrying the
  // realloc-rounded slack for the rest of the firmware lifetime. The shrink
  // is in-place when possible.
  if (clip_cap > clip_bytes) {
    int16_t* shrunk =
        (int16_t*)heap_caps_realloc(clip_pcm, clip_bytes, MALLOC_CAP_SPIRAM);
    if (shrunk) {
      clip_pcm = shrunk;
      clip_cap = clip_bytes;
    }
  }

  out_clip->pcm = clip_pcm;
  out_clip->frame_count = clip_bytes / (2 * sizeof(int16_t));
  out_clip->source_sr = source_sr;
  out_clip->source_channels = source_channels;
  s_total_psram_bytes += clip_bytes;

  ESP_LOGI(TAG, "loaded %s: %u frames (%.2f s), src=%u Hz/%u ch -> 44.1 kHz stereo, %u bytes PSRAM",
           spec.name, (unsigned)out_clip->frame_count,
           (double)out_clip->frame_count / (double)geelen::SAMPLE_RATE,
           (unsigned)source_sr, (unsigned)source_channels, (unsigned)clip_bytes);
  return true;
}

}  // namespace

bool voiceCacheInit() {
  if (s_init_done) return true;

  // Allocate scratch buffers used by every clip; freed before we return.
  // Uses MALLOC_CAP_8BIT so they end up in internal DRAM (Helix's pointer
  // arithmetic is happier without PSRAM cache-line surprises).
  uint8_t* in_buf = (uint8_t*)heap_caps_malloc(kInBufBytes, MALLOC_CAP_8BIT);
  int16_t* out_buf =
      (int16_t*)heap_caps_malloc(kOutBufSamples * sizeof(int16_t), MALLOC_CAP_8BIT);
  HMP3Decoder decoder = MP3InitDecoder();
  if (!in_buf || !out_buf || !decoder) {
    ESP_LOGE(TAG, "scratch alloc failed (in=%p out=%p dec=%p)", in_buf, out_buf, decoder);
    if (in_buf) heap_caps_free(in_buf);
    if (out_buf) heap_caps_free(out_buf);
    if (decoder) MP3FreeDecoder(decoder);
    return false;
  }

  bool all_ok = true;
  for (const ClipSpec& spec : kClipManifest) {
    // Fresh decoder state per clip — calling MP3Decode on a brand-new file
    // without resetting can leak Bit Reservoir state across clip boundaries.
    MP3FreeDecoder(decoder);
    decoder = MP3InitDecoder();
    if (!decoder) {
      ESP_LOGE(TAG, "MP3InitDecoder failed mid-loop; aborting cache init");
      all_ok = false;
      break;
    }
    if (!decodeOneClip(spec, &s_clips[spec.id], in_buf, out_buf, decoder)) {
      all_ok = false;
      // Continue with remaining clips — a missing clip is non-fatal; the
      // voice task simply skips it at runtime.
    }
  }

  heap_caps_free(in_buf);
  heap_caps_free(out_buf);
  if (decoder) MP3FreeDecoder(decoder);

  s_init_done = true;
  ESP_LOGI(TAG, "voice cache init complete: %u bytes PSRAM across %d slot(s)",
           (unsigned)s_total_psram_bytes, (int)VOICE_CLIP_COUNT_);
  return all_ok;
}

const VoiceClip* voiceCacheGet(voice_clip_id_t id) {
  if (!s_init_done) return nullptr;
  if ((int)id < 0 || (int)id >= (int)VOICE_CLIP_COUNT_) return nullptr;
  const VoiceClip* c = &s_clips[id];
  return (c->pcm && c->frame_count > 0) ? c : nullptr;
}

size_t voiceCacheTotalPsramBytes() { return s_total_psram_bytes; }

}  // namespace geelen_idf

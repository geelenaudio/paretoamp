// HTTP + MP3 → AudioEngine pipeline used by the native DLNA renderer.
//
// Usage (from the DLNA SOAP layer):
//
//   auto* s = geelen_idf::mp3::streamer();
//   s->setEngine(&g_engine);
//   s->play("http://10.0.0.5:8200/MediaItems/42.mp3");
//   ...
//   s->pause();   // halts HTTP reads + decode, keeps position cached
//   s->resume();  // re-opens the HTTP connection with a Range header
//   s->stop();    // closes connection and drops the URI
//
// The decoder uses `libhelix-mp3` (chmorgan/esp-libhelix-mp3). Helix
// decodes one MPEG frame per call, giving us predictable 50–70 ms worst
// case stack usage per invocation.
//
// Runtime task graph:
//
//   mp3_task (prio 5, core 1):
//     open TCP socket -> fetch bytes -> ring buffer -> MP3Decode loop
//       -> resample (if ≠ 44.1 kHz) -> AudioEngine::feedPCM -> repeat
//
// Resampling is a tiny fixed-ratio linear interpolator for 48 k and
// 32 k sources (the two common DLNA server outputs besides 44.1). For
// anything else we log a warning and play at 44.1 regardless (slight
// pitch shift but never silence).
//
// The streamer intentionally does not care about DLNA semantics: it
// only knows how to play/pause/stop a URI. DLNA-specific state (track
// metadata, seek timeline, UPnP event vars) lives in idf_dlna.

#pragma once

#include <cstdint>

namespace geelen { class AudioEngine; }

namespace geelen_idf {
namespace mp3 {

class Streamer {
 public:
  void setEngine(geelen::AudioEngine* engine);

  // Kick off playback of `url`. Spawns the fetch/decode task on first
  // call; subsequent calls swap the URL atomically. `url` must be
  // `http://...` or an absolute VFS path (`/spiffs/...`).
  bool play(const char* url);

  /** SPIFFS / local file (path must start with `/`). */
  bool playFile(const char* path);

  // Pause/resume. Pause closes the socket but keeps `url_` so resume
  // can reopen at the cached byte offset via a Range: header. Not all
  // DLNA servers honour Range, in which case resume starts from zero.
  void pause();
  void resume();

  // Cancel current playback. Drops URL + closes socket + kills the
  // decoder state. The task stays alive, waiting for the next play().
  void stop();

  bool isPlaying() const;
  /** True while a queued or active SPIFFS MP3 (`/…`) owns the voice path — A2DP must not feed the ring. */
  bool isPlayingLocalFile() const;
  uint32_t positionMs() const;
  uint32_t durationMs() const;
  void setDurationMs(uint32_t ms);
};

// Lazy-initialised process-wide singleton. Safe to call before WiFi.
Streamer* streamer();

}  // namespace mp3
}  // namespace geelen_idf

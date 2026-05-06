// See idf_mp3_streamer.h for the design. This file focuses on the
// network + decoder loop; it deliberately has no DLNA-specific code.
//
// Key invariants:
//   - Only one instance (process-wide singleton). If a second play()
//     comes in while we're already streaming, we drop the old socket
//     and replace the URL atomically.
//   - The HTTP GET code is minimal on purpose. DLNA servers tend to
//     serve plain static ranges with no chunked encoding and no TLS,
//     so a bespoke socket is cheaper than pulling in esp_http_client.
//   - All state mutations live behind s_mutex. The decode loop only
//     reads the state after copying the URL snapshot, so it can detect
//     a stop() mid-frame without racing.

#include "idf_mp3_streamer.h"

#include "audio_engine.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

extern "C" {
#include "mp3dec.h"
}

namespace geelen_idf {
namespace mp3 {

namespace {

constexpr const char* TAG = "dlna-mp3";

// Biggest frame is layer III, 48 kHz, free-format: ~2880 bytes. Give
// ourselves a comfortable 8 KiB so the sync+frame fit even after a
// partial read.
constexpr size_t kInBufBytes = 8 * 1024;

// libhelix outputs at most 1152 samples/frame × 2 channels = 2304 s16.
constexpr size_t kOutBufSamples = 2304;

struct Command {
  enum class Kind { None, Play, Pause, Resume, Stop };
  Kind kind{Kind::None};
  char url[256]{};
};

class StreamerImpl : public Streamer {
 public:
  StreamerImpl() {
    mutex_ = xSemaphoreCreateMutex();
  }

  void setEngineInternal(geelen::AudioEngine* eng) { engine_ = eng; }

  void ensureTask() {
    if (task_started_.exchange(true)) return;
    xTaskCreatePinnedToCore(&StreamerImpl::taskTrampoline, "dlna-mp3",
                            8192, this, 5, nullptr, 1);
  }

  bool playImpl(const char* url) {
    if (!url) return false;
    if (strncmp(url, "http://", 7) != 0 && url[0] != '/') {
      ESP_LOGW(TAG, "play: need http:// or absolute path: %s", url);
      return false;
    }
    ensureTask();
    xSemaphoreTake(mutex_, portMAX_DELAY);
    cmd_.kind = Command::Kind::Play;
    strlcpy(cmd_.url, url, sizeof(cmd_.url));
    playing_.store(true);
    position_ms_.store(0);
    // Gate A2DP before the decode task runs so SBC cannot splice into voice PCM.
    if (url[0] == '/') {
      local_mp3_gate_.fetch_add(1, std::memory_order_relaxed);
    }
    xSemaphoreGive(mutex_);
    return true;
  }

  void pauseImpl() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    cmd_.kind = Command::Kind::Pause;
    playing_.store(false);
    xSemaphoreGive(mutex_);
  }
  void resumeImpl() {
    ensureTask();
    xSemaphoreTake(mutex_, portMAX_DELAY);
    cmd_.kind = Command::Kind::Resume;
    playing_.store(true);
    xSemaphoreGive(mutex_);
  }
  void stopImpl() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    cmd_.kind = Command::Kind::Stop;
    playing_.store(false);
    cmd_.url[0] = 0;
    xSemaphoreGive(mutex_);
  }

  bool isPlayingImpl() const { return playing_.load(); }
  bool isPlayingLocalFileImpl() const { return local_mp3_gate_.load(std::memory_order_relaxed) > 0; }
  uint32_t positionMsImpl() const { return position_ms_.load(); }
  uint32_t durationMsImpl() const { return duration_ms_.load(); }
  void setDurationMsImpl(uint32_t ms) { duration_ms_.store(ms); }

  /** After closing a SPIFFS decode session, drain ring tail then release A2DP/Snap gate. */
  void drainTailAndReleaseLocalGate() {
    if (!engine_) {
      local_mp3_gate_.fetch_sub(1, std::memory_order_relaxed);
      return;
    }
    // Stay above `AUDIO_LOW_WATER_MS` while draining or `process()` flips to
    // rebuffer and returns silence (heard as missing boot / first-play audio).
    constexpr size_t kTailFrames =
        (size_t)((uint64_t)geelen::SAMPLE_RATE * (geelen::AUDIO_LOW_WATER_MS + 40) / 1000);
    for (int i = 0; i < 800; ++i) {
      if (engine_->bufferedFrames() <= kTailFrames) break;
      vTaskDelay(pdMS_TO_TICKS(15));
    }
    local_mp3_gate_.fetch_sub(1, std::memory_order_relaxed);
  }

 private:
  static void taskTrampoline(void* arg) {
    static_cast<StreamerImpl*>(arg)->taskLoop();
  }

  // Closes socket / file and resets per-stream state.
  void closeStream() {
    if (sock_ >= 0) {
      close(sock_);
      sock_ = -1;
    }
    if (file_) {
      fclose(file_);
      file_ = nullptr;
    }
    in_fill_ = 0;
    byte_offset_ = 0;
    eof_ = false;
    if (decoder_) {
      MP3FreeDecoder(decoder_);
      decoder_ = nullptr;
    }
  }

  // Open HTTP connection. URL shape: http://host[:port]/path. On return
  // sock_ is connected and HTTP headers have been consumed; the first
  // MP3 frame starts at the *next* byte on the socket.
  bool openUrl(const char* url, uint32_t start_offset) {
    // Parse URL.
    const char* rest = url + 7;  // skip "http://"
    const char* slash = strchr(rest, '/');
    char host[128];
    char path[256];
    int port = 80;
    if (slash) {
      size_t hlen = (size_t)(slash - rest);
      if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
      memcpy(host, rest, hlen);
      host[hlen] = 0;
      strlcpy(path, slash, sizeof(path));
    } else {
      strlcpy(host, rest, sizeof(host));
      strlcpy(path, "/", sizeof(path));
    }
    // Host:port split.
    char* colon = strchr(host, ':');
    if (colon) {
      *colon = 0;
      port = atoi(colon + 1);
      if (port <= 0 || port > 65535) port = 80;
    }

    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char portstr[8]; snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
      ESP_LOGW(TAG, "getaddrinfo failed for %s:%d", host, port);
      return false;
    }
    sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock_ < 0) { freeaddrinfo(res); return false; }
    timeval tv{}; tv.tv_sec = 5;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(sock_, res->ai_addr, res->ai_addrlen) != 0) {
      ESP_LOGW(TAG, "connect() to %s:%d failed", host, port);
      freeaddrinfo(res); close(sock_); sock_ = -1; return false;
    }
    freeaddrinfo(res);

    char req[512];
    int n;
    if (start_offset > 0) {
      n = snprintf(req, sizeof(req),
                   "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Geelen/DLNA\r\n"
                   "Range: bytes=%lu-\r\nConnection: close\r\n\r\n",
                   path, host, (unsigned long)start_offset);
    } else {
      n = snprintf(req, sizeof(req),
                   "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Geelen/DLNA\r\n"
                   "Connection: close\r\n\r\n",
                   path, host);
    }
    if (send(sock_, req, n, 0) != n) {
      close(sock_); sock_ = -1; return false;
    }

    // Slurp headers up to \r\n\r\n.
    char hdr[1024];
    size_t hlen = 0;
    while (hlen < sizeof(hdr) - 1) {
      int r = recv(sock_, hdr + hlen, 1, 0);
      if (r <= 0) { close(sock_); sock_ = -1; return false; }
      hlen += r;
      if (hlen >= 4 && memcmp(hdr + hlen - 4, "\r\n\r\n", 4) == 0) break;
    }
    hdr[hlen] = 0;
    // Loosely parse Content-Length so we can log duration estimates.
    const char* cl = strcasestr(hdr, "Content-Length:");
    if (cl) {
      content_length_ = strtoul(cl + 15, nullptr, 10) + start_offset;
    } else {
      content_length_ = 0;
    }
    byte_offset_ = start_offset;
    return true;
  }

  bool openFile(const char* path) {
    file_ = fopen(path, "rb");
    if (!file_) {
      ESP_LOGW(TAG, "fopen failed: %s", path ? path : "(null)");
      return false;
    }
    content_length_ = 0;
    if (byte_offset_ > 0) {
      if (fseek(file_, (long)byte_offset_, SEEK_SET) != 0) {
        ESP_LOGW(TAG, "fseek failed offset=%lu", (unsigned long)byte_offset_);
        fclose(file_);
        file_ = nullptr;
        return false;
      }
    }
    return true;
  }

  // Drive one decode cycle: fill buffer from socket if needed, decode
  // one frame, push to engine. Returns false if the stream ended.
  bool pumpOnce() {
    // Back-pressure: Helix decode runs several × realtime, so for short SPIFFS
    // clips the producer would race ahead and `feedPCM` would silently drop
    // samples once the engine ring fills (audible as the tail of a clip
    // being cut off — e.g. "Bluetooth" without "mode"). Pace the decoder
    // to roughly realtime by waiting whenever the engine has a healthy
    // buffer ahead.
    //
    // CRITICAL: this threshold must sit ABOVE the engine's prefill mark
    // (`AUDIO_PREFILL_MS` in audio_engine.h). Otherwise the audio task waits
    // for more buffered PCM than we ever allow the decoder to accumulate —
    // classic deadlock, silent boot prompts until BT music fills the ring.
    // Ring capacity is ~2 s; 1100 ms stays above a 500–1000 ms prefill with
    // margin below the cap.
    if (engine_) {
      constexpr size_t kHighWaterFrames =
          (size_t)((uint64_t)geelen::SAMPLE_RATE * 1100 / 1000);  // ~1.1 s
      while (engine_->bufferedFrames() > kHighWaterFrames) {
        vTaskDelay(pdMS_TO_TICKS(20));
      }
    }
    // Fill input buffer if low. Track EOF separately so we can keep decoding
    // whatever frames are already buffered after the source dries up — Helix
    // frames are up to ~1.7 KB each and we keep up to 2 KB of slack, so a
    // hard `return false` on the first read of zero bytes was discarding the
    // last ~100–200 ms of every clip (audible as "Bluetooth" instead of
    // "Bluetooth Mode", and a snipped tail on every other prompt).
    if (!eof_ && in_fill_ < 2048) {
      if (file_) {
        size_t n = fread(in_buf_ + in_fill_, 1, kInBufBytes - in_fill_, file_);
        if (n == 0) {
          eof_ = true;
        } else {
          in_fill_ += n;
          byte_offset_ += (uint32_t)n;
        }
      } else if (sock_ >= 0) {
        int r = recv(sock_, in_buf_ + in_fill_, kInBufBytes - in_fill_, 0);
        if (r <= 0) {
          eof_ = true;
        } else {
          in_fill_ += (size_t)r;
          byte_offset_ += (uint32_t)r;
        }
      } else {
        return false;
      }
    }
    // Once the source is drained AND the buffer is too small to hold even a
    // minimal MP3 frame, the stream is genuinely done.
    if (eof_ && in_fill_ < 64) return false;

    int sync = MP3FindSyncWord(in_buf_, in_fill_);
    if (sync < 0) {
      // No sync in whole buffer — shift last 512 bytes forward and refill.
      if (in_fill_ > 512) {
        memmove(in_buf_, in_buf_ + in_fill_ - 512, 512);
        in_fill_ = 512;
      } else {
        in_fill_ = 0;
      }
      return true;
    }
    if (sync > 0) {
      memmove(in_buf_, in_buf_ + sync, in_fill_ - sync);
      in_fill_ -= sync;
    }

    unsigned char* ptr = in_buf_;
    int bytes_left = (int)in_fill_;
    int ret = MP3Decode(decoder_, &ptr, &bytes_left, out_buf_, 0);
    if (ret != ERR_MP3_NONE) {
      // Skip 1 byte and resync.
      if (in_fill_ >= 1) {
        memmove(in_buf_, in_buf_ + 1, in_fill_ - 1);
        in_fill_ -= 1;
      }
      return true;
    }
    size_t consumed = in_fill_ - (size_t)bytes_left;
    memmove(in_buf_, in_buf_ + consumed, bytes_left);
    in_fill_ = (size_t)bytes_left;

    MP3FrameInfo info{};
    MP3GetLastFrameInfo(decoder_, &info);
    if (info.outputSamps <= 0 || info.nChans < 1 || info.nChans > 2) return true;

    // Advance position timeline (samples → ms at source sample rate).
    uint32_t frame_ms = (uint32_t)((info.outputSamps / info.nChans) * 1000ULL /
                                   (info.samprate ? info.samprate : 44100));
    position_ms_.fetch_add(frame_ms);

    // Stereoize mono and resample to 44.1 k (linear, fixed ratio).
    feedFrame(out_buf_, info);
    return true;
  }

  // Linear resample + channel expansion, writing int16 stereo frames to
  // the engine. Intentionally low-fi; Spotify Connect / Cast users who
  // care about audiophile resampling will use Snapcast anyway.
  void feedFrame(const int16_t* src, const MP3FrameInfo& info) {
    const int src_sr = info.samprate > 0 ? info.samprate : 44100;
    const int src_ch = info.nChans;
    const int frames = info.outputSamps / src_ch;
    const int dst_sr = 44100;

    // Fast path: 44.1 kHz stereo — just hand the buffer through.
    if (src_sr == dst_sr && src_ch == 2) {
      if (engine_) {
        engine_->feedPCM(reinterpret_cast<const uint8_t*>(src),
                         frames * 4);
      }
      return;
    }

    // Otherwise: walk the source in Q16 fractional steps. step = src_sr
    // / dst_sr in Q16. For each destination frame we pick the floor
    // source frame and linear-blend with the next.
    const uint32_t step = (uint32_t)(((uint64_t)src_sr << 16) / dst_sr);
    const int dst_frames = (int)(((uint64_t)frames * dst_sr) / src_sr);

    // Stage in chunks of 256 frames to avoid a multi-KB stack alloc.
    constexpr int kChunk = 256;
    int16_t tmp[kChunk * 2];
    int remaining = dst_frames;
    uint32_t pos = 0;  // Q16.16 fractional index into src
    while (remaining > 0) {
      int n = remaining < kChunk ? remaining : kChunk;
      for (int i = 0; i < n; ++i) {
        uint32_t idx = pos >> 16;
        uint32_t frac = pos & 0xFFFF;
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
        int32_t l = l0 + (((l1 - l0) * (int32_t)frac) >> 16);
        int32_t r = r0 + (((r1 - r0) * (int32_t)frac) >> 16);
        tmp[2 * i + 0] = (int16_t)l;
        tmp[2 * i + 1] = (int16_t)r;
        pos += step;
      }
      if (engine_) {
        engine_->feedPCM(reinterpret_cast<const uint8_t*>(tmp), n * 4);
      }
      remaining -= n;
    }
  }

  // Main task body.
  void taskLoop() {
    in_buf_ = (unsigned char*)heap_caps_malloc(kInBufBytes, MALLOC_CAP_8BIT);
    out_buf_ = (int16_t*)heap_caps_malloc(kOutBufSamples * sizeof(int16_t),
                                          MALLOC_CAP_8BIT);
    if (!in_buf_ || !out_buf_) {
      ESP_LOGE(TAG, "out of memory for buffers");
      vTaskDelete(nullptr);
    }

    while (true) {
      Command c;
      xSemaphoreTake(mutex_, portMAX_DELAY);
      c = cmd_;
      cmd_.kind = Command::Kind::None;
      xSemaphoreGive(mutex_);

      switch (c.kind) {
        case Command::Kind::Play: {
          const bool replace_had_local = (file_ != nullptr);
          closeStream();
          if (replace_had_local) drainTailAndReleaseLocalGate();
          strlcpy(current_url_, c.url, sizeof(current_url_));
          if (strncmp(c.url, "http://", 7) == 0) {
            if (!openUrl(current_url_, 0)) break;
          } else if (c.url[0] == '/') {
            if (!openFile(current_url_)) {
              local_mp3_gate_.fetch_sub(1, std::memory_order_relaxed);
              break;
            }
          } else {
            break;
          }
          decoder_ = MP3InitDecoder();
          break;
        }
        case Command::Kind::Resume: {
          if (!decoder_ && current_url_[0]) {
            if (strncmp(current_url_, "http://", 7) == 0) {
              if (!openUrl(current_url_, byte_offset_)) break;
            } else if (current_url_[0] == '/') {
              if (!openFile(current_url_)) break;
            } else {
              break;
            }
            decoder_ = MP3InitDecoder();
          }
          break;
        }
        case Command::Kind::Pause:
        case Command::Kind::Stop: {
          const bool stop_had_local = (file_ != nullptr);
          closeStream();
          if (stop_had_local) drainTailAndReleaseLocalGate();
          if (c.kind == Command::Kind::Stop) {
            current_url_[0] = 0;
            position_ms_.store(0);
          }
          break;
        }
        case Command::Kind::None: break;
      }

      if (playing_.load() && decoder_ && (sock_ >= 0 || file_)) {
        if (!pumpOnce()) {
          ESP_LOGI(TAG, "stream ended");
          const bool ended_local = (file_ != nullptr);
          closeStream();
          if (ended_local) drainTailAndReleaseLocalGate();
          playing_.store(false);
          position_ms_.store(0);
        }
      } else {
        vTaskDelay(pdMS_TO_TICKS(20));
      }
    }
  }

  SemaphoreHandle_t mutex_{nullptr};
  Command cmd_{};
  std::atomic<bool> task_started_{false};
  std::atomic<bool> playing_{false};
  /** A2DP gate: incremented when a `/…` play is queued, decremented when that file is closed. */
  std::atomic<int> local_mp3_gate_{0};
  std::atomic<uint32_t> position_ms_{0};
  std::atomic<uint32_t> duration_ms_{0};

  char current_url_[256]{};
  int sock_{-1};
  FILE* file_{nullptr};
  unsigned char* in_buf_{nullptr};
  size_t in_fill_{0};
  int16_t* out_buf_{nullptr};
  HMP3Decoder decoder_{nullptr};
  uint32_t byte_offset_{0};
  uint32_t content_length_{0};
  /** Set when the underlying source (file or socket) has no more bytes to
   *  yield; we then keep decoding from `in_buf_` until it is exhausted. */
  bool eof_{false};

  geelen::AudioEngine* engine_{nullptr};
};

StreamerImpl& instance() {
  static StreamerImpl inst;
  return inst;
}

}  // namespace

// Public shim: forward to the singleton via static_cast to avoid virtuals.
void Streamer::setEngine(geelen::AudioEngine* eng) {
  static_cast<StreamerImpl*>(this)->setEngineInternal(eng);
}
bool Streamer::play(const char* url)       { return static_cast<StreamerImpl*>(this)->playImpl(url); }
bool Streamer::playFile(const char* path) {
  if (!path || path[0] != '/') return false;
  return static_cast<StreamerImpl*>(this)->playImpl(path);
}
void Streamer::pause()                     { static_cast<StreamerImpl*>(this)->pauseImpl(); }
void Streamer::resume()                    { static_cast<StreamerImpl*>(this)->resumeImpl(); }
void Streamer::stop()                      { static_cast<StreamerImpl*>(this)->stopImpl(); }
bool Streamer::isPlaying() const           { return static_cast<const StreamerImpl*>(this)->isPlayingImpl(); }
bool Streamer::isPlayingLocalFile() const {
  return static_cast<const StreamerImpl*>(this)->isPlayingLocalFileImpl();
}
uint32_t Streamer::positionMs() const      { return static_cast<const StreamerImpl*>(this)->positionMsImpl(); }
uint32_t Streamer::durationMs() const      { return static_cast<const StreamerImpl*>(this)->durationMsImpl(); }
void Streamer::setDurationMs(uint32_t ms)  { static_cast<StreamerImpl*>(this)->setDurationMsImpl(ms); }

Streamer* streamer() {
  return &instance();
}

}  // namespace mp3
}  // namespace geelen_idf

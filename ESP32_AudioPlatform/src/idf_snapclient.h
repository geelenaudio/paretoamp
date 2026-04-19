// ESP-IDF Snapcast client (TCP) feeding PCM into AudioEngine.

#pragma once

#include <cstdint>
#include <cstddef>

namespace geelen {
class AudioEngine;
}

namespace geelen_idf {

class Snapclient {
 public:
  Snapclient() = default;
  void setEngine(geelen::AudioEngine* eng) { engine_ = eng; }

  // Open a TCP connection to the snapserver STREAM port (default 1704) and
  // send a Hello frame. Without the Hello, the server never sends
  // ServerSettings / CodecHeader / WireChunks and the stream stays silent.
  bool connect(const char* host, uint16_t port);
  void disconnect();
  void tick();
  bool isConnected() const { return connected_; }

 private:
  // Send a Snapcast Hello (message_type::kHello = 5). Called once per
  // successful connect() so the server registers us as a client.
  bool sendHello();

  // Apply a ServerSettings (message_type::kServerSettings = 3) JSON payload.
  // Extracts `volume` (0..100) and `muted` (bool) and pushes them into the
  // AudioEngine so snapweb's per-client volume slider actually does something.
  // Silently ignores malformed JSON / unknown fields.
  void applyServerSettings(const char* json, size_t len);

  bool connected_{false};
  int sock_{-1};
  geelen::AudioEngine* engine_{nullptr};

  // Cached volume so muted->unmuted restores the slider value without needing
  // another ServerSettings message.
  uint8_t last_volume_{100};
  bool last_muted_{false};

  static constexpr size_t READ_BUF_SIZE = 2048;
  uint8_t buf_[READ_BUF_SIZE]{};
};

}  // namespace geelen_idf


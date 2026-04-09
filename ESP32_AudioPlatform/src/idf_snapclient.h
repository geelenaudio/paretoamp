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
  bool connect(const char* host, uint16_t port);
  void disconnect();
  void tick();
  bool isConnected() const { return connected_; }

 private:
  bool connected_{false};
  int sock_{-1};
  geelen::AudioEngine* engine_{nullptr};

  static constexpr uint16_t MSG_WIRE_CHUNK = 4;
  static constexpr size_t WIRE_CHUNK_TS_SIZE = 8;
  static constexpr size_t READ_BUF_SIZE = 2048;
  uint8_t buf_[READ_BUF_SIZE]{};
};

}  // namespace geelen_idf


/**
 * Minimal Snapcast client – connect to snapserver, parse WireChunk, feed PCM to engine.
 * Protocol: base header (26 bytes) then payload; type 4 = WireChunk (8-byte timestamp + PCM).
 */

#ifndef GEELEN_SNAPCLIENT_H
#define GEELEN_SNAPCLIENT_H

#include <cstdint>
#include <cstddef>
#include <WiFi.h>

namespace geelen {

class AudioEngine;

class Snapclient {
 public:
  Snapclient() : engine_(nullptr), connected_(false), read_pos_(0), read_need_(0),
                 state_(kIdle), payload_size_(0), msg_type_(0) {}

  void setEngine(AudioEngine* eng) { engine_ = eng; }
  bool connect(const char* host, uint16_t port);
  void disconnect();
  void tick();
  bool isConnected() const { return connected_; }

 private:
  bool readBaseHeader(uint16_t& type, uint32_t& payload_size);
  bool readPayload(uint16_t type, uint32_t payload_size);
  static const size_t BASE_HEADER_SIZE = 26;
  static const uint16_t MSG_WIRE_CHUNK = 4;
  static const size_t WIRE_CHUNK_TS_SIZE = 8;
  static const size_t READ_BUF_SIZE = 2048;

  WiFiClient client_;
  AudioEngine* engine_;
  bool connected_;
  uint8_t read_buf_[READ_BUF_SIZE];
  size_t read_pos_;
  size_t read_need_;
  enum { kIdle, kReadingHeader, kReadingPayload } state_;
  uint32_t payload_size_;
  uint16_t msg_type_;
};

}  // namespace geelen

#endif  // GEELEN_SNAPCLIENT_H

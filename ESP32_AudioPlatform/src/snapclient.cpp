/**
 * Minimal Snapcast client – TCP connect, binary protocol, feed PCM to engine.
 */

#include "snapclient.h"
#include "audio_engine.h"
#include "snapcast_protocol.h"
#include <Arduino.h>
#include <cstring>

namespace geelen {

bool Snapclient::connect(const char* host, uint16_t port) {
  disconnect();
  if (!host || host[0] == '\0') return false;
  if (!client_.connect(host, port, 5000)) return false;
  connected_ = true;
  state_ = kIdle;
  read_pos_ = 0;
  read_need_ = BASE_HEADER_SIZE;
  return true;
}

void Snapclient::disconnect() {
  client_.stop();
  connected_ = false;
  state_ = kIdle;
}

bool Snapclient::readBaseHeader(uint16_t& type, uint32_t& payload_size) {
  while (client_.available() && read_pos_ < BASE_HEADER_SIZE) {
    read_buf_[read_pos_++] = (uint8_t)client_.read();
  }
  if (read_pos_ < BASE_HEADER_SIZE) return false;
  bool ok = parseSnapcastBaseHeader(read_buf_, read_pos_, type, payload_size);
  read_pos_ = 0;
  return ok;
}

bool Snapclient::readPayload(uint16_t type, uint32_t payload_size) {
  if (payload_size > READ_BUF_SIZE) {
    for (uint32_t i = 0; i < payload_size && client_.available(); i++) client_.read();
    return true;
  }
  while (client_.available() && read_pos_ < payload_size) {
    read_buf_[read_pos_++] = (uint8_t)client_.read();
  }
  if (read_pos_ < payload_size) return false;
  if (type == MSG_WIRE_CHUNK && payload_size > WIRE_CHUNK_TS_SIZE && engine_) {
    size_t pcm_len = payload_size - WIRE_CHUNK_TS_SIZE;
    engine_->feedPCM(read_buf_ + WIRE_CHUNK_TS_SIZE, pcm_len);
  }
  read_pos_ = 0;
  return true;
}

void Snapclient::tick() {
  if (!connected_ || !client_.connected()) {
    if (connected_) disconnect();
    return;
  }
  if (state_ == kIdle) {
    read_need_ = BASE_HEADER_SIZE;
    read_pos_ = 0;
    state_ = kReadingHeader;
  }
  if (state_ == kReadingHeader) {
    if (readBaseHeader(msg_type_, payload_size_)) {
      state_ = kReadingPayload;
      read_pos_ = 0;
    }
  }
  if (state_ == kReadingPayload) {
    if (readPayload(msg_type_, payload_size_)) {
      state_ = kIdle;
    }
  }
}

}  // namespace geelen

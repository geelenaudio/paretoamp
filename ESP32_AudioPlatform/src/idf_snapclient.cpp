// ESP-IDF Snapcast client implementation.

#include "idf_snapclient.h"

#include "snapcast_protocol.h"
#include "audio_engine.h"

#include "esp_log.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <cstring>

namespace geelen_idf {

static const char* TAG = "Snap";

bool Snapclient::connect(const char* host, uint16_t port) {
  disconnect();
  if (!host || host[0] == '\0') return false;

  sock_ = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_ < 0) return false;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = inet_addr(host);
  if (addr.sin_addr.s_addr == INADDR_NONE) {
    ESP_LOGE(TAG, "Bad host IP: %s", host);
    disconnect();
    return false;
  }

  // 5s connect timeout
  timeval tv{};
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  if (::connect(sock_, (sockaddr*)&addr, sizeof(addr)) != 0) {
    ESP_LOGW(TAG, "Connect failed");
    disconnect();
    return false;
  }

  connected_ = true;
  ESP_LOGI(TAG, "Connected to snapserver %s:%u", host, (unsigned)port);
  return true;
}

void Snapclient::disconnect() {
  if (sock_ >= 0) {
    close(sock_);
    sock_ = -1;
  }
  connected_ = false;
}

void Snapclient::tick() {
  if (!connected_ || sock_ < 0) return;
  if (!engine_) return;

  // Read base header
  uint8_t hdr[geelen::SNAPCAST_BASE_HEADER_SIZE];
  int got = recv(sock_, hdr, sizeof(hdr), MSG_DONTWAIT);
  if (got == 0) { disconnect(); return; }
  if (got < 0) return;
  if (got != (int)sizeof(hdr)) {
    // For simplicity in this first pass, require full header in one go.
    // (We’ll make it incremental if needed.)
    return;
  }

  uint16_t type = 0;
  uint32_t payload_size = 0;
  if (!geelen::parseSnapcastBaseHeader(hdr, sizeof(hdr), type, payload_size)) return;

  // Drain payload
  uint32_t remaining = payload_size;
  while (remaining > 0) {
    size_t want = remaining > READ_BUF_SIZE ? READ_BUF_SIZE : remaining;
    int n = recv(sock_, buf_, want, 0);
    if (n <= 0) { disconnect(); return; }
    remaining -= (uint32_t)n;

    if (type == MSG_WIRE_CHUNK) {
      if ((uint32_t)n <= WIRE_CHUNK_TS_SIZE) continue;
      engine_->feedPCM(buf_ + WIRE_CHUNK_TS_SIZE, (size_t)n - WIRE_CHUNK_TS_SIZE);
    }
  }
}

}  // namespace geelen_idf


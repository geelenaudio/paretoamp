// ESP-IDF Snapcast client implementation.
//
// Protocol handshake: after the TCP connect we immediately send a Hello
// (message_type::kHello = 5). The server replies with ServerSettings,
// StreamTags, CodecHeader and then starts pushing WireChunks every ~20 ms.
// Without the Hello, the server logs only "New connection from …" and the
// stream stays silent.

#include "idf_snapclient.h"

#include "snapcast_protocol.h"
#include "audio_engine.h"
#include "idf_voice_feedback.h"

#include "esp_log.h"
#include "esp_mac.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"

#include "cJSON.h"

#include <cstdio>
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

  // Larger receive buffer = can absorb a WiFi retransmit burst (~100 ms) without
  // the TCP window closing. LwIP's compile-time default `tcp rx win: 5760` is
  // only ~33 ms of 44.1 kHz stereo S16 audio; 32 KiB gives us ~186 ms of
  // slack, which matches the I2S DMA queue (16 × 512 frames = 185 ms).
  int rcvbuf = 32 * 1024;
  setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  // Nagle off: snapserver pushes 20 ms WireChunks at wall-clock cadence;
  // batching them delays each chunk by up to 200 ms which shows up as ring
  // buffer undershoot on the ESP32 side.
  int one = 1;
  setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  // tick() uses BLOCKING recv() so the task sleeps between frames and LwIP
  // wakes it the instant bytes arrive (no 10 ms polling latency). The timeout
  // is just a safety net so the outer loop can notice an external
  // g_snap.disconnect() within ~1 s even if the server went radio-silent.
  timeval tv{};
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  tv.tv_sec = 5;
  setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  if (::connect(sock_, (sockaddr*)&addr, sizeof(addr)) != 0) {
    ESP_LOGW(TAG, "Connect failed");
    disconnect();
    return false;
  }

  if (!sendHello()) {
    ESP_LOGW(TAG, "Hello send failed; dropping connection");
    disconnect();
    return false;
  }

  connected_ = true;
  ESP_LOGI(TAG, "Connected to snapserver %s:%u (Hello sent)", host,
           (unsigned)port);
  return true;
}

bool Snapclient::sendHello() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char mac_str[18];
  std::snprintf(mac_str, sizeof(mac_str),
                "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5]);

  uint8_t frame[384];
  const size_t n = geelen::buildSnapcastHelloFrame(
      frame, sizeof(frame),
      mac_str,
      "geelen-amp",  // HostName (also used as the display name in snapweb)
      "Snapclient",  // ClientName (snapweb filters controls per client name)
      "0.1.0",       // Version
      "ESP-IDF",     // OS
      "xtensa",      // Arch
      1u);           // Instance
  if (n == 0) {
    ESP_LOGE(TAG, "buildSnapcastHelloFrame: scratch buffer too small");
    return false;
  }

  size_t sent_total = 0;
  while (sent_total < n) {
    const int sent = ::send(sock_, frame + sent_total, n - sent_total, 0);
    if (sent <= 0) {
      ESP_LOGW(TAG, "Hello send: errno=%d after %u/%u bytes", errno,
               (unsigned)sent_total, (unsigned)n);
      return false;
    }
    sent_total += (size_t)sent;
  }
  return true;
}

void Snapclient::disconnect() {
  if (sock_ >= 0) {
    close(sock_);
    sock_ = -1;
  }
  connected_ = false;
}

// Parse a ServerSettings JSON payload and push volume/mute into the engine.
// Payload shape (from badaix/snapcast common/message/server_settings.hpp):
//   {"bufferMs":<int>,"latency":<int>,"muted":<bool>,"volume":<0..100>}
// `latency` and `bufferMs` are for the server's own lip-sync / drift-
// correction code path — we don't have a TimeProvider so we ignore them.
void Snapclient::applyServerSettings(const char* json, size_t len) {
  if (!engine_ || !json || len == 0) return;

  cJSON* root = cJSON_ParseWithLength(json, len);
  if (!root) {
    ESP_LOGW(TAG, "ServerSettings: JSON parse failed (len=%u)", (unsigned)len);
    return;
  }

  const cJSON* vol = cJSON_GetObjectItemCaseSensitive(root, "volume");
  const cJSON* mut = cJSON_GetObjectItemCaseSensitive(root, "muted");

  if (cJSON_IsNumber(vol)) {
    int v = vol->valueint;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    last_volume_ = (uint8_t)v;
  }
  if (cJSON_IsBool(mut)) {
    last_muted_ = cJSON_IsTrue(mut);
  }

  const uint8_t effective = last_muted_ ? 0 : last_volume_;
  engine_->setVolume(effective);
  ESP_LOGI(TAG, "ServerSettings applied: volume=%u muted=%d (effective=%u)",
           (unsigned)last_volume_, (int)last_muted_, (unsigned)effective);

  cJSON_Delete(root);
}

// Drain one Snapcast frame from the socket per call:
//  1. Block on recv() for up to 26 bytes of base header. BLOCKING is the
//     point — at WiFi/LwIP level the socket is woken the moment bytes
//     arrive, so we never lag behind WireChunk delivery. If SO_RCVTIMEO
//     fires (server radio-silent), we return empty-handed and the caller
//     loops back.
//  2. Based on `type`, consume the payload:
//       - WireChunk (2)       → stream through feedPCM with TS prefix skip.
//       - ServerSettings (3)  → read full JSON payload, extract volume /
//                               muted, push to the AudioEngine.
//       - anything else       → drain the bytes and discard, so the stream
//                               stays framed (CodecHeader, StreamTags, Time,
//                               etc. — we'll wire them up later if needed).
//  3. For WireChunk, strip the 8-byte tv timestamp at the start of the
//     payload and feed the rest to the audio engine. We track `ts_consumed`
//     across recvs so a TCP-split frame is still fed correctly.
void Snapclient::tick() {
  if (!connected_ || sock_ < 0) return;
  if (!engine_) return;

  uint8_t hdr[geelen::SNAPCAST_BASE_HEADER_SIZE];
  size_t have = 0;
  while (have < sizeof(hdr)) {
    const int n = recv(sock_, hdr + have, sizeof(hdr) - have, 0);
    if (n == 0) { disconnect(); return; }  // peer closed
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // SO_RCVTIMEO fired mid-header. If we already have some bytes, stay
        // put and keep reading — otherwise return so the outer loop can
        // notice disconnect() requests from the UI/config path.
        if (have == 0) return;
        continue;
      }
      // EBADF / ECONNRESET / ... — the caller disconnected us, or the peer
      // reset the connection. Tear down cleanly.
      disconnect();
      return;
    }
    have += (size_t)n;
  }

  uint16_t type = 0;
  uint32_t payload_size = 0;
  if (!geelen::parseSnapcastBaseHeader(hdr, sizeof(hdr), type, payload_size)) {
    ESP_LOGW(TAG, "Bad base header; dropping connection");
    disconnect();
    return;
  }

  // ServerSettings gets the full payload accumulated in buf_ so cJSON has a
  // contiguous blob to parse. Typical size is <200 bytes (one recv). If it
  // ever exceeds our 2 KiB buffer, we fall through to drain-and-discard
  // instead of half-parsing it.
  if (type == geelen::SNAPCAST_MSG_SERVER_SETTINGS &&
      payload_size >= 4u &&
      payload_size <= (uint32_t)READ_BUF_SIZE) {
    size_t got = 0;
    while (got < payload_size) {
      const int n = recv(sock_, buf_ + got, payload_size - got, 0);
      if (n == 0) { disconnect(); return; }
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
        disconnect();
        return;
      }
      got += (size_t)n;
    }
    // JsonMessage payload layout: u32 json_len (LE) + json_len bytes.
    const uint32_t json_len = (uint32_t)buf_[0] |
                              ((uint32_t)buf_[1] << 8) |
                              ((uint32_t)buf_[2] << 16) |
                              ((uint32_t)buf_[3] << 24);
    if (json_len + 4u <= got) {
      applyServerSettings(reinterpret_cast<const char*>(buf_ + 4),
                          (size_t)json_len);
    }
    return;
  }

  uint32_t remaining = payload_size;
  size_t ts_consumed = 0;  // only meaningful for WireChunk
  while (remaining > 0) {
    const size_t want = remaining > READ_BUF_SIZE ? READ_BUF_SIZE : remaining;
    const int n = recv(sock_, buf_, want, 0);
    if (n <= 0) { disconnect(); return; }
    remaining -= (uint32_t)n;

    if (type != geelen::SNAPCAST_MSG_WIRE_CHUNK) continue;

    // Skip past the 8-byte tv (sec, usec) prefix that heads every
    // WireChunk payload. `ts_consumed` accumulates across split recvs.
    size_t off = 0;
    if (ts_consumed < geelen::SNAPCAST_WIRE_CHUNK_TS_SIZE) {
      const size_t ts_left =
          geelen::SNAPCAST_WIRE_CHUNK_TS_SIZE - ts_consumed;
      if ((size_t)n <= ts_left) {
        ts_consumed += (size_t)n;
        continue;
      }
      off = ts_left;
      ts_consumed = geelen::SNAPCAST_WIRE_CHUNK_TS_SIZE;
    }
    // Single source of truth for "voice prompt owns the ring": gate on it so
    // WireChunks don't splice into the middle of "Snapcast mode" / mode-clip.
    if (!geelen_idf::voiceFeedbackActive()) {
      engine_->feedPCM(buf_ + off, (size_t)n - off);
    }
  }
}

}  // namespace geelen_idf

/**
 * Snapcast protocol – implementation (portable C++, no Arduino/ESP32).
 */

#include "snapcast_protocol.h"

#include <cstdio>
#include <cstring>

namespace geelen {

bool parseSnapcastBaseHeader(const uint8_t* buf, size_t len,
                            uint16_t& type, uint32_t& payload_size) {
  if (buf == nullptr || len < SNAPCAST_BASE_HEADER_SIZE) return false;
  const uint8_t* p = buf;
  type = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
  p += 2 + 2 + 2;   /* 6 bytes */
  p += 4 + 4 + 4 + 4;  /* 16 bytes, p now at offset 22 */
  payload_size = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  return true;
}

size_t buildSnapcastHelloFrame(uint8_t* out, size_t out_sz,
                              const char* mac,
                              const char* hostname,
                              const char* client_name,
                              const char* version,
                              const char* os,
                              const char* arch,
                              uint32_t instance) {
  if (!out || !mac || !hostname || !client_name || !version || !os || !arch) {
    return 0;
  }

  // Build the JSON payload into a scratch buffer so we know its exact length
  // before writing the header. 384 bytes is comfortably larger than every
  // realistic combination of the inputs below (MAC 17 + short strings).
  char json[384];
  const int json_len = std::snprintf(
      json, sizeof(json),
      "{"
        "\"Arch\":\"%s\","
        "\"ClientName\":\"%s\","
        "\"HostName\":\"%s\","
        "\"ID\":\"%s\","
        "\"Instance\":%u,"
        "\"MAC\":\"%s\","
        "\"OS\":\"%s\","
        "\"SnapStreamProtocolVersion\":2,"
        "\"Version\":\"%s\""
      "}",
      arch, client_name, hostname, mac, (unsigned)instance, mac, os, version);
  if (json_len <= 0 || (size_t)json_len >= sizeof(json)) return 0;

  const uint32_t payload_size = 4u + (uint32_t)json_len;
  const size_t total = SNAPCAST_BASE_HEADER_SIZE + (size_t)payload_size;
  if (out_sz < total) return 0;

  uint8_t* p = out;
  auto w16 = [&p](uint16_t v) {
    *p++ = (uint8_t)(v & 0xff);
    *p++ = (uint8_t)((v >> 8) & 0xff);
  };
  auto w32 = [&p](uint32_t v) {
    *p++ = (uint8_t)(v & 0xff);
    *p++ = (uint8_t)((v >> 8) & 0xff);
    *p++ = (uint8_t)((v >> 16) & 0xff);
    *p++ = (uint8_t)((v >> 24) & 0xff);
  };

  // BaseMessage (26 bytes). sent/received timestamps are left at zero; the
  // server fills in `received` server-side and doesn't rely on `sent`
  // being monotonic for Hello.
  w16(SNAPCAST_MSG_HELLO);
  w16(0);               // id
  w16(0);               // refersTo
  w32(0); w32(0);       // sent.sec, sent.usec
  w32(0); w32(0);       // received.sec, received.usec
  w32(payload_size);    // payload size

  // JsonMessage payload: uint32 length + raw bytes.
  w32((uint32_t)json_len);
  std::memcpy(p, json, (size_t)json_len);
  p += json_len;

  return (size_t)(p - out);
}

}  // namespace geelen

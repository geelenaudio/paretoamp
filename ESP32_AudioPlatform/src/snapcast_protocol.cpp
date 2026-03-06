/**
 * Snapcast protocol – implementation (portable C++, no Arduino/ESP32).
 */

#include "snapcast_protocol.h"

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

}  // namespace geelen

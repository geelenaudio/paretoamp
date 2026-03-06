/**
 * Snapcast protocol – portable parsing (base header) for host-side tests.
 */

#ifndef GEELEN_SNAPCAST_PROTOCOL_H
#define GEELEN_SNAPCAST_PROTOCOL_H

#include <cstdint>
#include <cstddef>

namespace geelen {

/** Snapcast base header size in bytes. */
constexpr size_t SNAPCAST_BASE_HEADER_SIZE = 26;

/**
 * Parse Snapcast base header (26 bytes).
 * type at 0–1, payload_size at 22–25 (after 2+2+2+4+4+4+4).
 * Returns false if len < 26.
 */
bool parseSnapcastBaseHeader(const uint8_t* buf, size_t len,
                            uint16_t& type, uint32_t& payload_size);

}  // namespace geelen

#endif  // GEELEN_SNAPCAST_PROTOCOL_H

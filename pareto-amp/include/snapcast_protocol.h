/**
 * Snapcast protocol – portable parsing + Hello frame builder.
 *
 * Wire format reference (little-endian on the wire, SWAP_16/SWAP_32 in
 * upstream snapcast is a no-op on LE hosts so what goes on the wire matches
 * the host's native byte order for LE hosts):
 *
 *   BaseMessage (26 bytes):
 *     uint16_t type
 *     uint16_t id
 *     uint16_t refersTo
 *     int32_t  sent.sec
 *     int32_t  sent.usec
 *     int32_t  received.sec
 *     int32_t  received.usec
 *     uint32_t payload_size
 *
 *   JsonMessage payload (used by Hello, ServerSettings, StreamTags,
 *   ClientInfo, Error):
 *     uint32_t json_len
 *     char[]   json_bytes (no NUL terminator)
 *
 * Canonical enum (see common/message/message.hpp in badaix/snapcast):
 *   kBase           = 0
 *   kCodecHeader    = 1
 *   kWireChunk      = 2
 *   kServerSettings = 3
 *   kTime           = 4
 *   kHello          = 5
 *   kClientInfo     = 7
 *   kError          = 8
 */

#ifndef GEELEN_SNAPCAST_PROTOCOL_H
#define GEELEN_SNAPCAST_PROTOCOL_H

#include <cstdint>
#include <cstddef>

namespace geelen {

/** Snapcast base header size in bytes. */
constexpr size_t SNAPCAST_BASE_HEADER_SIZE = 26;

/** Snapcast message type constants. */
constexpr uint16_t SNAPCAST_MSG_CODEC_HEADER    = 1;
constexpr uint16_t SNAPCAST_MSG_WIRE_CHUNK      = 2;
constexpr uint16_t SNAPCAST_MSG_SERVER_SETTINGS = 3;
constexpr uint16_t SNAPCAST_MSG_TIME            = 4;
constexpr uint16_t SNAPCAST_MSG_HELLO           = 5;
constexpr uint16_t SNAPCAST_MSG_CLIENT_INFO     = 7;
constexpr uint16_t SNAPCAST_MSG_ERROR           = 8;

/** Each WireChunk payload is prefixed with an 8-byte tv timestamp
 *  (int32 sec + int32 usec). Audio starts at offset 8. */
constexpr size_t SNAPCAST_WIRE_CHUNK_TS_SIZE = 8;

/**
 * Parse Snapcast base header (26 bytes).
 * type at 0–1, payload_size at 22–25 (after 2+2+2+4+4+4+4).
 * Returns false if len < 26.
 */
bool parseSnapcastBaseHeader(const uint8_t* buf, size_t len,
                            uint16_t& type, uint32_t& payload_size);

/**
 * Serialize a Snapcast Hello frame (BaseMessage type=5 + JsonMessage payload)
 * into @p out. Returns total bytes written, or 0 on error (buffer too small,
 * JSON overflow, missing required string).
 *
 * Strings must be plain ASCII. For speed / code size we do NOT JSON-escape
 * them, so callers must ensure none contain `"` or `\`.
 *
 * @param out          output buffer
 * @param out_sz       output buffer capacity
 * @param mac          "aa:bb:cc:dd:ee:ff" (used for both MAC and ID fields)
 * @param hostname     e.g. "geelen-amp"
 * @param client_name  e.g. "Snapclient" (Snapcast's Snapweb filters by this)
 * @param version      e.g. "0.1.0"
 * @param os           e.g. "ESP-IDF"
 * @param arch         e.g. "xtensa"
 * @param instance     usually 1
 */
size_t buildSnapcastHelloFrame(uint8_t* out, size_t out_sz,
                              const char* mac,
                              const char* hostname,
                              const char* client_name,
                              const char* version,
                              const char* os,
                              const char* arch,
                              uint32_t instance);

}  // namespace geelen

#endif  // GEELEN_SNAPCAST_PROTOCOL_H

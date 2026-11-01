/**
 * SSDP M-SEARCH parsing for the DLNA MediaRenderer.
 *
 * Header names are matched only at the start of a line. A substring search
 * for "ST:" also hits the "ST:" inside "HOST:", which is the first header
 * of every normal M-SEARCH, so the device would answer nothing.
 *
 * IDF-free so the native Unity suite can compile it.
 */

#ifndef GEELEN_DLNA_SSDP_H
#define GEELEN_DLNA_SSDP_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace geelen_idf {

/** root, uuid, MediaRenderer, AVTransport, RenderingControl, ConnectionManager. */
constexpr size_t kDlnaSsdpMaxTargets = 6;

/**
 * Stable RFC 4122 UUID (version 4, variant 10xx) derived from the STA MAC.
 * The node is the MAC, so the UDN stays unique per board and is hex-only.
 * Android and Cling reject a UDN that is not a real UUID, which hides the
 * renderer. `out` needs 37 bytes. Returns false if the buffer is short.
 */
inline bool dlnaUuidFromMac(const uint8_t mac[6], char* out, size_t cap) {
  if (!mac || !out || cap < 37) return false;
  const int n = std::snprintf(
      out, cap, "5f9e4a40-80f3-4000-8000-%02x%02x%02x%02x%02x%02x",
      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return n == 36;
}

/**
 * Copy the value of header `name` (e.g. "ST") into `out`.
 * Matches the name only at the start of a line, case-insensitive, then
 * optional space or tab, then the value up to CR/LF. Trailing whitespace
 * is stripped. Returns false when the header is absent, empty, or longer
 * than `cap` (including the terminating NUL).
 */
inline bool dlnaParseHeader(const char* packet, const char* name,
                            char* out, size_t cap) {
  if (!packet || !name || !name[0] || !out || cap == 0) return false;
  const size_t nlen = std::strlen(name);
  const char* p = packet;
  while (*p) {
    bool name_match = true;
    for (size_t i = 0; i < nlen; ++i) {
      char a = p[i];
      char b = name[i];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
      if (a != b) { name_match = false; break; }
    }
    if (name_match && p[nlen] == ':') {
      const char* v = p + nlen + 1;
      while (*v == ' ' || *v == '\t') ++v;
      size_t i = 0;
      while (v[i] && v[i] != '\r' && v[i] != '\n') {
        if (i + 1 >= cap) return false;
        out[i] = v[i];
        ++i;
      }
      while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t')) --i;
      out[i] = '\0';
      return i > 0;
    }
    const char* nl = std::strchr(p, '\n');
    if (!nl) break;
    p = nl + 1;
  }
  return false;
}

/**
 * The advertisements we NOTIFY and, for ssdp:all, answer with.
 * The UUID slot is `udn` (the full "uuid:..." string). A null or empty
 * UDN is omitted so we never advertise a blank NT.
 * Pointers in `out` are either string literals or `udn`.
 */
inline size_t dlnaAdvertTargets(const char* udn, const char** out, size_t cap) {
  if (!out || cap == 0) return 0;
  const char* all[kDlnaSsdpMaxTargets] = {
    "upnp:rootdevice",
    (udn && udn[0]) ? udn : nullptr,
    "urn:schemas-upnp-org:device:MediaRenderer:1",
    "urn:schemas-upnp-org:service:AVTransport:1",
    "urn:schemas-upnp-org:service:RenderingControl:1",
    "urn:schemas-upnp-org:service:ConnectionManager:1",
  };
  size_t n = 0;
  for (size_t i = 0; i < kDlnaSsdpMaxTargets && n < cap; ++i) {
    if (all[i]) out[n++] = all[i];
  }
  return n;
}

/**
 * Search targets to unicast back for this ST.
 * ssdp:all expands to every advertisement. A known type expands to that
 * one string. Anything else (including a MediaServer URN) expands to none.
 */
inline size_t dlnaSearchTargets(const char* st, const char* udn,
                                const char** out, size_t cap) {
  if (!st || !st[0] || !out || cap == 0) return 0;
  const char* all[kDlnaSsdpMaxTargets];
  const size_t n = dlnaAdvertTargets(udn, all, kDlnaSsdpMaxTargets);
  if (std::strcmp(st, "ssdp:all") == 0) {
    const size_t copy = n < cap ? n : cap;
    for (size_t i = 0; i < copy; ++i) out[i] = all[i];
    return copy;
  }
  for (size_t i = 0; i < n; ++i) {
    if (std::strcmp(st, all[i]) == 0) {
      out[0] = all[i];
      return 1;
    }
  }
  return 0;
}

}  // namespace geelen_idf

#endif

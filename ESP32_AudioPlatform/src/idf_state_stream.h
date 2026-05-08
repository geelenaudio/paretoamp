// Phase 4 — push-based device state for the SPA.
//
// `/api/state/stream` is a single SSE endpoint that delivers the full state
// snapshot on connect and a fresh snapshot whenever ConfigManager's monotonic
// `stateVersion()` advances. The SPA subscribes once per page mount; it no
// longer needs to re-poll `/api/state` after every mutation.
//
// Why "snapshot on change" rather than RFC-6902 JSON-Patch:
//   - The full snapshot is ~1–2 KB of JSON, gzipped by httpd to <1 KB.
//   - Mutations are debounced server-side (250 ms) so a slider drag emits
//     at most ~4 frames/s to all subscribers combined.
//   - A real diff engine would force an in-memory canonicalisation step on
//     every change, plus two cJSON trees, which costs more CPU than just
//     re-serializing on a low-rate change channel.
//
// We keep the door open for true JSON-Patch by reserving the SSE `event:`
// field — today only `event: snapshot` and `event: ping` are emitted, and a
// future delta-mode would add `event: patch` carrying a JSON-Patch array
// once the canonical snapshot diverges by less than N keys.
//
// Lifecycle: `stateStreamInit(cfg)` is called from `app_main` after the
// config manager is up, before httpd starts. It spawns the broadcaster
// task and prepares the subscriber list. `stateStreamRegister(httpd)`
// installs the URI handler. `stateStreamMarkDirty()` is a forcing function
// for code paths whose mutations don't bump `stateVersion()` (rare —
// `markDirty*` covers everything today).

#ifndef GEELEN_IDF_STATE_STREAM_H
#define GEELEN_IDF_STATE_STREAM_H

#ifdef ESP_PLATFORM

#include "esp_http_server.h"

namespace geelen_idf {

class ConfigManager;

void stateStreamInit(ConfigManager* cfg);
void stateStreamRegister(httpd_handle_t server);
void stateStreamMarkDirty();

}  // namespace geelen_idf

#endif  // ESP_PLATFORM

#endif  // GEELEN_IDF_STATE_STREAM_H

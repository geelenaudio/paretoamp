// Phase 4 — push-based device state for the SPA.
//
// `/api/state/stream` is a single SSE endpoint that delivers a full state
// snapshot on connect (`event: snapshot`) and, on later version bumps, a
// compact top-level key diff (`event: diff`) so each SSE frame stays under
// one lwip pbuf. A full snapshot is resent if too many keys changed or the
// subscriber has never received a frame (last_sent_v == 0).
//
  // Cadence: 250 ms idle, 500 ms while A2DP or a voice prompt is active.
  // Live AVRCP volume is included even when NVS `stateVersion` has not
  // bumped, so the SPA can follow the phone rocker without extra HTTP.
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

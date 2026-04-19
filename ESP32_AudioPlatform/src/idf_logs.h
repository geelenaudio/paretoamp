// Live log ring buffer + SSE stream source.
//
// Installs an esp_log vprintf hook that mirrors every ESP_LOGx line to:
//   1. the original vprintf (UART / JTAG), so serial debugging still works;
//   2. a FreeRTOS ring buffer consumed by the SSE handler.
//
// The ring buffer holds N most-recent formatted lines. On SSE connect the
// handler replays the snapshot, then blocks on the ring for new entries.
// A single-subscriber rule is enforced by CAS on a flag so an accidental
// second EventSource doesn't fan out logs to both (which would split the
// buffer unpredictably).
//
// Storage shape is "raw lines" rather than parsed {level, tag, msg}
// structs: we hand the client everything the UART saw and let the JS
// layer do the prettification. This keeps the firmware side small and
// makes future log-format changes a UI-only concern.
#pragma once

#include "esp_http_server.h"

namespace geelen_idf {

// Installs the vprintf hook. Safe to call multiple times (idempotent).
void logsInit();

// Register the SSE endpoint under /api/logs/stream. Called from
// registerApiHandlers once the ring is alive.
void logsRegisterStream(httpd_handle_t server);

}  // namespace geelen_idf

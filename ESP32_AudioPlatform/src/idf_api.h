// Phase 1 API handlers: /api/state, /api/system, /api/volume, /api/channels,
// /api/mode, /api/dlna, /api/eq, /api/eq/headroom, /api/eq/presets/…,
// /api/reset, /api/device-name.
//
// SSE endpoints (/api/logs/stream) are wired from Phase 7 in a separate
// module because they need the log-ring backpressure logic.
//
// Wire format is JSON in / JSON out, parsed with cJSON. The full surface is
// documented in the OpenAPI 3.1 spec at web/api/openapi.yaml; any new
// endpoint added here must have a matching spec entry.

#pragma once

struct httpd_req;
typedef struct httpd_req httpd_req_t;

namespace geelen_idf {

class ConfigManager;

// Register every /api/* JSON handler onto the supplied httpd handle.
// `cfg` is stashed in user_ctx so every handler has cheap access.
void registerApiHandlers(void* server_handle, ConfigManager* cfg);

}  // namespace geelen_idf

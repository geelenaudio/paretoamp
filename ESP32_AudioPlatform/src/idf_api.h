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
struct cJSON;

namespace geelen_idf {

class ConfigManager;

// Register every /api/* JSON handler onto the supplied httpd handle.
// `cfg` is stashed in user_ctx so every handler has cheap access.
void registerApiHandlers(void* server_handle, ConfigManager* cfg);

// Build the canonical "device state" JSON object — the same one served by
// `GET /api/state`. Caller owns the returned `cJSON*` and must `cJSON_Delete`
// it (or attach it to a parent that owns it). Used by the SSE state stream
// so the snapshot pushed to subscribers is byte-for-byte identical to the
// REST snapshot.
cJSON* buildStateJson(ConfigManager* cfg);

}  // namespace geelen_idf

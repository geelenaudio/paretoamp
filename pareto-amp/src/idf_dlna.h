// Native DLNA (UPnP AV) MediaRenderer implementation.
//
// Three moving parts live in this module:
//
//   1. SSDP responder
//      ------------------
//      Joins 239.255.255.250:1900 on the STA interface, replies to
//      M-SEARCH requests whose ST matches any of our advertised types
//      (root device, MediaRenderer, AVTransport, RenderingControl,
//      ConnectionManager) and emits periodic NOTIFY ssdp:alive
//      announcements so control points can notice us without polling.
//
//   2. HTTP surface
//      ------------------
//      Four URI trees are served off the existing `esp_http_server`
//      instance:
//        GET  /dlna/desc.xml          root MediaRenderer descriptor
//        GET  /dlna/scpd/avt.xml      AVTransport SCPD
//        GET  /dlna/scpd/rc.xml       RenderingControl SCPD
//        GET  /dlna/scpd/cm.xml       ConnectionManager SCPD
//        POST /dlna/ctrl/avt          AVTransport SOAP actions
//        POST /dlna/ctrl/rc           RenderingControl SOAP actions
//        POST /dlna/ctrl/cm           ConnectionManager SOAP actions
//        * /dlna/evt/*                GENA subscribe/unsubscribe stubs
//      No GENA NOTIFY callbacks are sent (yet) — most casting apps poll
//      GetPositionInfo / GetTransportInfo anyway. The endpoints accept
//      SUBSCRIBE requests so dumb control points that refuse to work
//      without a subscription header still get a 200.
//
//   3. Renderer state
//      ------------------
//      Transitions between Stopped / Playing / Paused / Transitioning
//      triggered by SOAP actions. The only "loaded" content is a
//      currentURI + metadata blob; the actual bytes are pulled by the
//      MP3 streamer (idf_mp3_streamer) which is started from `play()`
//      and stopped from `stop()` / `pause()`.
//
// Thread model:
//   - SSDP runs in its own LWIP-safe task (prio 4 on core 0, tied to the
//     WiFi stack).
//   - All SOAP handlers execute on the HTTPD task and mutate state under
//     `s_mutex`.
//   - The MP3 streamer has its own task; this module just signals it via
//     start/stop APIs.
//
// Config integration:
//   - ConfigManager::getDlnaEnabled() controls whether we advertise.
//   - ConfigManager::getDeviceName() supplies the friendly name.
//   - Volume / mute changes from RenderingControl update the shared
//     ConfigManager (so the SPA sees the slider move) and call the weak
//     `on_volume_changed` hook to propagate to the engine.

#pragma once

#include "esp_http_server.h"

namespace geelen_idf {

class ConfigManager;

namespace dlna {

enum class TransportState : int {
  NoMedia = 0,
  Stopped,
  Playing,
  Paused,
  Transitioning,
};

struct Snapshot {
  TransportState state;
  char uri[256];
  char title[128];
  char artist[64];
  char album[64];
  uint32_t duration_s;
  uint32_t position_s;
};

// Install the DLNA module. `server` is the already-started main HTTPD
// handle; we register additional URI handlers for DLNA descriptors and
// SOAP. Call after the WiFi STA has an IP — before that, SSDP can't do
// much. Returns false if SSDP socket setup fails.
bool start(httpd_handle_t server, ConfigManager* cfg);

// Stop advertising (deletes SSDP socket + task). Keeps HTTP descriptors
// registered so any in-flight control point GETs still get a 410 from
// the router.
void stop();

// Snapshot the current renderer state for display in the Web UI.
Snapshot snapshot();

}  // namespace dlna
}  // namespace geelen_idf

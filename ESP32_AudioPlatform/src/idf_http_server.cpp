// ESP-IDF HTTP server implementation (esp_http_server)
//
// Layout:
//   - Static assets in /spiffs/web/...       (served by handle_static)
//   - REST API at /api/*                     (Phase 1 collapses these)
//
// Static assets are pre-gzipped at build time by scripts/build_web.py, so the
// handler serves `<path>.gz` with `Content-Encoding: gzip` whenever the
// gzipped variant exists; it falls back to the plain file otherwise. This
// keeps both the SPIFFS footprint small and the transport wire-efficient.

#include "idf_http_server.h"
#include "idf_api.h"
#include "idf_config_manager.h"
#include "idf_logs.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace geelen_idf {

static const char* TAG = "HTTP";

// Schedule a graceful restart (no-op if already scheduled). Lives here rather
// than in idf_api.cpp so both the legacy handlers and the new JSON handlers
// call into the same implementation. Declared non-static so idf_api.cpp can
// forward-declare and reach it.
void scheduleRestart(unsigned delay_ms);

// Fallback HTML shown when /spiffs/web/index.html(.gz) is missing — e.g. a
// freshly flashed device that hasn't had `pio run -t uploadfs` yet. Kept
// intentionally tiny (<1 KB) so it fits inside a single TCP segment.
static const char FALLBACK_INDEX_HTML[] = R"raw(<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Geelen Audio (bootstrap)</title>
<style>body{font:14px/1.5 system-ui;background:#0b0d10;color:#eef1f5;margin:0;padding:2rem;}
a{color:#6ab0ff}code{background:#1a1f26;padding:2px 6px;border-radius:4px;font-family:ui-monospace,monospace}</style>
</head><body>
<h1>Geelen Audio — SPIFFS not loaded</h1>
<p>The firmware is running but the SPA assets are missing from SPIFFS. Upload them with:</p>
<p><code>pio run -t uploadfs -e amped-esp32-idf</code></p>
<p>Legacy API probes: <a href="/api/status">/api/status</a></p>
</body></html>)raw";

void scheduleRestart(unsigned delay_ms) {
  xTaskCreate(
    [](void* arg) {
      unsigned ms = (unsigned)(uintptr_t)arg;
      vTaskDelay(pdMS_TO_TICKS(ms));
      esp_restart();
    },
    "restart",
    2048,
    (void*)delay_ms,
    1,
    nullptr
  );
}

// ---- Static asset serving --------------------------------------------------

static const char* mime_for(const char* path) {
  // Look at the extension (before any optional `.gz`).
  const char* dot = strrchr(path, '.');
  if (!dot) return "application/octet-stream";
  // If the file ends with `.gz`, step back to the real extension.
  if (strcmp(dot, ".gz") == 0) {
    const char* gz = dot;
    for (const char* p = gz - 1; p >= path; --p) {
      if (*p == '.') { dot = p; break; }
      if (*p == '/') { dot = gz; break; }
    }
  }
  if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
  if (!strcmp(dot, ".css"))  return "text/css; charset=utf-8";
  if (!strcmp(dot, ".js") || !strcmp(dot, ".mjs")) return "text/javascript; charset=utf-8";
  if (!strcmp(dot, ".json")) return "application/json; charset=utf-8";
  if (!strcmp(dot, ".svg"))  return "image/svg+xml";
  if (!strcmp(dot, ".png"))  return "image/png";
  if (!strcmp(dot, ".ico"))  return "image/x-icon";
  if (!strcmp(dot, ".woff2")) return "font/woff2";
  if (!strcmp(dot, ".yaml") || !strcmp(dot, ".yml")) return "application/yaml; charset=utf-8";
  if (!strcmp(dot, ".xml"))  return "application/xml; charset=utf-8";
  if (!strcmp(dot, ".txt"))  return "text/plain; charset=utf-8";
  if (!strcmp(dot, ".map"))  return "application/json; charset=utf-8";
  return "application/octet-stream";
}

// Serve `/spiffs/<abs_path>`. Tries `<path>.gz` first (sets Content-Encoding:
// gzip) and falls back to the plain file. Returns ESP_OK on success, a
// non-OK httpd result (already sent via send_err) otherwise.
static esp_err_t serve_file(httpd_req_t* req, const char* fs_path_no_gz,
                            const char* mime) {
  char path[160];
  struct stat st{};

  // Prefer the gzipped variant.
  snprintf(path, sizeof(path), "%s.gz", fs_path_no_gz);
  bool gz = (stat(path, &st) == 0 && S_ISREG(st.st_mode));
  if (!gz) {
    strncpy(path, fs_path_no_gz, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
      return ESP_FAIL;
    }
  }

  int fd = open(path, O_RDONLY);
  if (fd < 0) return ESP_FAIL;

  httpd_resp_set_type(req, mime);
  if (gz) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  // Cache static assets for a day — they're versioned implicitly by firmware
  // build; a new upload flips the gz mtime and the browser will re-fetch on
  // any 304-validating load. Keep the TTL modest so dev iteration isn't
  // painful.
  httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");

  char buf[1024];
  ssize_t r;
  while ((r = read(fd, buf, sizeof(buf))) > 0) {
    if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) {
      close(fd);
      return ESP_FAIL;
    }
  }
  close(fd);
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

static esp_err_t handle_static(httpd_req_t* req) {
  // `req->uri` includes query string. Strip it first.
  const char* uri = req->uri;
  char path[128];
  size_t i = 0;
  for (; uri[i] && uri[i] != '?' && i < sizeof(path) - 1; ++i) path[i] = uri[i];
  path[i] = '\0';

  // Reject `..` — no path traversal allowed. We're joining with /spiffs/web/
  // below; a leading `..` would escape the static root.
  if (strstr(path, "..")) {
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "nope");
  }

  // Route rewrites for non-file URLs that should still map to static assets:
  //   /                -> /spiffs/web/index.html           (SPA shell)
  //   /docs or /docs/  -> /spiffs/web/docs/index.html      (Swagger UI)
  //   /openapi.yaml    -> /spiffs/web/api/openapi.yaml     (API spec)
  //   anything else    -> /spiffs/web<path>
  char fs_path[160];
  if (path[0] == '\0' || strcmp(path, "/") == 0) {
    snprintf(fs_path, sizeof(fs_path), "/spiffs/web/index.html");
  } else if (strcmp(path, "/docs") == 0 || strcmp(path, "/docs/") == 0) {
    snprintf(fs_path, sizeof(fs_path), "/spiffs/web/docs/index.html");
  } else if (strcmp(path, "/openapi.yaml") == 0) {
    snprintf(fs_path, sizeof(fs_path), "/spiffs/web/api/openapi.yaml");
  } else {
    snprintf(fs_path, sizeof(fs_path), "/spiffs/web%s", path);
  }

  esp_err_t r = serve_file(req, fs_path, mime_for(fs_path));
  if (r == ESP_OK) return ESP_OK;

  // For SPA deep-links (`/mode`, `/dsp`, …) our router is hash-based so we
  // don't actually need an HTML fallback — browsers always request `/` for
  // root navigations and `#/…` stays client-side. Still, if someone manually
  // types `/mode` we fall back to index.html so the route resolves after JS
  // loads. Only do this if the path has no extension (i.e. it's a "page",
  // not a missing asset like `/foo.js.gz`).
  const bool looks_like_page = !strchr(path, '.');
  if (looks_like_page) {
    if (serve_file(req, "/spiffs/web/index.html", "text/html; charset=utf-8") == ESP_OK) {
      return ESP_OK;
    }
    // If even index.html isn't on SPIFFS, surface the bootstrap page.
    if (strcmp(path, "/") == 0 || path[0] == '\0') {
      httpd_resp_set_type(req, "text/html; charset=utf-8");
      return httpd_resp_send(req, FALLBACK_INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    }
  }

  return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
}

// ---- Legacy REST API (Phase 1 consolidates this) ---------------------------

static esp_err_t handle_status(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  char host[SNAPCAST_HOST_MAX] = {0};
  cfg->getSnapcastHost(host, sizeof(host));

  char out[384];
  int n = snprintf(out, sizeof(out),
                   "{\"volume\":%u,\"source\":%u,\"preset\":%u,"
                   "\"snap_host\":\"%s\",\"snap_port\":%u}",
                   (unsigned)cfg->getVolume(),
                   (unsigned)cfg->getAudioSource(),
                   (unsigned)cfg->getPresetId(),
                   host,
                   (unsigned)cfg->getSnapcastPort());
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, out, n);
}

static esp_err_t handle_volume(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char vbuf[16];
    if (httpd_query_key_value(query, "v", vbuf, sizeof(vbuf)) == ESP_OK) {
      int v = atoi(vbuf);
      if (v < 0) v = 0;
      if (v > 100) v = 100;
      cfg->setVolume((uint8_t)v);
      cfg->save();
    }
  }

  char out[64];
  int n = snprintf(out, sizeof(out), "{\"volume\":%u}", (unsigned)cfg->getVolume());
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, out, n);
}

static esp_err_t handle_source(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  AudioSource prev = cfg->getAudioSource();
  char query[64];
  int s = 0;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char sbuf[16];
    if (httpd_query_key_value(query, "s", sbuf, sizeof(sbuf)) == ESP_OK) {
      s = atoi(sbuf);
    }
  }
  if (s < 0) s = 0;
  if (s > 1) s = 1;
  cfg->setAudioSource((AudioSource)s);
  cfg->save();

  unsigned pu = (unsigned)prev;
  unsigned nu = (unsigned)cfg->getAudioSource();
  const bool bt_edge = (nu == 0u || pu == 0u);
  const bool reboot = (pu != nu && bt_edge);
  if (reboot) {
    ESP_LOGI(TAG, "Audio source %u -> %u: rebooting to (un)load BT stack",
             pu, nu);
    scheduleRestart(500);
  }

  char out[96];
  int n = snprintf(out, sizeof(out),
                   "{\"source\":%u,\"reboot\":%s}",
                   (unsigned)cfg->getAudioSource(),
                   reboot ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, out, n);
}

static esp_err_t handle_wifi(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  int total = req->content_len;
  if (total <= 0 || total > 512) total = 512;
  char body[513];
  int r = httpd_req_recv(req, body, total);
  if (r < 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
  body[r] = '\0';

  auto getField = [&](const char* key, char* out, size_t out_sz) {
    const char* p = strstr(body, key);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(key);
    const char* end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
  };

  char ssid[WIFI_SSID_MAX];
  char pass[WIFI_PASS_MAX];
  getField("ssid=", ssid, sizeof(ssid));
  getField("pass=", pass, sizeof(pass));

  if (ssid[0] != '\0') {
    cfg->setWifiSsid(ssid);
    cfg->setWifiPass(pass);
    cfg->save();
    ESP_LOGI(TAG, "WiFi credentials saved via Web UI; rebooting.");
    scheduleRestart(800);
  }

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

bool HttpServer::begin(ConfigManager* cfg) {
  cfg_ = cfg;
  if (!cfg_) return false;
  if (server_) return true;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.lru_purge_enable = true;
  // Phase 1 registers ~16 /api/* handlers + 4 legacy + the static catch-all
  // + headroom for Phase 7 (SSE) and beyond. Default is 8; bump generously.
  config.max_uri_handlers = 32;
  // Enable wildcard matching so `/*` actually fires for /foo.js.gz, /vendor/…,
  // /pages/…. Without this the catch-all only matches the literal string `/*`.
  config.uri_match_fn = httpd_uri_match_wildcard;

  httpd_handle_t h = nullptr;
  if (httpd_start(&h, &config) != ESP_OK) return false;
  server_ = (void*)h;

  // Legacy compat endpoints. The SPA prefers the richer JSON endpoints in
  // idf_api.cpp, but these are retained so existing tools/probes keep
  // working:
  //   GET  /api/status        — scalar snapshot (Phase 1 ships GET /api/state)
  //   GET  /api/volume?v=N    — convenience GET (Phase 1 adds POST JSON)
  //   POST /api/source?s=N    — old Bluetooth/Network toggle
  //   POST /api/wifi          — form-encoded creds (form is still the only
  //                             path here until we ship a WiFi page)
  httpd_uri_t status_uri{ .uri="/api/status",   .method=HTTP_GET,  .handler=handle_status,   .user_ctx=cfg_ };
  httpd_uri_t vol_uri{    .uri="/api/volume",   .method=HTTP_GET,  .handler=handle_volume,   .user_ctx=cfg_ };
  httpd_uri_t src_uri{    .uri="/api/source",   .method=HTTP_POST, .handler=handle_source,   .user_ctx=cfg_ };
  httpd_uri_t wifi_uri{   .uri="/api/wifi",     .method=HTTP_POST, .handler=handle_wifi,     .user_ctx=cfg_ };
  httpd_register_uri_handler(h, &status_uri);
  httpd_register_uri_handler(h, &vol_uri);
  httpd_register_uri_handler(h, &src_uri);
  httpd_register_uri_handler(h, &wifi_uri);

  // Phase 1 JSON API (POST /api/state, POST /api/volume JSON, /api/mode,
  // /api/channels, /api/eq, /api/reset, …). Lives in idf_api.cpp.
  registerApiHandlers(h, cfg_);

  // SSE endpoint for the Logs page. Registered before the catch-all so
  // /api/logs/stream wins over the static handler's /* pattern.
  geelen_idf::logsRegisterStream(h);

  // Catch-all static handler, registered last so the /api/* URIs win by
  // prefix-specificity above it.
  httpd_uri_t static_uri{ .uri="/*", .method=HTTP_GET, .handler=handle_static, .user_ctx=cfg_ };
  httpd_register_uri_handler(h, &static_uri);

  ESP_LOGI(TAG, "HTTP server started (static from /spiffs/web/, JSON API on /api/*)");
  return true;
}

}  // namespace geelen_idf

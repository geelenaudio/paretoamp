// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
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
#include "idf_state_stream.h"
#include "idf_wifi_manager.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace geelen_idf {

static const char* TAG = "HTTP";

// Singleton pointer set in HttpServer::begin so static URI handlers (which
// only get user_ctx = ConfigManager*) can also see the Wi-Fi state. It is
// only used for read-only `isApOnly()` probing — never owned here.
static WifiManager* s_wifi = nullptr;

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
<style>body{font:14px/1.5 system-ui;background:#16181A;color:#FFFCF2;margin:0;padding:2rem;}
a{color:#8aa890}code{background:#24282c;padding:2px 6px;border-radius:4px;font-family:ui-monospace,monospace}</style>
</head><body>
<h1>Geelen Audio — SPIFFS not loaded</h1>
<p>The firmware is running but the SPA assets are missing from SPIFFS. Upload them with:</p>
<p><code>pio run -t uploadfs -e amped-esp32-idf</code></p>
<p>Legacy API probes: <a href="/api/status">/api/status</a></p>
</body></html>)raw";

// Wi‑Fi onboarding portal shown on OS captive-portal probe URLs while the
// device runs the AP fallback. `/` itself serves the full SPA. Self-contained
// so it works even when SPIFFS is empty / not yet uploaded. Posts form-encoded
// creds to `POST /api/wifi`, reads nearby SSIDs from `GET /api/wifi/scan`.
static const char SETUP_HTML[] = R"raw(<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Geelen Audio — Wi-Fi setup</title>
<style>
:root{color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;padding:0;background:#16181A;color:#FFFCF2;font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
main{max-width:480px;margin:0 auto;padding:24px}
h1{font-size:22px;margin:0 0 4px;letter-spacing:.2px}
.sub{color:#DEDCD4;margin:0 0 20px;font-size:14px}
.card{background:#1c1f22;border:1px solid #32383e;border-radius:10px;padding:18px;margin-bottom:14px}
label{display:block;font-size:13px;color:#DEDCD4;margin:14px 0 6px}
label:first-child{margin-top:0}
input,select{width:100%;padding:11px 12px;border-radius:8px;border:1px solid #434a52;background:#24282c;color:#FFFCF2;font:inherit}
input:focus,select:focus{outline:none;border-color:#8aa890}
button{width:100%;padding:12px 14px;border-radius:8px;border:none;background:#8aa890;color:#FFFCF2;font:inherit;font-weight:600;cursor:pointer;margin-top:18px}
button[disabled]{opacity:.55;cursor:default}
.row{display:flex;gap:8px;align-items:center}
.row button{margin:0;flex:0 0 auto;background:#24282c;color:#FFFCF2;width:auto;padding:11px 14px}
.bar{display:inline-block;width:36px;height:6px;background:#24282c;border-radius:3px;vertical-align:middle;margin-left:6px;overflow:hidden}
.bar i{display:block;height:100%;background:#8aa890}
.muted{color:#DEDCD4;font-size:13px}
.err{color:#ef9a9a;font-size:13px;margin-top:6px;min-height:1.2em}
.busy{display:none;text-align:center;padding:20px}
.busy.show{display:block}
.spinner{width:28px;height:28px;border:3px solid #32383e;border-top-color:#8aa890;border-radius:50%;animation:spin 1s linear infinite;margin:0 auto 10px}
@keyframes spin{to{transform:rotate(360deg)}}
.lock{color:#DEDCD4;margin-left:6px}
</style></head><body>
<main>
  <h1>Wi-Fi setup</h1>
  <p class="sub">Pick your network so the amplifier can reach the internet and Snapcast/DLNA hosts.</p>
  <form id="f" class="card" autocomplete="off">
    <label for="ssid">Network <span id="status" class="muted"></span></label>
    <div class="row">
      <select id="ssid" name="ssid"><option value="">Scanning…</option></select>
      <button type="button" id="rescan" title="Rescan">↻</button>
      <button type="button" id="manual" title="Type SSID manually">A</button>
    </div>
    <input id="ssid_text" name="ssid_text" placeholder="Network name" style="display:none;margin-top:8px" />
    <label for="pass">Password</label>
    <input id="pass" name="pass" type="password" autocomplete="new-password" />
    <div id="err" class="err"></div>
    <button id="save" type="submit">Validate & save</button>
  </form>
  <div id="busy" class="card busy">
    <div class="spinner"></div>
    <div id="busymsg">Checking credentials…</div>
    <p class="muted">Reconnect your phone/laptop to its normal Wi-Fi. The amplifier will join <span id="targetssid">your network</span> in <span id="cd">15</span> s.</p>
  </div>
  <p class="muted" style="margin-top:16px">
    <a href="/" style="color:#8aa890">Open full controls</a>
    — volume, EQ, Bluetooth pairing, and the rest of the amp UI at 192.168.4.1.
  </p>
</main>
<script>
(function(){
  var sel=document.getElementById('ssid'),text=document.getElementById('ssid_text'),
      pass=document.getElementById('pass'),err=document.getElementById('err'),
      st=document.getElementById('status'),f=document.getElementById('f'),
      busy=document.getElementById('busy'),save=document.getElementById('save'),
      cd=document.getElementById('cd'),tg=document.getElementById('targetssid'),
      rescan=document.getElementById('rescan'),manual=document.getElementById('manual'),
      manualOn=false;
  function setManual(on){
    manualOn=on;
    sel.style.display=on?'none':'';
    text.style.display=on?'':'none';
    if(on)text.focus();
  }
  manual.onclick=function(){setManual(!manualOn)};
  function pct(rssi){var v=Math.max(0,Math.min(100,Math.round((rssi+90)/60*100)));return v}
  function lockIcon(auth){return auth>0?' \u{1F512}':''}
  function refresh(){
    st.textContent='Scanning…';
    sel.innerHTML='<option value="">Scanning…</option>';
    fetch('/api/wifi/scan').then(function(r){return r.json()}).then(function(j){
      var nets=(j&&j.networks)||[];
      if(!nets.length){sel.innerHTML='<option value="">No networks found</option>';st.textContent='';setManual(true);return}
      sel.innerHTML=nets.map(function(n){
        return '<option value="'+(n.ssid||'').replace(/"/g,'&quot;')+'">'+
          (n.ssid||'').replace(/</g,'&lt;')+' ('+n.rssi+' dBm'+lockIcon(n.auth)+')</option>'
      }).join('');
      st.textContent=nets.length+' nearby';
    }).catch(function(e){
      sel.innerHTML='<option value="">Scan failed</option>';
      st.textContent='';
      setManual(true);
    });
  }
  rescan.onclick=function(e){e.preventDefault();refresh()};
  refresh();
  f.onsubmit=function(e){
    e.preventDefault();
    var ssid=manualOn?text.value.trim():sel.value;
    if(!ssid){err.textContent='Pick or type a network name.';return}
    err.textContent='';
    save.disabled=true;
    var busymsg=document.getElementById('busymsg');
    fetch('/api/wifi/validate',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({ssid:ssid,pass:pass.value})})
      .then(function(r){return r.json().then(function(j){return {ok:r.ok,j:j}})})
      .then(function(x){
        if(!x.j || !x.j.ok){
          var reason=(x.j&&x.j.reason)||'FAIL';
          var detail=(x.j&&x.j.detail)||'';
          err.textContent=reason+(detail?' — '+detail:'');
          save.disabled=false;
          return;
        }
        busymsg.textContent='Wi-Fi credentials validated. Saving and rebooting…';
        f.style.display='none';busy.classList.add('show');tg.textContent=ssid;
        return fetch('/api/wifi/commit',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'})
          .then(function(){
            var n=15;cd.textContent=n;
            var t=setInterval(function(){n--;cd.textContent=n;if(n<=0)clearInterval(t)},1000);
          });
      })
      .catch(function(e2){err.textContent='Validate failed: '+e2;save.disabled=false});
  };
})();
</script>
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

// ---- Async streaming for large static files -------------------------------
//
// Why this exists: ESP-IDF's `esp_http_server` uses ONE worker thread that
// processes requests sequentially. While that thread is mid-`send` on a
// large response, every other client (SSE, /api/state polls, page asset
// loads) waits until it's done. The Swagger UI bundle (`/docs/ui.js`,
// ~400 KiB gzipped) made this very visible: a single request could occupy
// the worker for tens of seconds, occasionally getting truncated when TCP
// backpressure (5 s send_wait_timeout) tripped. Concurrent navigation to
// other pages (e.g. /dsp) timed out because the worker was still busy.
//
// Fix: for any file ≥ kAsyncThreshold we detach the request to a worker
// task (PSRAM-backed stack) using `httpd_req_async_handler_begin` — exactly
// the same pattern the SSE handlers use. The httpd worker returns to its
// accept loop within microseconds and other requests proceed in parallel.
// The detached worker streams the file at its own pace with periodic yields
// so lwip's TX path drains between chunks.
//
// Cap: at most kAsyncMaxWorkers concurrent async sends. Beyond that we
// fall back to inline streaming (which still works for the *first* couple
// of large requests; this just prevents a request flood from spawning
// arbitrarily many tasks). 2 is enough for one browser tab refreshing
// the Swagger page while the SPA is also loading.
static constexpr size_t kAsyncThreshold = 32 * 1024;
static constexpr size_t kAsyncMaxWorkers = 2;
static std::atomic<size_t> s_async_workers{0};

struct AsyncFileSendArgs {
  httpd_req_t* req;     // detached request handle (owns its own copy)
  int fd;               // open fd; worker takes ownership and closes it
  size_t total_bytes;   // for logging
};

// Drain `fd` to `req` in 2 KiB chunks with a 1-tick yield between sends so
// the lwip TX task can recycle pbufs between iterations. `httpd_resp_set_*`
// headers must already have been written before `async_handler_begin` —
// IDF locks the response state at handoff, so the worker only emits body.
static void async_file_send_task(void* arg) {
  auto* a = static_cast<AsyncFileSendArgs*>(arg);
  httpd_req_t* req = a->req;
  int fd = a->fd;
  const size_t total = a->total_bytes;
  delete a;

  // 2 KiB chunks: a balance between fewer send_chunk calls (vs. 1 KiB) and
  // a cheap stack footprint (vs. malloc). Stays well below the 5760 B TCP
  // send buf so each chunk fits in a single lwip write. Lives on the task
  // stack — we sized it to 6 KiB below specifically to accommodate this.
  char buf[2048];
  size_t sent = 0;
  ssize_t r;
  while ((r = read(fd, buf, sizeof(buf))) > 0) {
    esp_err_t rc = httpd_resp_send_chunk(req, buf, r);
    if (rc != ESP_OK) {
      ESP_LOGW(TAG, "async send_chunk failed after %u/%u B (rc=0x%x)",
               (unsigned)sent, (unsigned)total, rc);
      goto done;
    }
    sent += (size_t)r;
    // Yield one tick so the lwip TCPIP task can drain its tx queue. Without
    // this we hot-loop into send_chunk faster than ACKs come back, the
    // socket buffer fills, and the next send blocks for up to 5 s — which
    // either truncates the response or stalls other handlers waiting on
    // shared httpd state. One tick = 10 ms (CONFIG_FREERTOS_HZ=100), so
    // a 400 KiB file takes ~2 s of wall-clock yields on top of network
    // time — fine for a one-time SwaggerUI bundle load.
    vTaskDelay(1);
  }

  // Terminating empty chunk completes the chunked body.
  httpd_resp_send_chunk(req, nullptr, 0);
  if (sent != total) {
    ESP_LOGW(TAG, "async send: short read %u/%u B",
             (unsigned)sent, (unsigned)total);
  }

done:
  close(fd);
  httpd_req_async_handler_complete(req);
  s_async_workers.fetch_sub(1, std::memory_order_relaxed);
  vTaskDelete(nullptr);
}

// Hand off `fd` (already-opened, headers already set on `req`) to a detached
// worker. Returns ESP_OK if the worker is running (httpd thread should also
// return ESP_OK so it stops touching `req`); on failure caller is responsible
// for closing fd / returning an error to the client.
static esp_err_t start_async_file_send(httpd_req_t* req, int fd, size_t total_bytes) {
  // Bound concurrency so a request flood can't spawn unbounded workers.
  size_t cur = s_async_workers.load(std::memory_order_relaxed);
  if (cur >= kAsyncMaxWorkers) {
    return ESP_ERR_NO_MEM;
  }
  if (!s_async_workers.compare_exchange_strong(cur, cur + 1)) {
    // Someone raced us to a slot — refuse rather than overshoot.
    return ESP_ERR_NO_MEM;
  }

  httpd_req_t* detached = nullptr;
  esp_err_t rc = httpd_req_async_handler_begin(req, &detached);
  if (rc != ESP_OK || !detached) {
    s_async_workers.fetch_sub(1, std::memory_order_relaxed);
    return rc != ESP_OK ? rc : ESP_FAIL;
  }

  auto* args = new (std::nothrow) AsyncFileSendArgs{detached, fd, total_bytes};
  if (!args) {
    httpd_req_async_handler_complete(detached);
    s_async_workers.fetch_sub(1, std::memory_order_relaxed);
    return ESP_ERR_NO_MEM;
  }

  // Stack on internal DRAM (default for `xTaskCreatePinnedToCore` — see
  // idf_logs.cpp's `sse_stream_task` for the same pattern). 6 KiB
  // accommodates the 2 KiB on-stack chunk buffer plus lwip/httpd/file IO
  // call frames with margin. PSRAM stacks tested unstable here (long
  // chunked writes hit PSRAM-cache flush windows in lwip), so we accept
  // the internal-DRAM cost — capped at 12 KiB total via kAsyncMaxWorkers.
  // Pin to Core 1 to mirror the httpd worker (Core 0 is full of Wi-Fi /
  // BT / lwip; see HttpServer::begin for the rationale).
  // Priority 4: below httpd (10) so /api/state JSON polls take priority,
  // mirrors the SSE log streamer (4).
  BaseType_t ok = xTaskCreatePinnedToCore(
      async_file_send_task, "http-bigsend", /*stack*/ 6144, args,
      /*prio*/ 4, nullptr, /*core*/ 1);
  if (ok != pdPASS) {
    delete args;
    httpd_req_async_handler_complete(detached);
    s_async_workers.fetch_sub(1, std::memory_order_relaxed);
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

// Inline streaming — used for small files and for the fallback when the
// async worker pool is full. Identical to the historical implementation
// (1 KiB chunks). For small files this is fine: a 7 KiB asset is 7 chunks
// and the httpd worker is back to the accept loop in milliseconds.
static esp_err_t inline_file_send(httpd_req_t* req, int fd) {
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

  // Build a weak ETag from (size << 32 | mtime). Two different builds produce
  // distinct ETags; unchanged files re-use the same one so the client keeps
  // its cached copy. Prefix `W/` marks it weak — we don't promise byte-exact
  // equality, only semantic equivalence (good enough for gzip wrappers).
  char etag[40];
  snprintf(etag, sizeof(etag), "W/\"%lx-%lx\"",
           (unsigned long)st.st_size, (unsigned long)st.st_mtime);

  // If the client has a matching ETag, we're done — reply 304 with no body.
  // This is the fast path for unchanged assets (typical page revisit).
  char inm[48];
  if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK
      && strcmp(inm, etag) == 0) {
    httpd_resp_set_status(req, "304 Not Modified");
    httpd_resp_set_hdr(req, "ETag", etag);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, must-revalidate");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
  }

  int fd = open(path, O_RDONLY);
  if (fd < 0) return ESP_FAIL;

  httpd_resp_set_type(req, mime);
  if (gz) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  // Revalidate every load: the browser keeps a copy, sends If-None-Match, and
  // we answer 304 (above) when the file hasn't changed. This is the only way
  // to get "new flash -> fresh UI" without a 24 h stale window or the user
  // having to hard-reload. The overhead per asset is a single round-trip
  // with an empty body — negligible on LAN.
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "ETag", etag);

  // Detach large files to a worker task so the single httpd thread isn't
  // monopolised. Without this, a Swagger-UI page load (`/docs/ui.js` ≈
  // 400 KiB) blocks /api/state, /pages/dsp.js, etc., for the duration of
  // the transfer — which manifests as "EQ page won't load" if the user
  // has a docs tab open. Async path also handles TCP backpressure more
  // gracefully (yields between chunks instead of hot-spinning send_chunk).
  if ((size_t)st.st_size >= kAsyncThreshold) {
    esp_err_t rc = start_async_file_send(req, fd, (size_t)st.st_size);
    if (rc == ESP_OK) {
      // Worker owns fd + req from here. Returning ESP_OK frees the httpd
      // thread immediately to accept the next request.
      return ESP_OK;
    }
    // Pool full or async setup failed — fall back to inline send. Better
    // a slow request than a dropped one. We still own fd at this point.
    ESP_LOGW(TAG, "async pool busy, sending %s inline (%u B)",
             path, (unsigned)st.st_size);
  }

  return inline_file_send(req, fd);
}

// Send the inline Wi-Fi setup page. Used both for the captive `/` redirect
// and for OS captive-portal probe URLs (Android `/generate_204`, iOS
// `/hotspot-detect.html`, Windows `/connecttest.txt`, Firefox `/canonical.html`,
// GNOME `/check_network_status.txt`, RFC 8908 `/.well-known/captive-portal/*`).
// Ubuntu NetworkManager uses `http://connectivity-check.ubuntu.com/` (path `/`,
// host resolved by our captive DNS) — that is already covered by `/` in AP-only
// mode; the extra paths catch Linux stacks that probe CAPPORT / GNOME first.
// Returning a 200 + non-trivial HTML on those probes is what makes the OS
// pop its built-in captive sheet automatically.
static esp_err_t send_setup_page(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, SETUP_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_captive_probe(httpd_req_t* req) {
  // In AP-only mode, return the setup page so phones surface their
  // captive-portal sheet automatically. In STA mode the probe still works
  // (e.g. Android explicitly hitting our IP from the same LAN) — pretend
  // to be the upstream "no captive" responder so we don't trigger a
  // false-positive captive popup on the user's normal network.
  const bool ap_only = s_wifi && s_wifi->isApOnly();
  if (ap_only) return send_setup_page(req);

  const char* uri = req->uri;
  if (strstr(uri, "/generate_204") || strstr(uri, "/gen_204")) {
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, nullptr, 0);
  }
  // iOS / Windows / Firefox style: return the canonical "success" body.
  static const char kIosSuccess[] =
      "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, kIosSuccess, HTTPD_RESP_USE_STRLEN);
}

static bool path_is_captive_probe(const char* path) {
  static const char* const kProbes[] = {
      "/hotspot-detect.html",     // iOS / macOS
      "/library/test/success.html",
      "/generate_204",             // Android / ChromeOS
      "/gen_204",
      "/connecttest.txt",          // Windows
      "/ncsi.txt",
      "/redirect",
      "/canonical.html",           // Firefox
      "/check_network_status.txt", // GNOME / nmcheck.gnome.org (many Linux desktops)
      // RFC 8908 CAPPORT + common variants (systemd, NetworkManager builds, phones).
      "/.well-known/captive-portal",
      "/.well-known/captive-portal/",
  };
  for (const char* p : kProbes) {
    if (strcmp(path, p) == 0) return true;
  }
  // Subpaths under CAPPORT, e.g. `/.well-known/captive-portal/api` or `.../status`.
  if (!strncmp(path, "/.well-known/captive-portal/", 28)) return true;
  return false;
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

  // Captive OS probes share this catch-all so we do not spend 11 URI slots
  // that previously overflowed the table and dropped `/*` (SPA pages 404).
  if (path_is_captive_probe(path)) {
    return handle_captive_probe(req);
  }

  // Route rewrites for non-file URLs that should still map to static assets:
  //   /                -> /spiffs/web/index.html           (SPA shell)
  //   /docs or /docs/  -> /spiffs/web/docs/index.html      (Swagger UI)
  //   /openapi.yaml    -> /spiffs/web/api/openapi.yaml     (API spec)
  //   /COPYING         -> /spiffs/web/COPYING.txt          (GPL-3.0 licence)
  //   anything else    -> /spiffs/web<path>
  char fs_path[160];
  if (path[0] == '\0' || strcmp(path, "/") == 0) {
    snprintf(fs_path, sizeof(fs_path), "/spiffs/web/index.html");
  } else if (strcmp(path, "/docs") == 0 || strcmp(path, "/docs/") == 0) {
    snprintf(fs_path, sizeof(fs_path), "/spiffs/web/docs/index.html");
  } else if (strcmp(path, "/openapi.yaml") == 0) {
    snprintf(fs_path, sizeof(fs_path), "/spiffs/web/api/openapi.yaml");
  } else if (strcmp(path, "/COPYING") == 0) {
    // "Appropriate Legal Notices" surface required by GPL §5(d) — the SPA
    // links here from the About screen (`web/pages/system.js`).
    snprintf(fs_path, sizeof(fs_path), "/spiffs/web/COPYING.txt");
  } else {
    snprintf(fs_path, sizeof(fs_path), "/spiffs/web%s", path);
  }

  esp_err_t r = serve_file(req, fs_path, mime_for(fs_path));
  if (r == ESP_OK) return ESP_OK;

  // For SPA deep-links (`/mode`, `/dsp`, …) we use the History API in the
  // client, so the browser sends GET /mode on reload or bookmark. Serve
  // index.html and let the router take over after JS boots. Only do this
  // if the path has no extension (i.e. it's a "page", not a missing asset
  // like `/foo.js.gz`) AND isn't an `/api/...` miss (which should 404 so
  // the SPA's fetch() rejects visibly).
  const bool looks_like_page =
      !strchr(path, '.') && strncmp(path, "/api/", 5) != 0;
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
  if (s > 3) s = 3;
  cfg->setAudioSource((AudioSource)s);
  cfg->save();

  unsigned pu = (unsigned)prev;
  unsigned nu = (unsigned)cfg->getAudioSource();
  const bool reboot = (pu != nu);
  if (reboot) {
    ESP_LOGI(TAG, "Audio source %u -> %u: rebooting to reconfigure", pu, nu);
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

  httpd_resp_set_type(req, "application/json");
  if (ssid[0] == '\0') {
    return httpd_resp_send(req, "{\"ok\":false,\"reason\":\"FAIL\",\"detail\":\"missing ssid\"}",
                           HTTPD_RESP_USE_STRLEN);
  }
  if (!s_wifi) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no wifi");
  }

  WifiProbeResult pr{};
  bool ok = s_wifi->probeCredentials(ssid, pass, 12000, &pr);
  if (!ok) {
    char out[256];
    int n = snprintf(out, sizeof(out),
                     "{\"ok\":false,\"reason\":\"%s\",\"detail\":\"%s\"}",
                     pr.reason[0] ? pr.reason : "FAIL",
                     pr.detail[0] ? pr.detail : "probe failed");
    return httpd_resp_send(req, out, n);
  }
  if (!s_wifi->commitValidated()) {
    return httpd_resp_send(req, "{\"ok\":false,\"reason\":\"FAIL\",\"detail\":\"commit failed\"}",
                           HTTPD_RESP_USE_STRLEN);
  }
  ESP_LOGI(TAG, "WiFi credentials validated and saved; rebooting.");
  scheduleRestart(800);
  return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

bool HttpServer::begin(ConfigManager* cfg, WifiManager* wifi) {
  cfg_ = cfg;
  wifi_ = wifi;
  s_wifi = wifi;  // expose to static handlers
  apiSetWifiManager(wifi);
  if (!cfg_) return false;
  if (server_) return true;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.lru_purge_enable = true;
  // Phase 1+ registers many /api/* handlers + legacy + 11 captive probes +
  // 2 SSE + static /*. Bluetooth GET/POST added two more (~49 without DLNA).
  // DLNA adds 8. 48 overflowed: `/*` never registered, so `/pages/*.js` 404'd.
  config.max_uri_handlers = 64;
  // Enable wildcard matching so `/*` actually fires for /foo.js.gz, /vendor/…,
  // /pages/…. Without this the catch-all only matches the literal string `/*`.
  config.uri_match_fn = httpd_uri_match_wildcard;
  // Modern browsers open up to 6 parallel connections per host for assets,
  // plus we keep one long-lived detached socket for the SSE log stream
  // (see idf_logs.cpp). The default (7) leaves exactly zero headroom, so
  // the SSE plus a page reload would lock other requests out until LRU
  // purge kicked in. Keep below CONFIG_LWIP_MAX_SOCKETS (see sdkconfig.defaults);
  // httpd also reserves a few FDs internally — values above the cap can misbehave.
  // NOTE: 10 is intentional. Bumping past this caused Bluedroid `hci_hal_env_init`
  // to fail at `xQueueCreate` because httpd's session table grew the internal-DRAM
  // working set just enough to starve the (small but cap-restricted) BT init heap.
  config.max_open_sockets = 10;
  config.backlog_conn = 8;
  // Tighter timeouts so a half-closed peer frees its socket sooner rather
  // than sitting in the LRU list. 5 s is plenty for any legitimate LAN
  // request but short enough that dead idle connections don't linger.
  config.recv_wait_timeout = 5;
  config.send_wait_timeout = 5;
  // The static + API handlers allocate a few KiB of locals on the stack
  // (json assembly, 1 KiB file-serving buffer). Default of 4 KiB is tight
  // with FP prints in cJSON.
  // Large JSON handlers (e.g. GET /api/state) need comfortable headroom.
  config.stack_size = 8 * 1024;
  // Run httpd on Core 1, NOT Core 0. Counter-intuitive — earlier code pinned
  // it to Core 0 to "stay near lwip/Wi-Fi/BT" — but during BT music playback
  // Core 0 is already saturated by:
  //   - Wi-Fi task              (prio 23)
  //   - BT controller           (prio 23)
  //   - lwip TCPIP              (prio 18)
  //   - Bluedroid (BTC/BTA)     (prio 12–13)
  //   - …everything else        (≤ 5)
  // …leaving the default-priority httpd task (prio 5) effectively starved.
  // Symptom: `/api/state` polls take seconds, the SPA UI freezes, browser
  // retries fire `httpd_accept_conn errno 113` bursts (client gives up before
  // accept() runs).
  //
  // Core 1 hosts `audio_task` at prio 20, but that task spends ~75% of every
  // second blocked in `i2s_channel_write` (DMA-paced — see [Audio] dma_wait
  // stats). httpd at prio 10 runs uninterrupted in that 75% window and
  // immediately yields the 25% busy slice to audio. Lwip socket calls go
  // through the TCPIP thread message queue regardless of caller core, so
  // there is no extra IPC penalty for serving requests from Core 1.
  //
  // Priority 10 chosen so:
  //   - `audio_task` (20) preempts httpd unconditionally → audio is uncontended
  //   - httpd preempts `dlna-mp3` (5), `voice` (3), idle (0) on Core 1
  //   - httpd stays below lwip TCPIP (18) so the network stack can drain
  //     packets while httpd is parsing one.
  config.core_id = 1;
  config.task_priority = 10;

  httpd_handle_t h = nullptr;
  if (httpd_start(&h, &config) != ESP_OK) return false;
  server_ = (void*)h;

  // Legacy compat endpoints. The SPA prefers the richer JSON endpoints in
  // idf_api.cpp, but these are retained so existing tools/probes keep
  // working:
  //   GET  /api/status        — scalar snapshot (Phase 1 ships GET /api/state)
  //   GET  /api/volume?v=N    — registered in idf_api.cpp (JSON l/r + legacy scalar)
  //   POST /api/source?s=N    — old Bluetooth/Network toggle
  //   POST /api/wifi          — form-encoded creds (also driven by the new
  //                             /wifi SPA page and the inline AP-mode portal)
  httpd_uri_t status_uri{ .uri="/api/status",   .method=HTTP_GET,  .handler=handle_status,   .user_ctx=cfg_ };
  httpd_uri_t src_uri{    .uri="/api/source",   .method=HTTP_POST, .handler=handle_source,   .user_ctx=cfg_ };
  httpd_uri_t wifi_uri{   .uri="/api/wifi",     .method=HTTP_POST, .handler=handle_wifi,     .user_ctx=cfg_ };
  httpd_register_uri_handler(h, &status_uri);
  httpd_register_uri_handler(h, &src_uri);
  httpd_register_uri_handler(h, &wifi_uri);

  // Phase 1 JSON API (POST /api/state, POST /api/volume JSON, /api/mode,
  // /api/channels, /api/eq, /api/reset, …). Lives in idf_api.cpp.
  registerApiHandlers(h, cfg_);

  // SSE endpoint for the Logs page. Registered before the catch-all so
  // /api/logs/stream wins over the static handler's /* pattern.
  geelen_idf::logsRegisterStream(h);

  // SSE endpoint for the SPA's push-based device state. Same registration
  // ordering rationale as the logs stream — must be ahead of `/*`.
  geelen_idf::stateStreamRegister(h);

  // Catch-all static handler, registered last so the /api/* URIs win by
  // prefix-specificity above it. Captive probes are handled inside
  // handle_static (no extra URI slots).
  httpd_uri_t static_uri{ .uri="/*", .method=HTTP_GET, .handler=handle_static, .user_ctx=cfg_ };
  if (httpd_register_uri_handler(h, &static_uri) != ESP_OK) {
    ESP_LOGE(TAG, "failed to register static /* — raise max_uri_handlers");
  }

  ESP_LOGI(TAG, "HTTP server started (static from /spiffs/web/, JSON API on /api/*)");
  return true;
}

}  // namespace geelen_idf

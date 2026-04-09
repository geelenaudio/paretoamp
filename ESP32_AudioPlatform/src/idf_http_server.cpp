// ESP-IDF HTTP server implementation (esp_http_server)

#include "idf_http_server.h"
#include "idf_config_manager.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cstdio>

namespace geelen_idf {

static const char* TAG = "HTTP";

static const char INDEX_HTML[] = R"raw(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Geelen Audio</title></head><body>
<h1>Geelen Audio Amp</h1>
<p>Volume: <span id="vol">--</span> <button onclick="setVol(0)">0</button><button onclick="setVol(50)">50</button><button onclick="setVol(100)">100</button></p>
<p>Source: <span id="src">--</span> <button onclick="setSource(0)">Bluetooth</button><button onclick="setSource(1)">Network</button><button onclick="setSource(2)">Spotify</button></p>
<p>WiFi: <input id="ssid" placeholder="SSID"/><input id="pass" type="password" placeholder="Password"/><button onclick="saveWifi()">Save</button></p>
<p><a href="/api/status">API status</a></p>
<script>
function saveWifi(){
  var s=document.getElementById('ssid').value;
  var p=document.getElementById('pass').value;
  fetch('/api/wifi',{
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)
  }).then(r=>r.json()).then(d=>{
    alert(d.ok ? 'Saved. Device will reboot and try WiFi.' : 'Save failed');
  });
}
function setVol(v){
  fetch('/api/volume?v='+v).then(r=>r.json()).then(d=>{
    document.getElementById('vol').textContent=d.volume;
  });
}
function setSource(s){
  fetch('/api/source?s='+s,{method:'POST'}).then(r=>r.json()).then(d=>{
    document.getElementById('src').textContent=d.source===0?'Bluetooth':(d.source===1?'Network':'Spotify');
  });
}
fetch('/api/status').then(r=>r.json()).then(d=>{
  document.getElementById('vol').textContent=d.volume||'--';
  document.getElementById('src').textContent=d.source===0?'Bluetooth':(d.source===1?'Network':'Spotify');
});
</script></body></html>
)raw";

static void scheduleRestart(unsigned delay_ms) {
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

static esp_err_t handle_root(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_status(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  char out[256];
  int n = snprintf(out, sizeof(out),
                   "{\"volume\":%u,\"source\":%u,\"preset\":%u}",
                   (unsigned)cfg->getVolume(),
                   (unsigned)cfg->getAudioSource(),
                   (unsigned)cfg->getPresetId());
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
  if (s > 2) s = 2;
  cfg->setAudioSource((AudioSource)s);
  cfg->save();

  // BT and Spotify stacks are started at boot based on saved source.
  // Reboot when entering/leaving either mode.
  unsigned pu = (unsigned)prev;
  unsigned nu = (unsigned)cfg->getAudioSource();
  const bool bt_edge = (nu == 0u || pu == 0u);
  const bool sp_edge = (nu == 2u || pu == 2u);
  if (pu != nu && (bt_edge || sp_edge)) {
    scheduleRestart(500);
  }

  char out[64];
  int n = snprintf(out, sizeof(out), "{\"source\":%u}", (unsigned)cfg->getAudioSource());
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, out, n);
}

static esp_err_t handle_wifi(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  // Read body (application/x-www-form-urlencoded)
  int total = req->content_len;
  if (total <= 0 || total > 512) total = 512;
  char body[513];
  int r = httpd_req_recv(req, body, total);
  if (r < 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
  body[r] = '\0';

  // Very small form parser: look for ssid=...&pass=...
  auto getField = [&](const char* key, char* out, size_t out_sz) {
    const char* p = strstr(body, key);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(key);
    const char* end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    // NOTE: not URL-decoding here; good enough for ASCII SSIDs in early bring-up.
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
  // `/spotify_info` pairing uses heavier C++ parsing/crypto; default stack can overflow.
  config.stack_size = 12288;

  httpd_handle_t h = nullptr;
  if (httpd_start(&h, &config) != ESP_OK) return false;
  server_ = (void*)h;

  httpd_uri_t root_uri{ .uri="/", .method=HTTP_GET, .handler=handle_root, .user_ctx=cfg_ };
  httpd_uri_t status_uri{ .uri="/api/status", .method=HTTP_GET, .handler=handle_status, .user_ctx=cfg_ };
  httpd_uri_t vol_uri{ .uri="/api/volume", .method=HTTP_GET, .handler=handle_volume, .user_ctx=cfg_ };
  httpd_uri_t src_uri{ .uri="/api/source", .method=HTTP_POST, .handler=handle_source, .user_ctx=cfg_ };
  httpd_uri_t wifi_uri{ .uri="/api/wifi", .method=HTTP_POST, .handler=handle_wifi, .user_ctx=cfg_ };

  httpd_register_uri_handler(h, &root_uri);
  httpd_register_uri_handler(h, &status_uri);
  httpd_register_uri_handler(h, &vol_uri);
  httpd_register_uri_handler(h, &src_uri);
  httpd_register_uri_handler(h, &wifi_uri);

  ESP_LOGI(TAG, "HTTP server started");
  return true;
}

}  // namespace geelen_idf


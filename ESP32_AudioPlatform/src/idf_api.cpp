// Phase 1 JSON API handlers. See idf_api.h for scope.

#include "idf_api.h"
#include "idf_config_manager.h"
#include "idf_cpu_sampler.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "cJSON.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

namespace geelen_idf {

static const char* TAG = "API";

static const char* PRESETS_DIR = "/spiffs/presets";

// ---- Utilities ------------------------------------------------------------

// Forward-declared here so reset handler can use it without a circular header.
extern void scheduleRestart(unsigned delay_ms);

// Weak hooks into main so the API can push live config into the audio
// engine without pulling the engine into this translation unit. main
// provides strong overrides that forward to g_engine. Declared once here
// and called from the relevant handlers.
extern "C" void on_volume_changed()   __attribute__((weak));
extern "C" void on_channels_changed() __attribute__((weak));
// Reports the peak gain (dB) of the currently-enabled EQ cascade(s),
// computed by sweeping the engine's biquad responses. Returns 0 if the
// hook is absent so the UI still has a usable number.
extern "C" float compute_eq_peak_db() __attribute__((weak));

static esp_err_t send_json(httpd_req_t* req, cJSON* root) {
  char* out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!out) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t r = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
  free(out);
  return r;
}

static esp_err_t send_err_json(httpd_req_t* req, httpd_err_code_t code, const char* msg) {
  cJSON* r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "error", msg ? msg : "error");
  char* out = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_status(req, code == HTTPD_400_BAD_REQUEST ? "400 Bad Request"
                            : code == HTTPD_404_NOT_FOUND   ? "404 Not Found"
                            : "500 Internal Server Error");
  esp_err_t rc = httpd_resp_send(req, out ? out : "{}", HTTPD_RESP_USE_STRLEN);
  free(out);
  return rc;
}

static esp_err_t send_ok(httpd_req_t* req) {
  cJSON* r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", true);
  return send_json(req, r);
}

// Read the request body into a fresh buffer and cJSON-parse it. Returns the
// parsed root (owned by caller) or nullptr on any failure (and already sent
// an error response). MAX_BODY bounds us at 8 KiB — generous headroom over
// the largest expected request (a full EQ curve with names), yet small
// enough to fit comfortably in heap.
static cJSON* parse_body(httpd_req_t* req) {
  static constexpr int MAX_BODY = 8192;
  int total = req->content_len;
  if (total <= 0) {
    send_err_json(req, HTTPD_400_BAD_REQUEST, "empty body");
    return nullptr;
  }
  if (total > MAX_BODY) {
    send_err_json(req, HTTPD_400_BAD_REQUEST, "body too large");
    return nullptr;
  }
  char* buf = (char*)malloc(total + 1);
  if (!buf) {
    send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    return nullptr;
  }
  int got = 0;
  while (got < total) {
    int r = httpd_req_recv(req, buf + got, total - got);
    if (r <= 0) { free(buf); send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv"); return nullptr; }
    got += r;
  }
  buf[got] = '\0';
  cJSON* root = cJSON_ParseWithLength(buf, got);
  free(buf);
  if (!root) {
    send_err_json(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    return nullptr;
  }
  return root;
}

static int jint(const cJSON* o, const char* k, int fallback) {
  const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
  if (cJSON_IsNumber(v)) return (int)v->valuedouble;
  return fallback;
}
static double jdouble(const cJSON* o, const char* k, double fallback) {
  const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
  if (cJSON_IsNumber(v)) return v->valuedouble;
  return fallback;
}
static bool jbool(const cJSON* o, const char* k, bool fallback) {
  const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
  if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
  if (cJSON_IsNumber(v)) return v->valueint != 0;
  return fallback;
}
static const char* jstr(const cJSON* o, const char* k, const char* fallback) {
  const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
  if (cJSON_IsString(v) && v->valuestring) return v->valuestring;
  return fallback;
}

static const char* modeName(AudioSource s) {
  switch (s) {
    case AudioSource::Bluetooth: return "bluetooth";
    case AudioSource::Network:   return "snapclient";
    case AudioSource::Dlna:      return "dlna";
  }
  return "bluetooth";
}

static bool parseModeName(const char* s, AudioSource* out) {
  if (!s || !out) return false;
  if (!strcmp(s, "bluetooth") || !strcmp(s, "bt")) { *out = AudioSource::Bluetooth; return true; }
  if (!strcmp(s, "snapclient") || !strcmp(s, "snap") || !strcmp(s, "network")) {
    *out = AudioSource::Network; return true;
  }
  if (!strcmp(s, "dlna") || !strcmp(s, "upnp")) { *out = AudioSource::Dlna; return true; }
  return false;
}

static cJSON* bandToJson(const ParametricBand& b) {
  cJSON* o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "freq", b.freq_hz);
  cJSON_AddNumberToObject(o, "gain_db", b.gain_db);
  cJSON_AddNumberToObject(o, "q", b.q);
  return o;
}

static bool jsonToBand(const cJSON* o, ParametricBand* out) {
  if (!cJSON_IsObject(o) || !out) return false;
  out->freq_hz = (float)jdouble(o, "freq", out->freq_hz);
  out->gain_db = (float)jdouble(o, "gain_db", out->gain_db);
  out->q       = (float)jdouble(o, "q", out->q);
  return true;
}

static cJSON* curveToJson(const ParametricBand bands[PARAMETRIC_BANDS], bool enabled) {
  cJSON* o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "enabled", enabled);
  cJSON* arr = cJSON_CreateArray();
  for (size_t i = 0; i < PARAMETRIC_BANDS; ++i) cJSON_AddItemToArray(arr, bandToJson(bands[i]));
  cJSON_AddItemToObject(o, "bands", arr);
  return o;
}

// ---- /api/state (aggregated snapshot) ------------------------------------

static esp_err_t handle_get_state(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  cJSON* root = cJSON_CreateObject();

  char dev_name[DEV_NAME_MAX];
  cfg->getDeviceName(dev_name, sizeof(dev_name));
  cJSON_AddStringToObject(root, "device_name", dev_name);

  const esp_app_desc_t* desc = esp_app_get_description();
  cJSON_AddStringToObject(root, "firmware", desc ? desc->version : "dev");

  cJSON_AddStringToObject(root, "mode", modeName(cfg->getAudioSource()));

  cJSON* vol = cJSON_CreateObject();
  cJSON_AddNumberToObject(vol, "l", cfg->getVolumeL());
  cJSON_AddNumberToObject(vol, "r", cfg->getVolumeR());
  cJSON_AddBoolToObject(vol, "muted", cfg->isMuted());
  cJSON_AddItemToObject(root, "volume", vol);

  cJSON* ch = cJSON_CreateObject();
  cJSON_AddStringToObject(ch, "stereo_mono",
                          cfg->getStereoMode() == StereoMode::Mono ? "mono" : "stereo");
  cJSON_AddBoolToObject(ch, "l_on", cfg->getChannelLOn());
  cJSON_AddBoolToObject(ch, "r_on", cfg->getChannelROn());
  cJSON_AddItemToObject(root, "channels", ch);

  cJSON* snap = cJSON_CreateObject();
  char host[SNAPCAST_HOST_MAX];
  cfg->getSnapcastHost(host, sizeof(host));
  cJSON_AddStringToObject(snap, "host", host);
  cJSON_AddNumberToObject(snap, "port", cfg->getSnapcastPort());
  cJSON_AddItemToObject(root, "snapcast", snap);

  cJSON* dlna = cJSON_CreateObject();
  cJSON_AddBoolToObject(dlna, "enabled", cfg->getDlnaEnabled());
  cJSON_AddStringToObject(dlna, "name", dev_name);
  cJSON_AddItemToObject(root, "dlna", dlna);

  cJSON* eq = cJSON_CreateObject();
  ParametricBand cal[PARAMETRIC_BANDS], pref[PARAMETRIC_BANDS];
  cfg->getEqCalibration(cal);
  cfg->getEqPreference(pref);
  cJSON_AddItemToObject(eq, "calibration", curveToJson(cal, cfg->getEqCalibrationEnabled()));
  cJSON_AddItemToObject(eq, "preference",  curveToJson(pref, cfg->getEqPreferenceEnabled()));
  cJSON* hr = cJSON_CreateObject();
  cJSON_AddStringToObject(hr, "mode",
                          cfg->getHeadroomMode() == HeadroomMode::Manual ? "manual" : "auto");
  cJSON_AddNumberToObject(hr, "manual_db", cfg->getHeadroomManualDb());
  // auto_db is negative the peak gain of the enabled cascades. A peak of
  // +6 dB -> auto_db of -6 dB (so total gain stays at or below unity).
  const float peak_db = &compute_eq_peak_db ? compute_eq_peak_db() : 0.f;
  cJSON_AddNumberToObject(hr, "auto_db", (double)(peak_db > 0.f ? -peak_db : 0.f));
  cJSON_AddItemToObject(eq, "headroom", hr);
  cJSON_AddNumberToObject(eq, "active_slot", cfg->getEqPreferenceSlot());
  cJSON_AddItemToObject(root, "eq", eq);

  return send_json(req, root);
}

// ---- /api/system (runtime metrics) ---------------------------------------

// Best-effort runtime snapshot: uptime, free heap, PSRAM, chip, WiFi link
// info, CPU load. CPU load comes from the idle-hook sampler
// (idf_cpu_sampler); WiFi from esp_wifi_sta_get_ap_info. Everything here is
// cheap enough to be called once per second from the SPA Overview page.
static esp_err_t handle_get_system(httpd_req_t* req) {
  cJSON* root = cJSON_CreateObject();

  // Uptime from the monotonic timer.
  uint64_t us = esp_timer_get_time();
  cJSON_AddNumberToObject(root, "uptime_s", (double)(us / 1000000ULL));

  cJSON_AddNumberToObject(root, "free_heap",        (double)esp_get_free_heap_size());
  cJSON_AddNumberToObject(root, "min_free_heap",    (double)esp_get_minimum_free_heap_size());
  cJSON_AddNumberToObject(root, "free_psram",       (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  esp_chip_info_t chip{};
  esp_chip_info(&chip);
  cJSON* c = cJSON_CreateObject();
  const char* model = "ESP32";
  switch (chip.model) {
    case CHIP_ESP32:   model = "ESP32";   break;
    case CHIP_ESP32S2: model = "ESP32-S2"; break;
    case CHIP_ESP32S3: model = "ESP32-S3"; break;
    case CHIP_ESP32C3: model = "ESP32-C3"; break;
    default: break;
  }
  cJSON_AddStringToObject(c, "model", model);
  cJSON_AddNumberToObject(c, "revision", chip.revision);
  cJSON_AddNumberToObject(c, "cores", chip.cores);
  cJSON_AddItemToObject(root, "chip", c);

  // WiFi / IP. Only available in STA mode once associated.
  cJSON* wifi = cJSON_CreateObject();
  wifi_ap_record_t ap{};
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    cJSON_AddBoolToObject(wifi, "connected", true);
    cJSON_AddStringToObject(wifi, "ssid", (const char*)ap.ssid);
    cJSON_AddNumberToObject(wifi, "rssi", ap.rssi);
    cJSON_AddNumberToObject(wifi, "channel", ap.primary);
  } else {
    cJSON_AddBoolToObject(wifi, "connected", false);
  }
  esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (nif) {
    esp_netif_ip_info_t ip{};
    if (esp_netif_get_ip_info(nif, &ip) == ESP_OK) {
      char buf[16];
      snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
               (unsigned)((ip.ip.addr >> 0) & 0xff),
               (unsigned)((ip.ip.addr >> 8) & 0xff),
               (unsigned)((ip.ip.addr >> 16) & 0xff),
               (unsigned)((ip.ip.addr >> 24) & 0xff));
      cJSON_AddStringToObject(wifi, "ip", buf);
    }
  }
  cJSON_AddItemToObject(root, "wifi", wifi);

  uint8_t c0 = 0, c1 = 0;
  cpuSamplerRead(c0, c1);
  cJSON* cpu = cJSON_CreateObject();
  cJSON_AddNumberToObject(cpu, "core0_pct", c0);
  cJSON_AddNumberToObject(cpu, "core1_pct", c1);
  cJSON_AddItemToObject(root, "cpu", cpu);

  // Firmware identity: version / compile time / IDF version / ELF SHA256.
  // Everything here is baked at link time and survives OTA swaps.
  const esp_app_desc_t* desc = esp_app_get_description();
  cJSON* fw = cJSON_CreateObject();
  cJSON_AddStringToObject(fw, "version",  desc ? desc->version      : "dev");
  cJSON_AddStringToObject(fw, "project",  desc ? desc->project_name : "");
  cJSON_AddStringToObject(fw, "build_date", desc ? desc->date       : "");
  cJSON_AddStringToObject(fw, "build_time", desc ? desc->time       : "");
  cJSON_AddStringToObject(fw, "idf_ver",  desc ? desc->idf_ver      : "");
  // ELF SHA256, 32 bytes → 64 hex chars. Truncated server-side to avoid
  // spraying NUL terminators into the JSON.
  // `esp_app_get_elf_sha256` formats it as a NUL-terminated hex string
  // directly, so we only need a 65-byte buffer and zero post-processing.
  char sha_hex[65] = {0};
  if (esp_app_get_elf_sha256(sha_hex, sizeof(sha_hex)) > 0) {
    cJSON_AddStringToObject(fw, "elf_sha256", sha_hex);
  }
  cJSON_AddItemToObject(root, "firmware", fw);

  // Network identity: STA and softAP MACs + hostname.
  cJSON* net = cJSON_CreateObject();
  uint8_t mac[6] = {0};
  char mac_buf[18];
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
    snprintf(mac_buf, sizeof(mac_buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(net, "sta_mac", mac_buf);
  }
  if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK) {
    snprintf(mac_buf, sizeof(mac_buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(net, "ap_mac", mac_buf);
  }
  if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
    snprintf(mac_buf, sizeof(mac_buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(net, "bt_mac", mac_buf);
  }
  if (nif) {
    const char* host = nullptr;
    if (esp_netif_get_hostname(nif, &host) == ESP_OK && host) {
      cJSON_AddStringToObject(net, "hostname", host);
    }
  }
  cJSON_AddItemToObject(root, "net", net);

  return send_json(req, root);
}

// ---- /api/volume ----------------------------------------------------------

// `GET /api/volume?v=N` still works as a convenience shim for tools and for
// the Phase-0 compat fallback; POST accepts the new JSON shape.
static esp_err_t handle_get_volume(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char vbuf[16];
    if (httpd_query_key_value(query, "v", vbuf, sizeof(vbuf)) == ESP_OK) {
      int v = atoi(vbuf);
      if (v < 0) v = 0;
      if (v > 100) v = 100;
      cfg->setVolume((uint8_t)v);
      cfg->save();
      if (&on_volume_changed) on_volume_changed();
    }
  }

  cJSON* r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "l", cfg->getVolumeL());
  cJSON_AddNumberToObject(r, "r", cfg->getVolumeR());
  cJSON_AddBoolToObject(r, "muted", cfg->isMuted());
  cJSON_AddNumberToObject(r, "volume", cfg->getVolume());  // legacy scalar
  return send_json(req, r);
}

static esp_err_t handle_post_volume(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  cJSON* body = parse_body(req);
  if (!body) return ESP_FAIL;

  int l = cfg->getVolumeL();
  int r = cfg->getVolumeR();
  // `v` alone sets both channels; `l`/`r` set per-channel (and win over `v`
  // if present).
  int v = jint(body, "v", -1);
  if (v >= 0) { l = v; r = v; }
  l = jint(body, "l", l);
  r = jint(body, "r", r);
  if (l < 0) l = 0; if (l > 100) l = 100;
  if (r < 0) r = 0; if (r > 100) r = 100;
  bool muted = jbool(body, "muted", cfg->isMuted());

  cfg->setVolumeLR((uint8_t)l, (uint8_t)r);
  cfg->setMuted(muted);
  cfg->save();
  cJSON_Delete(body);

  if (&on_volume_changed) on_volume_changed();

  cJSON* rj = cJSON_CreateObject();
  cJSON_AddNumberToObject(rj, "l", cfg->getVolumeL());
  cJSON_AddNumberToObject(rj, "r", cfg->getVolumeR());
  cJSON_AddBoolToObject(rj, "muted", cfg->isMuted());
  return send_json(req, rj);
}

// ---- /api/channels --------------------------------------------------------

static esp_err_t handle_post_channels(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  cJSON* body = parse_body(req);
  if (!body) return ESP_FAIL;

  const char* sm = jstr(body, "stereo_mono",
                        cfg->getStereoMode() == StereoMode::Mono ? "mono" : "stereo");
  bool l_on = jbool(body, "l_on", cfg->getChannelLOn());
  bool r_on = jbool(body, "r_on", cfg->getChannelROn());

  StereoMode mode = StereoMode::Stereo;
  if (sm && (!strcmp(sm, "mono") || !strcmp(sm, "mix"))) mode = StereoMode::Mono;

  cfg->setStereoMode(mode);
  cfg->setChannels(l_on, r_on);
  cfg->save();
  cJSON_Delete(body);

  if (&on_channels_changed) on_channels_changed();

  cJSON* rj = cJSON_CreateObject();
  cJSON_AddStringToObject(rj, "stereo_mono", mode == StereoMode::Mono ? "mono" : "stereo");
  cJSON_AddBoolToObject(rj, "l_on", l_on);
  cJSON_AddBoolToObject(rj, "r_on", r_on);
  return send_json(req, rj);
}

// ---- /api/mode ------------------------------------------------------------

// Externally-supplied hook: lets idf_main toggle subsystems (BT/Snap/DLNA)
// when the source changes. Weak so the API layer stays decoupled from the
// actual source modules.
extern "C" void on_audio_source_changed(unsigned prev, unsigned next) __attribute__((weak));

static esp_err_t handle_post_mode(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  cJSON* body = parse_body(req);
  if (!body) return ESP_FAIL;

  const char* mode = jstr(body, "mode", nullptr);
  AudioSource next = AudioSource::Bluetooth;
  if (!mode || !parseModeName(mode, &next)) {
    cJSON_Delete(body);
    return send_err_json(req, HTTPD_400_BAD_REQUEST, "mode must be one of bluetooth|snapclient|dlna");
  }
  AudioSource prev = cfg->getAudioSource();
  cfg->setAudioSource(next);
  cfg->save();

  unsigned pu = (unsigned)prev;
  unsigned nu = (unsigned)next;
  // Any actual change reboots until the Phase-9 runtime source manager
  // lands. The BT-only path used to special-case BT edges, but switching
  // between Snapclient and DLNA also reconfigures the task graph, so the
  // simplest correct answer is "changed == reboot".
  const bool reboot = (pu != nu);

  // Let main decide how to reconfigure runtime (start/stop Snap / DLNA). If
  // the hook is absent we still reboot so NVS takes effect on next boot.
  if (&on_audio_source_changed) on_audio_source_changed(pu, nu);
  else if (reboot) scheduleRestart(500);

  cJSON_Delete(body);

  cJSON* rj = cJSON_CreateObject();
  cJSON_AddStringToObject(rj, "mode", modeName(next));
  cJSON_AddBoolToObject(rj, "reboot", reboot);
  return send_json(req, rj);
}

// ---- /api/snapcast (JSON form) -------------------------------------------

extern "C" void on_snapcast_config_changed() __attribute__((weak));

static esp_err_t handle_post_snapcast(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  cJSON* body = parse_body(req);
  if (!body) return ESP_FAIL;

  const char* host = jstr(body, "host", "");
  int port = jint(body, "port", (int)SNAPCAST_PORT_DEFAULT);
  if (port <= 0 || port > 65535) port = SNAPCAST_PORT_DEFAULT;

  cfg->setSnapcastHost(host);
  cfg->setSnapcastPort((uint16_t)port);
  cfg->save();
  cJSON_Delete(body);

  if (&on_snapcast_config_changed) on_snapcast_config_changed();

  ESP_LOGI(TAG, "Snapcast host=%s port=%d saved via JSON API", host, port);

  cJSON* rj = cJSON_CreateObject();
  cJSON_AddStringToObject(rj, "host", host);
  cJSON_AddNumberToObject(rj, "port", port);
  return send_json(req, rj);
}

// ---- /api/dlna ------------------------------------------------------------

static esp_err_t handle_post_dlna(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  cJSON* body = parse_body(req);
  if (!body) return ESP_FAIL;

  bool enabled = jbool(body, "enabled", cfg->getDlnaEnabled());
  const char* name = jstr(body, "name", nullptr);
  cfg->setDlnaEnabled(enabled);
  if (name && *name) cfg->setDeviceName(name);
  cfg->save();
  cJSON_Delete(body);

  cJSON* rj = cJSON_CreateObject();
  cJSON_AddBoolToObject(rj, "enabled", enabled);
  char dn[DEV_NAME_MAX]; cfg->getDeviceName(dn, sizeof(dn));
  cJSON_AddStringToObject(rj, "name", dn);
  return send_json(req, rj);
}

// ---- /api/device-name ----------------------------------------------------

static esp_err_t handle_post_device_name(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  cJSON* body = parse_body(req);
  if (!body) return ESP_FAIL;
  const char* n = jstr(body, "name", nullptr);
  if (!n || !*n) { cJSON_Delete(body); return send_err_json(req, HTTPD_400_BAD_REQUEST, "name required"); }
  cfg->setDeviceName(n);
  cfg->save();
  cJSON_Delete(body);

  cJSON* rj = cJSON_CreateObject();
  char dn[DEV_NAME_MAX]; cfg->getDeviceName(dn, sizeof(dn));
  cJSON_AddStringToObject(rj, "name", dn);
  return send_json(req, rj);
}

// ---- /api/eq --------------------------------------------------------------

// Hook so Phase 5 can push EQ updates into the audio engine without the API
// module having to know about AudioEngine.
extern "C" void on_eq_changed() __attribute__((weak));

static esp_err_t handle_post_eq(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  cJSON* body = parse_body(req);
  if (!body) return ESP_FAIL;

  const char* layer = jstr(body, "layer", nullptr);
  if (!layer || (strcmp(layer, "calibration") && strcmp(layer, "preference"))) {
    cJSON_Delete(body);
    return send_err_json(req, HTTPD_400_BAD_REQUEST, "layer must be calibration|preference");
  }

  const bool is_cal = !strcmp(layer, "calibration");

  // Load current curve, overlay any bands the request provides (array of
  // length 10, or partial — we iterate by index so the client can send just
  // a single edited band if they want).
  ParametricBand bands[PARAMETRIC_BANDS];
  if (is_cal) cfg->getEqCalibration(bands);
  else        cfg->getEqPreference(bands);

  const cJSON* arr = cJSON_GetObjectItemCaseSensitive(body, "bands");
  if (cJSON_IsArray(arr)) {
    int i = 0;
    const cJSON* it = nullptr;
    cJSON_ArrayForEach(it, arr) {
      if (i >= (int)PARAMETRIC_BANDS) break;
      (void)jsonToBand(it, &bands[i]);
      ++i;
    }
  }

  bool enabled_before = is_cal ? cfg->getEqCalibrationEnabled() : cfg->getEqPreferenceEnabled();
  bool enabled = jbool(body, "enabled", enabled_before);

  if (is_cal) {
    cfg->setEqCalibration(bands);
    cfg->setEqCalibrationEnabled(enabled);
  } else {
    cfg->setEqPreference(bands);
    cfg->setEqPreferenceEnabled(enabled);
  }
  cfg->save();
  cJSON_Delete(body);

  if (&on_eq_changed) on_eq_changed();

  // Reply with the full curve as stored (post-clamp) so the UI can
  // reconcile. We also include the updated headroom block — any EQ edit
  // can change the peak gain and thus the auto headroom.
  cJSON* rj = curveToJson(bands, enabled);
  cJSON_AddStringToObject(rj, "layer", layer);
  cJSON* hr = cJSON_CreateObject();
  cJSON_AddStringToObject(hr, "mode",
                          cfg->getHeadroomMode() == HeadroomMode::Manual ? "manual" : "auto");
  cJSON_AddNumberToObject(hr, "manual_db", cfg->getHeadroomManualDb());
  const float peak_db = &compute_eq_peak_db ? compute_eq_peak_db() : 0.f;
  cJSON_AddNumberToObject(hr, "auto_db", (double)(peak_db > 0.f ? -peak_db : 0.f));
  cJSON_AddItemToObject(rj, "headroom", hr);
  return send_json(req, rj);
}

static esp_err_t handle_post_eq_headroom(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  cJSON* body = parse_body(req);
  if (!body) return ESP_FAIL;

  const char* m = jstr(body, "mode", "auto");
  HeadroomMode mode = (m && !strcmp(m, "manual")) ? HeadroomMode::Manual : HeadroomMode::Auto;
  double manual_db = jdouble(body, "manual_db", cfg->getHeadroomManualDb());
  if (manual_db > 0.0) manual_db = 0.0;
  cfg->setHeadroom(mode, (float)manual_db);
  cfg->save();
  cJSON_Delete(body);

  if (&on_eq_changed) on_eq_changed();

  cJSON* rj = cJSON_CreateObject();
  cJSON_AddStringToObject(rj, "mode", mode == HeadroomMode::Manual ? "manual" : "auto");
  cJSON_AddNumberToObject(rj, "manual_db", cfg->getHeadroomManualDb());
  const float peak_db = &compute_eq_peak_db ? compute_eq_peak_db() : 0.f;
  cJSON_AddNumberToObject(rj, "auto_db", (double)(peak_db > 0.f ? -peak_db : 0.f));
  return send_json(req, rj);
}

// ---- /api/eq/presets -----------------------------------------------------

// On-disk slot layout. 32 B name prefix + 10 × 12 B bands = 152 B. Plus
// reserved padding so struct evolution doesn't break old slots.
struct PresetFile {
  char name[PRESET_NAME_MAX];
  ParametricBand bands[PARAMETRIC_BANDS];
  uint8_t reserved[8];  // pad to 168 B
};

static void presetPath(char out[64], size_t idx) {
  snprintf(out, 64, "%s/pref_%u.bin", PRESETS_DIR, (unsigned)idx);
}

static bool presetRead(size_t idx, PresetFile* out) {
  char path[64]; presetPath(path, idx);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  ssize_t r = read(fd, out, sizeof(*out));
  close(fd);
  return r == (ssize_t)sizeof(*out);
}

static bool presetWrite(size_t idx, const PresetFile* in) {
  (void)mkdir(PRESETS_DIR, 0777);
  char path[64]; presetPath(path, idx);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) return false;
  ssize_t w = write(fd, in, sizeof(*in));
  close(fd);
  return w == (ssize_t)sizeof(*in);
}

static bool presetDelete(size_t idx) {
  char path[64]; presetPath(path, idx);
  return remove(path) == 0;
}

static esp_err_t handle_get_presets(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  cJSON* root = cJSON_CreateObject();
  uint8_t active = cfg->getEqPreferenceSlot();
  if (active == 0xFF) cJSON_AddNullToObject(root, "active");
  else                cJSON_AddNumberToObject(root, "active", active);

  cJSON* slots = cJSON_CreateArray();
  for (size_t i = 0; i < PRESET_SLOT_COUNT; ++i) {
    cJSON* s = cJSON_CreateObject();
    cJSON_AddNumberToObject(s, "index", i);
    PresetFile pf{};
    const bool saved = presetRead(i, &pf);
    if (saved) {
      pf.name[PRESET_NAME_MAX - 1] = '\0';
      cJSON_AddStringToObject(s, "name", pf.name);
    } else {
      char fallback[16];
      snprintf(fallback, sizeof(fallback), "Preset %u", (unsigned)(i + 1));
      cJSON_AddStringToObject(s, "name", fallback);
    }
    cJSON_AddBoolToObject(s, "saved", saved);
    cJSON_AddItemToArray(slots, s);
  }
  cJSON_AddItemToObject(root, "slots", slots);

  // Four factory defaults live in FIXED_PRESET_EQ over in config_manager; we
  // avoid an extra include and just export their names + centres here so the
  // UI can render a "Load factory preset…" menu. Full band data is fetched
  // on demand via POST .../factory/load (not yet implemented).
  cJSON* factory = cJSON_CreateArray();
  static const char* kFactoryNames[] = { "Flat", "Bass Boost", "Vocals", "Treble Clarity" };
  for (size_t i = 0; i < 4; ++i) {
    cJSON* f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "name", kFactoryNames[i]);
    cJSON_AddItemToArray(factory, f);
  }
  cJSON_AddItemToObject(root, "factory", factory);

  return send_json(req, root);
}

// Route param parser: extract `idx` from `/api/eq/presets/<idx>/<action>`.
static bool parsePresetIndex(const char* uri, size_t* out_idx, const char** out_action) {
  if (!uri) return false;
  const char* base = strstr(uri, "/api/eq/presets/");
  if (!base) return false;
  base += strlen("/api/eq/presets/");
  const char* slash = strchr(base, '/');
  char numbuf[8];
  size_t nl = slash ? (size_t)(slash - base) : strlen(base);
  if (nl == 0 || nl >= sizeof(numbuf)) return false;
  memcpy(numbuf, base, nl);
  numbuf[nl] = '\0';
  char* endp = nullptr;
  long v = strtol(numbuf, &endp, 10);
  if (!endp || *endp != '\0') return false;
  if (v < 0 || v >= (long)PRESET_SLOT_COUNT) return false;
  *out_idx = (size_t)v;
  *out_action = slash ? slash + 1 : "";
  return true;
}

static esp_err_t handle_presets_route(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  // Strip query string if any, for uri-parsing purposes.
  char uri[96] = {0};
  strncpy(uri, req->uri, sizeof(uri) - 1);
  for (char* p = uri; *p; ++p) { if (*p == '?') { *p = '\0'; break; } }

  size_t idx = 0;
  const char* action = nullptr;
  if (!parsePresetIndex(uri, &idx, &action)) {
    return send_err_json(req, HTTPD_404_NOT_FOUND, "unknown preset path");
  }

  if (req->method == HTTP_DELETE) {
    bool deleted = presetDelete(idx);
    if ((uint8_t)idx == cfg->getEqPreferenceSlot()) cfg->setEqPreferenceSlot(0xFF);
    cJSON* r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "index", (double)idx);
    cJSON_AddBoolToObject(r, "deleted", deleted);
    return send_json(req, r);
  }

  if (req->method != HTTP_POST)
    return send_err_json(req, HTTPD_404_NOT_FOUND, "only POST/DELETE here");

  if (!strcmp(action, "save")) {
    cJSON* body = parse_body(req);
    if (!body) return ESP_FAIL;
    const char* name = jstr(body, "name", nullptr);
    PresetFile pf{};
    if (name && *name) strncpy(pf.name, name, PRESET_NAME_MAX - 1);
    else snprintf(pf.name, PRESET_NAME_MAX, "Preset %u", (unsigned)(idx + 1));
    cfg->getEqPreference(pf.bands);
    const bool ok = presetWrite(idx, &pf);
    cJSON_Delete(body);
    if (!ok) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    cfg->setEqPreferenceSlot((uint8_t)idx);
    cfg->save();
    cJSON* rj = cJSON_CreateObject();
    cJSON_AddNumberToObject(rj, "index", (double)idx);
    cJSON_AddStringToObject(rj, "name", pf.name);
    cJSON_AddBoolToObject(rj, "saved", true);
    return send_json(req, rj);
  }

  if (!strcmp(action, "load")) {
    PresetFile pf{};
    if (!presetRead(idx, &pf)) return send_err_json(req, HTTPD_404_NOT_FOUND, "slot empty");
    cfg->setEqPreference(pf.bands);
    cfg->setEqPreferenceSlot((uint8_t)idx);
    cfg->save();
    if (&on_eq_changed) on_eq_changed();
    cJSON* rj = cJSON_CreateObject();
    cJSON_AddNumberToObject(rj, "index", (double)idx);
    pf.name[PRESET_NAME_MAX - 1] = '\0';
    cJSON_AddStringToObject(rj, "name", pf.name);
    cJSON_AddItemToObject(rj, "preference", curveToJson(pf.bands, cfg->getEqPreferenceEnabled()));
    return send_json(req, rj);
  }

  if (!strcmp(action, "rename")) {
    cJSON* body = parse_body(req);
    if (!body) return ESP_FAIL;
    const char* name = jstr(body, "name", nullptr);
    if (!name || !*name) { cJSON_Delete(body); return send_err_json(req, HTTPD_400_BAD_REQUEST, "name required"); }
    PresetFile pf{};
    if (!presetRead(idx, &pf)) { cJSON_Delete(body); return send_err_json(req, HTTPD_404_NOT_FOUND, "slot empty"); }
    strncpy(pf.name, name, PRESET_NAME_MAX - 1);
    pf.name[PRESET_NAME_MAX - 1] = '\0';
    const bool ok = presetWrite(idx, &pf);
    cJSON_Delete(body);
    if (!ok) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    cJSON* rj = cJSON_CreateObject();
    cJSON_AddNumberToObject(rj, "index", (double)idx);
    cJSON_AddStringToObject(rj, "name", pf.name);
    return send_json(req, rj);
  }

  return send_err_json(req, HTTPD_404_NOT_FOUND, "unknown preset action");
}

// ---- /api/reset -----------------------------------------------------------

static esp_err_t handle_post_reset(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  cJSON* body = parse_body(req);
  if (!body) return ESP_FAIL;

  const char* scope = jstr(body, "scope", "");
  bool reboot = false;
  bool handled = true;

  if (!strcmp(scope, "wifi"))              { cfg->resetWifi(); reboot = true; }
  else if (!strcmp(scope, "bluetooth"))    { cfg->resetBluetoothPairings(); reboot = true; }
  else if (!strcmp(scope, "snapcast"))     { cfg->resetSnapcast();
                                             if (&on_snapcast_config_changed) on_snapcast_config_changed(); }
  else if (!strcmp(scope, "dlna"))         { cfg->resetDlna(); }
  else if (!strcmp(scope, "eq"))           { cfg->resetEq();
                                             if (&on_eq_changed) on_eq_changed(); }
  else if (!strcmp(scope, "audio"))        { cfg->resetAudio(); }
  else if (!strcmp(scope, "all") || !strcmp(scope, "factory")) {
    cfg->factoryReset(); reboot = true;
  } else {
    handled = false;
  }

  cJSON_Delete(body);
  if (!handled) return send_err_json(req, HTTPD_400_BAD_REQUEST, "unknown scope");

  cJSON* rj = cJSON_CreateObject();
  cJSON_AddStringToObject(rj, "scope", scope);
  cJSON_AddBoolToObject(rj, "reboot", reboot);
  esp_err_t ok = send_json(req, rj);
  if (reboot) scheduleRestart(800);
  return ok;
}

// ---- Registration --------------------------------------------------------

void registerApiHandlers(void* server_handle, ConfigManager* cfg) {
  httpd_handle_t h = (httpd_handle_t)server_handle;
  if (!h) return;

  const auto reg = [&](const char* uri, httpd_method_t method, esp_err_t (*fn)(httpd_req_t*)) {
    httpd_uri_t u{};
    u.uri = uri;
    u.method = method;
    u.handler = fn;
    u.user_ctx = cfg;
    httpd_register_uri_handler(h, &u);
  };

  reg("/api/state",              HTTP_GET,    handle_get_state);
  reg("/api/system",             HTTP_GET,    handle_get_system);
  reg("/api/volume",             HTTP_POST,   handle_post_volume);
  reg("/api/channels",           HTTP_POST,   handle_post_channels);
  reg("/api/mode",               HTTP_POST,   handle_post_mode);
  reg("/api/snapcast",           HTTP_POST,   handle_post_snapcast);  // Overrides legacy form handler; JSON-only.
  reg("/api/dlna",               HTTP_POST,   handle_post_dlna);
  reg("/api/device-name",        HTTP_POST,   handle_post_device_name);
  reg("/api/eq",                 HTTP_POST,   handle_post_eq);
  reg("/api/eq/headroom",        HTTP_POST,   handle_post_eq_headroom);
  reg("/api/eq/presets",         HTTP_GET,    handle_get_presets);
  // Preset item CRUD: we register a single wildcard entry and dispatch on
  // the parsed index/action inside handle_presets_route. Saves URI slots.
  reg("/api/eq/presets/*",       HTTP_POST,   handle_presets_route);
  reg("/api/eq/presets/*",       HTTP_DELETE, handle_presets_route);
  reg("/api/reset",              HTTP_POST,   handle_post_reset);
}

}  // namespace geelen_idf

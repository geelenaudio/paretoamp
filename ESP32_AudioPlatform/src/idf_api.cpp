// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// JSON API handlers. See idf_api.h for scope. Includes /api/led (WS2812).

#include "idf_api.h"
#include "idf_config_manager.h"
#include "idf_wifi_manager.h"
#include "eq_types.h"
#include "hal/hal_board.h"
#include "hal/dac.h"
#include "hal/led.h"
#include "idf_led.h"
#include "idf_cpu_sampler.h"
#include "idf_mode_manager.h"
#include "idf_identity.h"
#include "idf_a2dp_sink.h"

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

#include "cJSON.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <strings.h>
#include <cmath>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

namespace geelen_idf {

static const char* TAG = "API";
static WifiManager* s_wifi_mgr = nullptr;

void apiSetWifiManager(WifiManager* wifi) { s_wifi_mgr = wifi; }

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
extern "C" void on_led_changed()      __attribute__((weak));
// Reports the peak gain (dB) of the currently-enabled EQ cascade(s),
// computed by sweeping the engine's biquad responses. Returns 0 if the
// hook is absent so the UI still has a usable number.
extern "C" float compute_eq_peak_db() __attribute__((weak));
extern "C" void geelen_engine_read_output_meters(float* pl, float* pr, float* rl, float* rr);
extern "C" void testToneNotifyParamsUpdated(void) __attribute__((weak));

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

static bool parse_hex_color(const char* s, uint8_t* r, uint8_t* g, uint8_t* b) {
  if (!s || s[0] != '#' || strlen(s) != 7) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  int v[6];
  for (int i = 0; i < 6; ++i) {
    v[i] = nibble(s[i + 1]);
    if (v[i] < 0) return false;
  }
  *r = (uint8_t)((v[0] << 4) | v[1]);
  *g = (uint8_t)((v[2] << 4) | v[3]);
  *b = (uint8_t)((v[4] << 4) | v[5]);
  return true;
}

static void format_hex_color(char out[8], uint8_t r, uint8_t g, uint8_t b) {
  snprintf(out, 8, "#%02x%02x%02x", r, g, b);
}

static void add_led_json(cJSON* parent, ConfigManager* cfg) {
  cJSON* led = cJSON_CreateObject();
  cJSON_AddBoolToObject(led, "present", geelen_hal::ledPresent());
  cJSON_AddNumberToObject(led, "count", (double)geelen_hal::ledPixelCount());
  cJSON_AddStringToObject(led, "phase", ledPhaseName());
  char col[8];
  format_hex_color(col, cfg->getLedR(), cfg->getLedG(), cfg->getLedB());
  cJSON_AddStringToObject(led, "color", col);
  cJSON_AddNumberToObject(led, "brightness", cfg->getLedBrightness());
  cJSON_AddItemToObject(parent, "led", led);
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
    case AudioSource::TestTone:  return "test_tone";
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
  if (!strcmp(s, "test_tone") || !strcmp(s, "testtone") || !strcmp(s, "tone")) {
    *out = AudioSource::TestTone;
    return true;
  }
  return false;
}

static const char* eqTypeToStr(geelen::EqFilterType t) {
  switch (t) {
    case geelen::EqFilterType::Bell: return "bell";
    case geelen::EqFilterType::LowShelf: return "low_shelf";
    case geelen::EqFilterType::HighShelf: return "high_shelf";
    case geelen::EqFilterType::Notch: return "notch";
    case geelen::EqFilterType::LowPass: return "low_pass";
    case geelen::EqFilterType::HighPass: return "high_pass";
    default: return "bell";
  }
}

static void strToEqType(const char* s, geelen::EqFilterType* out) {
  if (!s || !out) return;
  if (!strcasecmp(s, "low_shelf")) *out = geelen::EqFilterType::LowShelf;
  else if (!strcasecmp(s, "high_shelf")) *out = geelen::EqFilterType::HighShelf;
  else if (!strcasecmp(s, "notch")) *out = geelen::EqFilterType::Notch;
  else if (!strcasecmp(s, "low_pass")) *out = geelen::EqFilterType::LowPass;
  else if (!strcasecmp(s, "high_pass")) *out = geelen::EqFilterType::HighPass;
  else *out = geelen::EqFilterType::Bell;
}

static cJSON* bandToJson(const ParametricBand& b) {
  cJSON* o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "freq", b.freq_hz);
  cJSON_AddNumberToObject(o, "gain_db", b.gain_db);
  cJSON_AddNumberToObject(o, "q", b.q);
  cJSON_AddStringToObject(o, "type", eqTypeToStr(b.type));
  cJSON_AddNumberToObject(o, "slope_db_oct", b.slope_db_oct);
  return o;
}

static bool jsonToBand(const cJSON* o, ParametricBand* out) {
  if (!cJSON_IsObject(o) || !out) return false;
  out->freq_hz = (float)jdouble(o, "freq", out->freq_hz);
  out->gain_db = (float)jdouble(o, "gain_db", out->gain_db);
  out->q = (float)jdouble(o, "q", out->q);
  const cJSON* tj = cJSON_GetObjectItemCaseSensitive(o, "type");
  if (cJSON_IsString(tj) && tj->valuestring) strToEqType(tj->valuestring, &out->type);
  else if (cJSON_IsNumber(tj)) {
    int v = (int)cJSON_GetNumberValue(tj);
    if (v >= 0 && v <= (int)geelen::EqFilterType::HighPass) out->type = (geelen::EqFilterType)v;
  }
  const cJSON* sj = cJSON_GetObjectItemCaseSensitive(o, "slope_db_oct");
  if (cJSON_IsNumber(sj)) {
    int slo = (int)cJSON_GetNumberValue(sj);
    if (slo == 6 || slo == 12 || slo == 24) out->slope_db_oct = (uint8_t)slo;
  }
  if (out->slope_db_oct != 6 && out->slope_db_oct != 12 && out->slope_db_oct != 24) out->slope_db_oct = 12;
  out->_pad[0] = out->_pad[1] = 0;
  return true;
}

// EQ backend for `/api/state.eq.backend` — mirrors what
// `AudioEngine`/`geelen_hal::dacApplyPeqBands` are actually doing. Values:
//   "software"             — cascade lives in the AudioEngine (Amped).
//   "hardware"             — DAC applied the biquads in HW (Louder OK).
//   "software (fallback)"  — Louder HW upload failed, dropped back to SW.
static const char* g_eq_backend = "software";
extern "C" void geelen_eq_set_backend(const char* label) {
  if (label && *label) g_eq_backend = label;
}

static cJSON* bandsToJsonArray(const ParametricBand bands[PARAMETRIC_BANDS]) {
  cJSON* arr = cJSON_CreateArray();
  for (size_t i = 0; i < PARAMETRIC_BANDS; ++i) cJSON_AddItemToArray(arr, bandToJson(bands[i]));
  return arr;
}

static void fill_eq_object(cJSON* eq, ConfigManager* cfg) {
  ParametricBand left[PARAMETRIC_BANDS];
  ParametricBand right[PARAMETRIC_BANDS];
  cfg->getEqBands(left);
  cfg->getEqBandsR(right);
  const uint8_t band_count = cfg->getEqActiveBands();
  cJSON_AddBoolToObject(eq, "enabled", cfg->getEqEnabled());
  cJSON_AddNumberToObject(eq, "band_count", band_count);
  cJSON_AddNumberToObject(eq, "max_bands", (double)HAL_EQ_MAX_BANDS_ACTIVE);
  cJSON_AddStringToObject(eq, "backend", g_eq_backend);
  cJSON_AddBoolToObject(eq, "supports_split", HAL_HAS_HW_PEQ ? true : false);
  cJSON_AddStringToObject(eq, "channel_mode",
                          cfg->getEqChannelMode() == EqChannelMode::Split ? "split" : "ganged");
  cJSON_AddItemToObject(eq, "bands", bandsToJsonArray(left));
#if HAL_HAS_HW_PEQ
  cJSON_AddItemToObject(eq, "bands_r", bandsToJsonArray(right));
#endif
  cJSON* hr = cJSON_CreateObject();
  cJSON_AddStringToObject(hr, "mode",
                          cfg->getHeadroomMode() == HeadroomMode::Manual ? "manual" : "auto");
  cJSON_AddNumberToObject(hr, "manual_db", cfg->getHeadroomManualDb());
  const float peak_db = &compute_eq_peak_db ? compute_eq_peak_db() : 0.f;
  cJSON_AddNumberToObject(hr, "auto_db", (double)(peak_db > 0.f ? -peak_db : 0.f));
  cJSON_AddItemToObject(eq, "headroom", hr);
  cJSON_AddNumberToObject(eq, "active_slot", cfg->getEqActiveSlot());
}

// ---- /api/state (aggregated snapshot) ------------------------------------
//
// `buildStateJson` is the single source of truth for the wire shape. Both
// the REST handler below and the SSE state stream (idf_state_stream.cpp)
// call it so a subscriber's snapshot is byte-for-byte identical to a
// `GET /api/state` response. The caller owns the returned cJSON object.

static void fill_volume_object(cJSON* vol, ConfigManager* cfg, bool legacy_scalar) {
  uint8_t l = cfg->getVolumeL();
  uint8_t r = cfg->getVolumeR();
  const int live = a2dpLiveVolumePct();
  if (live >= 0) {
    l = r = static_cast<uint8_t>(live);
  }
  cJSON_AddNumberToObject(vol, "l", l);
  cJSON_AddNumberToObject(vol, "r", r);
  cJSON_AddBoolToObject(vol, "muted", cfg->isMuted());
  if (legacy_scalar) {
    cJSON_AddNumberToObject(vol, "volume", l > r ? l : r);
  }
}

cJSON* buildStateJson(ConfigManager* cfg) {
  cJSON* root = cJSON_CreateObject();

  char dev_name[DEV_NAME_MAX];
  cfg->getDeviceName(dev_name, sizeof(dev_name));
  cJSON_AddStringToObject(root, "device_name", dev_name);

  cJSON* ident = cJSON_CreateObject();
  identityAddJson(ident, cfg, /*include_macs=*/false);
  cJSON_AddItemToObject(root, "identity", ident);

  const esp_app_desc_t* desc = esp_app_get_description();
  cJSON_AddStringToObject(root, "firmware", desc ? desc->version : "dev");

  cJSON_AddStringToObject(root, "mode", modeName(cfg->getAudioSource()));
  cJSON_AddBoolToObject(root, "voice_feedback", cfg->getVoiceFeedbackEnabled());

  // Monotonic version, useful as an ETag analogue and so SPA clients can
  // detect dropped frames in the SSE stream.
  cJSON_AddNumberToObject(root, "_v", (double)cfg->stateVersion());

  cJSON* vol = cJSON_CreateObject();
  fill_volume_object(vol, cfg, false);
  cJSON_AddItemToObject(root, "volume", vol);

  cJSON* ch = cJSON_CreateObject();
  cJSON_AddStringToObject(ch, "stereo_mono",
                          cfg->getStereoMode() == StereoMode::Mono ? "mono" : "stereo");
  cJSON_AddBoolToObject(ch, "l_on", cfg->getChannelLOn());
  cJSON_AddBoolToObject(ch, "r_on", cfg->getChannelROn());
  cJSON_AddItemToObject(root, "channels", ch);

  add_led_json(root, cfg);

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

  // Phase 5 — Wi-Fi power-save preference. Persisted in NVS, applied at
  // boot inside `WifiManager::begin`. Exposed here so the SPA can show
  // the current setting; mutated via POST `/api/wifi/power-save`.
  cJSON* wifi_cfg = cJSON_CreateObject();
  const char* ps_str = "none";
  switch (cfg->getWifiPsMode()) {
    case WifiPsMode::None:     ps_str = "none";       break;
    case WifiPsMode::MinModem: ps_str = "min_modem";  break;
    case WifiPsMode::MaxModem: ps_str = "max_modem";  break;
  }
  cJSON_AddStringToObject(wifi_cfg, "power_save", ps_str);
  cJSON_AddStringToObject(wifi_cfg, "state",
                          s_wifi_mgr ? s_wifi_mgr->probeStateName() : "idle");
  cJSON_AddItemToObject(root, "wifi", wifi_cfg);

  // Parametric EQ (Louder may include bands_r + channel_mode).
  cJSON* eq = cJSON_CreateObject();
  fill_eq_object(eq, cfg);
  cJSON_AddItemToObject(root, "eq", eq);

  cJSON* tt = cJSON_CreateObject();
  cJSON_AddNumberToObject(tt, "freq_hz", cfg->getTestToneFreqHz());
  cJSON_AddNumberToObject(tt, "amp_db", cfg->getTestToneAmpDb());
  cJSON_AddNumberToObject(tt, "pan", cfg->getTestTonePan());
  cJSON_AddNumberToObject(tt, "mode", cfg->getTestToneMode());
  cJSON_AddNumberToObject(tt, "sweep_s", cfg->getTestToneSweepS());
  cJSON_AddNumberToObject(tt, "sweep_lo_hz", cfg->getTestToneSweepLoHz());
  cJSON_AddNumberToObject(tt, "sweep_hi_hz", cfg->getTestToneSweepHiHz());
  cJSON_AddNumberToObject(tt, "dual_f2_hz", cfg->getTestToneDualF2Hz());
  cJSON_AddItemToObject(root, "test_tone", tt);

  cJSON* bt = cJSON_CreateObject();
  cJSON_AddBoolToObject(bt, "connected", a2dpIsConnected());
  cJSON_AddBoolToObject(bt, "discoverable", a2dpIsDiscoverable());
  cJSON_AddNumberToObject(bt, "window_ms_left", (double)a2dpPairingWindowMsLeft());
  cJSON_AddItemToObject(root, "bluetooth", bt);

  return root;
}

static esp_err_t handle_get_state(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  char etag[24];
  snprintf(etag, sizeof(etag), "\"%08lx\"", (unsigned long)cfg->stateVersion());

  char inm[80] = {0};
  if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK) {
    const char* cm = inm;
    while (*cm == ' ' || *cm == '\t') ++cm;
    if (cm[0] == 'W' && cm[1] == '/') cm += 2;
    if (strcmp(cm, etag) == 0) {
      // Status first — some httpd builds reject header/body ordering otherwise.
      httpd_resp_set_status(req, "304 Not Modified");
      httpd_resp_set_hdr(req, "ETag", etag);
      httpd_resp_set_hdr(req, "Cache-Control", "no-store");
      return httpd_resp_send(req, nullptr, 0);
    }
  }

  cJSON* root = buildStateJson(cfg);
  httpd_resp_set_hdr(req, "ETag", etag);
  return send_json(req, root);
}

// ---- /api/system (runtime metrics + identity) ----------------------------

static void appendSystemMetrics(cJSON* root, bool include_audio = true) {
  uint64_t us = esp_timer_get_time();
  cJSON_AddNumberToObject(root, "uptime_s", (double)(us / 1000000ULL));
  cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
  cJSON_AddNumberToObject(root, "min_free_heap", (double)esp_get_minimum_free_heap_size());
  cJSON_AddNumberToObject(root, "free_psram", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

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

  if (!include_audio) return;
  float pl = -90.f, pr = -90.f, rl = -90.f, rr = -90.f;
  geelen_engine_read_output_meters(&pl, &pr, &rl, &rr);
  cJSON* audio = cJSON_CreateObject();
  cJSON* outm = cJSON_CreateObject();
  cJSON_AddNumberToObject(outm, "peak_l_db", (double)pl);
  cJSON_AddNumberToObject(outm, "peak_r_db", (double)pr);
  cJSON_AddNumberToObject(outm, "rms_l_db", (double)rl);
  cJSON_AddNumberToObject(outm, "rms_r_db", (double)rr);
  cJSON_AddItemToObject(audio, "out", outm);
  cJSON_AddItemToObject(root, "audio", audio);
}

static void appendSystemIdentity(cJSON* root, ConfigManager* cfg) {
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

  const esp_app_desc_t* desc = esp_app_get_description();
  cJSON* fw = cJSON_CreateObject();
  cJSON_AddStringToObject(fw, "version", desc ? desc->version : "dev");
  cJSON_AddStringToObject(fw, "project", desc ? desc->project_name : "");
  cJSON_AddStringToObject(fw, "build_date", desc ? desc->date : "");
  cJSON_AddStringToObject(fw, "build_time", desc ? desc->time : "");
  cJSON_AddStringToObject(fw, "idf_ver", desc ? desc->idf_ver : "");
  char sha_hex[65] = {0};
  if (esp_app_get_elf_sha256(sha_hex, sizeof(sha_hex)) > 0) {
    cJSON_AddStringToObject(fw, "elf_sha256", sha_hex);
  }
  cJSON_AddItemToObject(root, "firmware", fw);

  cJSON* net = cJSON_CreateObject();
  uint8_t mac[6] = {0};
  char mac_buf[18];
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
    identityFormatMac(mac, mac_buf);
    cJSON_AddStringToObject(net, "sta_mac", mac_buf);
  }
  if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK) {
    identityFormatMac(mac, mac_buf);
    cJSON_AddStringToObject(net, "ap_mac", mac_buf);
  }
  if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
    identityFormatMac(mac, mac_buf);
    cJSON_AddStringToObject(net, "bt_mac", mac_buf);
  }
  esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (nif) {
    const char* host = nullptr;
    if (esp_netif_get_hostname(nif, &host) == ESP_OK && host) {
      cJSON_AddStringToObject(net, "hostname", host);
    }
  }
  cJSON_AddItemToObject(root, "net", net);

  if (cfg) {
    cJSON* ident = cJSON_CreateObject();
    identityAddJson(ident, cfg, /*include_macs=*/true);
    cJSON_AddItemToObject(root, "identity", ident);
  }
}

static esp_err_t handle_get_system(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  cJSON* root = cJSON_CreateObject();
  appendSystemMetrics(root);
  appendSystemIdentity(root, cfg);
  return send_json(req, root);
}

static esp_err_t handle_get_system_metrics(httpd_req_t* req) {
  cJSON* root = cJSON_CreateObject();
  appendSystemMetrics(root);
  return send_json(req, root);
}

static esp_err_t handle_get_system_identity(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  cJSON* root = cJSON_CreateObject();
  appendSystemIdentity(root, cfg);
  return send_json(req, root);
}

static esp_err_t handle_get_system_summary(httpd_req_t* req) {
  cJSON* root = cJSON_CreateObject();
  appendSystemMetrics(root, /*include_audio=*/false);
  return send_json(req, root);
}

static esp_err_t handle_get_system_tasks(httpd_req_t* req) {
  cJSON* root = cJSON_CreateObject();
#if (configUSE_TRACE_FACILITY == 1)
  UBaseType_t n = uxTaskGetNumberOfTasks();
  TaskStatus_t* arr = (TaskStatus_t*)calloc(n, sizeof(TaskStatus_t));
  if (!arr) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
  uint32_t total = 0;
  UBaseType_t got = uxTaskGetSystemState(arr, n, &total);
  cJSON* tasks = cJSON_CreateArray();
  for (UBaseType_t i = 0; i < got; ++i) {
    cJSON* t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "name", arr[i].pcTaskName ? arr[i].pcTaskName : "?");
    cJSON_AddNumberToObject(t, "prio", arr[i].uxCurrentPriority);
    cJSON_AddNumberToObject(t, "stack_hwm", arr[i].usStackHighWaterMark);
#if (configGENERATE_RUN_TIME_STATS == 1)
    cJSON_AddNumberToObject(t, "runtime", (double)arr[i].ulRunTimeCounter);
    if (total > 0) {
      cJSON_AddNumberToObject(t, "runtime_pct",
                              (double)arr[i].ulRunTimeCounter * 100.0 / (double)total);
    }
#endif
#ifdef configTASKLIST_COREID
    cJSON_AddNumberToObject(t, "core", (double)arr[i].xCoreID);
#endif
    cJSON_AddItemToArray(tasks, t);
  }
  free(arr);
  cJSON_AddItemToObject(root, "tasks", tasks);
  cJSON_AddNumberToObject(root, "count", (double)got);
#else
  cJSON_AddStringToObject(root, "error", "CONFIG_FREERTOS_USE_TRACE_FACILITY is off");
#endif
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
      if (&on_volume_changed) on_volume_changed();
    }
  }

  cJSON* r = cJSON_CreateObject();
  fill_volume_object(r, cfg, true);
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
  cJSON_Delete(body);

  if (&on_channels_changed) on_channels_changed();

  cJSON* rj = cJSON_CreateObject();
  cJSON_AddStringToObject(rj, "stereo_mono", mode == StereoMode::Mono ? "mono" : "stereo");
  cJSON_AddBoolToObject(rj, "l_on", l_on);
  cJSON_AddBoolToObject(rj, "r_on", r_on);
  return send_json(req, rj);
}

// ---- /api/led -------------------------------------------------------------

static esp_err_t handle_post_led(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");
  if (!geelen_hal::ledPresent()) {
    return send_err_json(req, HTTPD_404_NOT_FOUND, "no ws2812");
  }

  cJSON* body = parse_body(req);
  if (!body) return ESP_FAIL;

  uint8_t r = cfg->getLedR();
  uint8_t g = cfg->getLedG();
  uint8_t b = cfg->getLedB();
  const char* col = jstr(body, "color", nullptr);
  if (col && !parse_hex_color(col, &r, &g, &b)) {
    cJSON_Delete(body);
    return send_err_json(req, HTTPD_400_BAD_REQUEST, "color must be #rrggbb");
  }
  int bri = jint(body, "brightness", cfg->getLedBrightness());
  if (bri < 0) bri = 0;
  if (bri > 100) bri = 100;
  cJSON_Delete(body);

  cfg->setLed(r, g, b, (uint8_t)bri);
  if (&on_led_changed) on_led_changed();

  cJSON* rj = cJSON_CreateObject();
  add_led_json(rj, cfg);
  // add_led_json nests under "led"; flatten for the POST echo.
  cJSON* led = cJSON_DetachItemFromObject(rj, "led");
  cJSON_Delete(rj);
  return send_json(req, led);
}

// ---- /api/mode ------------------------------------------------------------

static esp_err_t handle_post_mode(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  cJSON* body = parse_body(req);
  if (!body) return ESP_FAIL;

  const char* mode = jstr(body, "mode", nullptr);
  AudioSource next = AudioSource::Bluetooth;
  if (!mode || !parseModeName(mode, &next)) {
    cJSON_Delete(body);
    return send_err_json(req, HTTPD_400_BAD_REQUEST,
                         "mode must be one of bluetooth|snapclient|dlna|test_tone");
  }
  AudioSource prev = cfg->getAudioSource();
  cfg->setAudioSource(next);
  cfg->save();

  unsigned pu = (unsigned)prev;
  unsigned nu = (unsigned)next;

  // Phase 3: dispatch to the Mode Manager FSM. It posts
  // EV_MODE_CHANGE_REQUESTED and decides whether the transition can run
  // hot or needs a reboot. Today the answer is always "reboot" for any
  // change, but the seam is in place — concrete `IProducer` implementations
  // for Snap / TestTone can flip the "non-BT pair" branch to a hot-swap
  // without touching this handler again.
  const ModeTransitionResult result =
      modeManager().requestChange((uint8_t)pu, (uint8_t)nu);
  bool reboot = false;
  switch (result) {
    case ModeTransitionResult::Noop:
      break;
    case ModeTransitionResult::HotSwap:
      // No reboot — producer swap already happened inside requestChange.
      break;
    case ModeTransitionResult::RebootRequired:
      reboot = true;
      scheduleRestart(500);
      break;
    case ModeTransitionResult::Failed:
      // Mode Manager rolled back to `prev`; surface the failure to the
      // SPA so the toggle reverts visually. Persisted source is still
      // `next` here because we wrote it above; revert.
      cfg->setAudioSource(prev);
      cfg->save();
      cJSON_Delete(body);
      return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                           "mode change failed; reverted");
  }

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
  cJSON_Delete(body);
  cfg->save();
  if (name && *name) identityApplyLive();

  cJSON* rj = cJSON_CreateObject();
  cJSON_AddBoolToObject(rj, "enabled", enabled);
  char dn[DEV_NAME_MAX]; cfg->getDeviceName(dn, sizeof(dn));
  cJSON_AddStringToObject(rj, "name", dn);
  return send_json(req, rj);
}

// ---- /api/wifi/power-save (Phase 5) ---------------------------------------
//
// POST {"mode": "none" | "min_modem" | "max_modem"}. Persisted in NVS;
// the new mode applies on next boot. We deliberately do NOT hot-apply
// because flipping `esp_wifi_set_ps` mid-stream interacts poorly with
// active BT coex (sometimes drops PCM frames).

static bool parsePsModeStr(const char* s, WifiPsMode* out) {
  if (!s || !out) return false;
  if (!strcmp(s, "none") || !strcmp(s, "off")) {
    *out = WifiPsMode::None; return true;
  }
  if (!strcmp(s, "min_modem") || !strcmp(s, "min")) {
    *out = WifiPsMode::MinModem; return true;
  }
  if (!strcmp(s, "max_modem") || !strcmp(s, "max")) {
    *out = WifiPsMode::MaxModem; return true;
  }
  return false;
}

static esp_err_t handle_post_wifi_power_save(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  cJSON* body = parse_body(req);
  if (!body) return ESP_FAIL;

  const char* mode_s = jstr(body, "mode", nullptr);
  WifiPsMode mode = cfg->getWifiPsMode();
  bool ok = parsePsModeStr(mode_s, &mode);
  cJSON_Delete(body);
  if (!ok) {
    return send_err_json(req, HTTPD_400_BAD_REQUEST,
                         "expected mode in {none, min_modem, max_modem}");
  }
  cfg->setWifiPsMode(mode);

  cJSON* rj = cJSON_CreateObject();
  const char* out = "none";
  switch (mode) {
    case WifiPsMode::None:     out = "none";       break;
    case WifiPsMode::MinModem: out = "min_modem";  break;
    case WifiPsMode::MaxModem: out = "max_modem";  break;
  }
  cJSON_AddStringToObject(rj, "mode", out);
  cJSON_AddBoolToObject(rj, "applies_on_next_boot", true);
  return send_json(req, rj);
}

// ---- /api/voice-feedback -------------------------------------------------

static esp_err_t handle_post_voice_feedback(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  cJSON* body = parse_body(req);
  if (!body) return ESP_FAIL;

  bool enabled = jbool(body, "enabled", cfg->getVoiceFeedbackEnabled());
  cfg->setVoiceFeedbackEnabled(enabled);
  cJSON_Delete(body);

  cJSON* rj = cJSON_CreateObject();
  cJSON_AddBoolToObject(rj, "enabled", cfg->getVoiceFeedbackEnabled());
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
  cJSON_Delete(body);
  cfg->save();
  identityApplyLive();

  cJSON* rj = cJSON_CreateObject();
  char dn[DEV_NAME_MAX]; cfg->getDeviceName(dn, sizeof(dn));
  cJSON_AddStringToObject(rj, "name", dn);
  return send_json(req, rj);
}

static void fill_effective_mac_(ConfigManager* cfg, bool sta, uint8_t out[6]) {
  if (sta) {
    if (cfg->getStaMacOverride(out)) return;
    identityFactoryMac((int)ESP_MAC_WIFI_STA, out);
  } else {
    if (cfg->getBtMacOverride(out)) return;
    identityFactoryMac((int)ESP_MAC_BT, out);
  }
}

// Parse a MAC JSON field: missing = no change; null = factory; string = set.
// Returns false on malformed string (caller 400).
static bool parse_mac_field_(const cJSON* body, const char* key, bool* present,
                             bool* clear, uint8_t mac[6], const char** err) {
  *present = false;
  *clear = false;
  const cJSON* v = cJSON_GetObjectItemCaseSensitive(body, key);
  if (!v) return true;
  *present = true;
  if (cJSON_IsNull(v)) {
    *clear = true;
    return true;
  }
  if (!cJSON_IsString(v) || !v->valuestring) {
    *err = "MAC must be a string or null";
    return false;
  }
  if (!identityParseMac(v->valuestring, mac)) {
    *err = "MAC must be aa:bb:cc:dd:ee:ff";
    return false;
  }
  if (!identitySanitizeCustomMac(mac)) {
    *err = "MAC must be unicast (not multicast/zero/broadcast)";
    return false;
  }
  return true;
}

static esp_err_t handle_get_identity(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");
  cJSON* root = cJSON_CreateObject();
  identityAddJson(root, cfg, /*include_macs=*/true);
  return send_json(req, root);
}

static esp_err_t handle_post_identity(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  cJSON* body = parse_body(req);
  if (!body) return ESP_FAIL;

  const cJSON* name_j = cJSON_GetObjectItemCaseSensitive(body, "name");
  if (name_j) {
    if (!cJSON_IsString(name_j) || !name_j->valuestring || !name_j->valuestring[0]) {
      cJSON_Delete(body);
      return send_err_json(req, HTTPD_400_BAD_REQUEST, "name required");
    }
  }
  const cJSON* host_j = cJSON_GetObjectItemCaseSensitive(body, "hostname");
  char host_norm[HOSTNAME_MAX];
  bool host_clear = false;
  if (host_j) {
    if (cJSON_IsNull(host_j) || (cJSON_IsString(host_j) && host_j->valuestring &&
                                 !host_j->valuestring[0])) {
      host_clear = true;
    } else if (cJSON_IsString(host_j) && host_j->valuestring) {
      if (!identityNormalizeHostname(host_j->valuestring, host_norm, sizeof(host_norm))) {
        cJSON_Delete(body);
        return send_err_json(req, HTTPD_400_BAD_REQUEST,
                             "hostname must be 1-31 chars [a-z0-9-], no leading/trailing hyphen");
      }
    } else {
      cJSON_Delete(body);
      return send_err_json(req, HTTPD_400_BAD_REQUEST, "hostname must be a string or null");
    }
  }

  bool sta_present = false, sta_clear = false, bt_present = false, bt_clear = false;
  uint8_t sta_mac[6] = {};
  uint8_t bt_mac[6] = {};
  const char* mac_err = nullptr;
  if (!parse_mac_field_(body, "sta_mac", &sta_present, &sta_clear, sta_mac, &mac_err) ||
      !parse_mac_field_(body, "bt_mac", &bt_present, &bt_clear, bt_mac, &mac_err)) {
    cJSON_Delete(body);
    return send_err_json(req, HTTPD_400_BAD_REQUEST, mac_err ? mac_err : "bad MAC");
  }

  uint8_t eff_sta[6] = {};
  uint8_t eff_bt[6] = {};
  fill_effective_mac_(cfg, true, eff_sta);
  fill_effective_mac_(cfg, false, eff_bt);
  if (sta_present) {
    if (sta_clear) identityFactoryMac((int)ESP_MAC_WIFI_STA, eff_sta);
    else memcpy(eff_sta, sta_mac, 6);
  }
  if (bt_present) {
    if (bt_clear) identityFactoryMac((int)ESP_MAC_BT, eff_bt);
    else memcpy(eff_bt, bt_mac, 6);
  }
  if ((sta_present || bt_present) && identityMacEqual(eff_sta, eff_bt)) {
    cJSON_Delete(body);
    return send_err_json(req, HTTPD_400_BAD_REQUEST, "STA and BT MACs must differ");
  }

  if (name_j) cfg->setDeviceName(name_j->valuestring);
  if (host_j) cfg->setHostname(host_clear ? "" : host_norm);
  if (sta_present) {
    if (sta_clear) cfg->clearStaMacOverride();
    else cfg->setStaMacOverride(sta_mac);
  }
  if (bt_present) {
    if (bt_clear) cfg->clearBtMacOverride();
    else cfg->setBtMacOverride(bt_mac);
  }
  cJSON_Delete(body);

  const bool reboot = sta_present || bt_present;
  cfg->save();
  if (!reboot) identityApplyLive();

  cJSON* rj = cJSON_CreateObject();
  identityAddJson(rj, cfg, /*include_macs=*/true);
  cJSON_AddBoolToObject(rj, "reboot", reboot);
  esp_err_t ok = send_json(req, rj);
  if (reboot) scheduleRestart(800);
  return ok;
}

// ---- /api/bluetooth -------------------------------------------------------

static cJSON* bluetooth_json(ConfigManager* cfg, bool include_pin) {
  cJSON* bt = cJSON_CreateObject();
  if (include_pin) {
    char pin[BT_PIN_MAX];
    cfg->getBtPin(pin, sizeof(pin));
    cJSON_AddStringToObject(bt, "pin", pin);
  }
  cJSON_AddBoolToObject(bt, "connected", a2dpIsConnected());
  cJSON_AddBoolToObject(bt, "discoverable", a2dpIsDiscoverable());
  cJSON_AddNumberToObject(bt, "window_ms_left", (double)a2dpPairingWindowMsLeft());
  return bt;
}

static esp_err_t handle_get_bluetooth(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");
  return send_json(req, bluetooth_json(cfg, /*include_pin=*/true));
}

static esp_err_t handle_post_bluetooth(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");

  cJSON* body = parse_body(req);
  if (!body) return ESP_FAIL;

  const cJSON* pin_j = cJSON_GetObjectItemCaseSensitive(body, "pin");
  if (pin_j) {
    if (!cJSON_IsString(pin_j) || !pin_j->valuestring || !cfg->setBtPin(pin_j->valuestring)) {
      cJSON_Delete(body);
      return send_err_json(req, HTTPD_400_BAD_REQUEST, "pin must be exactly 4 digits");
    }
    a2dpApplyPin(pin_j->valuestring);
    cfg->save();
  }

  const cJSON* disc = cJSON_GetObjectItemCaseSensitive(body, "discoverable");
  if (disc) {
    if (!cJSON_IsBool(disc)) {
      cJSON_Delete(body);
      return send_err_json(req, HTTPD_400_BAD_REQUEST, "discoverable must be a boolean");
    }
    if (cJSON_IsTrue(disc)) {
      if (!a2dpIsUp()) {
        cJSON_Delete(body);
        return send_err_json(req, HTTPD_400_BAD_REQUEST, "bluetooth sink not started");
      }
      a2dpStartPairingWindow();
    }
  }

  cJSON_Delete(body);
  return send_json(req, bluetooth_json(cfg, /*include_pin=*/true));
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

  ParametricBand bands[PARAMETRIC_BANDS];
  ParametricBand bands_r[PARAMETRIC_BANDS];
  cfg->getEqBands(bands);
  cfg->getEqBandsR(bands_r);

  const cJSON* modej = cJSON_GetObjectItemCaseSensitive(body, "channel_mode");
  if (cJSON_IsString(modej) && modej->valuestring) {
    if (!strcmp(modej->valuestring, "split")) {
      cfg->setEqChannelMode(EqChannelMode::Split);
    } else if (!strcmp(modej->valuestring, "ganged")) {
      cfg->setEqChannelMode(EqChannelMode::Ganged);
    }
  }

  // `band_count` is clamped to the board's runtime cap
  // (`HAL_EQ_MAX_BANDS_ACTIVE`). Storage still holds up to 15 bands so a
  // preset created on Louder round-trips through Amped without losing
  // metadata — Amped just refuses to *activate* more than 10 bands.
  const cJSON* bcj = cJSON_GetObjectItemCaseSensitive(body, "band_count");
  if (cJSON_IsNumber(bcj)) {
    int v = (int)cJSON_GetNumberValue(bcj);
    if (v < 1) v = 1;
    if (v > HAL_EQ_MAX_BANDS_ACTIVE) v = HAL_EQ_MAX_BANDS_ACTIVE;
    cfg->setEqActiveBands((uint8_t)v);
  }

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
  const cJSON* arr_r = cJSON_GetObjectItemCaseSensitive(body, "bands_r");
  if (cJSON_IsArray(arr_r)) {
    int i = 0;
    const cJSON* it = nullptr;
    cJSON_ArrayForEach(it, arr_r) {
      if (i >= (int)PARAMETRIC_BANDS) break;
      (void)jsonToBand(it, &bands_r[i]);
      ++i;
    }
  }

  const bool enabled = jbool(body, "enabled", cfg->getEqEnabled());
  cfg->setEqBands(bands);
  if (cfg->getEqChannelMode() == EqChannelMode::Split) {
    cfg->setEqBandsR(bands_r);
  }
  cfg->setEqEnabled(enabled);
  cJSON_Delete(body);

  if (&on_eq_changed) on_eq_changed();

  cJSON* rj = cJSON_CreateObject();
  fill_eq_object(rj, cfg);
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

// Preset file format history:
//   V1 (pre-Phase-1): 32-byte name + 10 bands × 12 bytes (freq/gain/Q).
//   V2 (Phase-1..2)  : magic + version + band_count + reserved + 32-byte name +
//                      10 bands × 16 bytes.
//   V3               : same header + 15 bands × 16 bytes. `band_count` is
//                      the user-active count (1..15). Read as ganged (R=L).
//   V4 (current)     : V3 + channel_mode in the reserved byte + second 15-band
//                      array (`bands_r`). Magic 'EGP4'. Amped load uses L only.
static constexpr uint32_t kPresetMagic = 0x34504745u;   // 'EGP4' LE
static constexpr uint32_t kPresetMagicV3 = 0x33504745u; // 'EGP3' LE — read-only
static constexpr uint32_t kPresetMagicV2 = 0x32504745u; // 'EGP2' LE — read-only
static constexpr uint16_t kPresetVersion = 4;
static constexpr uint16_t kPresetVersionV3 = 3;
static constexpr size_t kPresetV2BandCount = 10;

#pragma pack(push, 1)
struct PresetFileV3 {
  uint32_t magic;
  uint16_t version;
  uint8_t band_count;
  uint8_t reserved;
  char name[PRESET_NAME_MAX];
  ParametricBand bands[PARAMETRIC_BANDS];
};
struct PresetFileV4 {
  uint32_t magic;
  uint16_t version;
  uint8_t band_count;
  uint8_t channel_mode;  // 0 ganged, 1 split
  char name[PRESET_NAME_MAX];
  ParametricBand bands[PARAMETRIC_BANDS];
  ParametricBand bands_r[PARAMETRIC_BANDS];
};
#pragma pack(pop)

static_assert(sizeof(PresetFileV4) > 512, "V4 preset must not fit in the old 512-byte read buffer");

static void presetCopyLtoR(PresetFileV4* out) {
  memcpy(out->bands_r, out->bands, sizeof(out->bands_r));
  out->channel_mode = 0;
}

static void presetMigrate12(const uint8_t* src, size_t src_bands, ParametricBand out[PARAMETRIC_BANDS]) {
  // Match ConfigManager::loadDefaultEqBands defaults for the tail.
  for (size_t i = 0; i < PARAMETRIC_BANDS; ++i) {
    out[i].freq_hz = 1000.f;
    out[i].gain_db = 0.f;
    out[i].q = 1.2f;
    out[i].type = geelen::EqFilterType::Bell;
    out[i].slope_db_oct = 12;
    out[i]._pad[0] = out[i]._pad[1] = 0;
  }
  for (size_t i = 0; i < src_bands && i < PARAMETRIC_BANDS; ++i) {
    float f, g, q;
    memcpy(&f, src + i * 12, sizeof(float));
    memcpy(&g, src + i * 12 + 4, sizeof(float));
    memcpy(&q, src + i * 12 + 8, sizeof(float));
    out[i].freq_hz = f;
    out[i].gain_db = g;
    out[i].q = q;
  }
}

static void presetPath(char out[64], size_t idx) {
  snprintf(out, 64, "%s/pref_%u.bin", PRESETS_DIR, (unsigned)idx);
}

static bool presetReadV4(size_t idx, PresetFileV4* out) {
  char path[64];
  presetPath(path, idx);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  uint8_t buf[640];
  ssize_t r = read(fd, buf, sizeof(buf));
  close(fd);

  constexpr ssize_t kV4 = (ssize_t)sizeof(PresetFileV4);
  constexpr ssize_t kV3 = (ssize_t)sizeof(PresetFileV3);
  constexpr ssize_t kV2 = (ssize_t)(8u + PRESET_NAME_MAX + kPresetV2BandCount * sizeof(ParametricBand));
  constexpr ssize_t kV1 = (ssize_t)(PRESET_NAME_MAX + kPresetV2BandCount * 12u + 8u);

  auto clamp_count = [](uint8_t bc) {
    if (bc < 1 || bc > (uint8_t)PARAMETRIC_BANDS) return (uint8_t)PARAMETRIC_BANDS;
    return bc;
  };

  if (r >= kV4) {
    memcpy(out, buf, sizeof(*out));
    if (out->magic == kPresetMagic && out->version == kPresetVersion) {
      out->band_count = clamp_count(out->band_count);
      if (out->channel_mode > 1) out->channel_mode = 0;
      out->name[PRESET_NAME_MAX - 1] = '\0';
      return true;
    }
  }
  if (r >= kV3) {
    PresetFileV3 v3{};
    memcpy(&v3, buf, sizeof(v3));
    if (v3.magic == kPresetMagicV3 && v3.version == kPresetVersionV3) {
      out->magic = kPresetMagic;
      out->version = kPresetVersion;
      out->band_count = clamp_count(v3.band_count);
      memcpy(out->name, v3.name, PRESET_NAME_MAX);
      out->name[PRESET_NAME_MAX - 1] = '\0';
      memcpy(out->bands, v3.bands, sizeof(out->bands));
      presetCopyLtoR(out);
      return true;
    }
  }
  if (r >= kV2) {
    uint32_t magic;
    memcpy(&magic, buf, sizeof(magic));
    if (magic == kPresetMagicV2) {
      out->magic = kPresetMagic;
      out->version = kPresetVersion;
      uint8_t band_count = buf[6];
      out->band_count = (band_count >= 1 && band_count <= kPresetV2BandCount) ? band_count
                                                                              : (uint8_t)kPresetV2BandCount;
      memcpy(out->name, buf + 8, PRESET_NAME_MAX);
      out->name[PRESET_NAME_MAX - 1] = '\0';
      for (size_t i = 0; i < PARAMETRIC_BANDS; ++i) {
        out->bands[i].freq_hz = 1000.f;
        out->bands[i].gain_db = 0.f;
        out->bands[i].q = 1.2f;
        out->bands[i].type = geelen::EqFilterType::Bell;
        out->bands[i].slope_db_oct = 12;
        out->bands[i]._pad[0] = out->bands[i]._pad[1] = 0;
      }
      memcpy(out->bands, buf + 8 + PRESET_NAME_MAX,
             kPresetV2BandCount * sizeof(ParametricBand));
      presetCopyLtoR(out);
      return true;
    }
  }
  if (r >= kV1) {
    memcpy(out->name, buf, PRESET_NAME_MAX);
    out->name[PRESET_NAME_MAX - 1] = '\0';
    presetMigrate12(buf + PRESET_NAME_MAX, kPresetV2BandCount, out->bands);
    out->magic = kPresetMagic;
    out->version = kPresetVersion;
    out->band_count = (uint8_t)kPresetV2BandCount;
    presetCopyLtoR(out);
    return true;
  }
  return false;
}

static bool presetWriteV4(size_t idx, const PresetFileV4* in) {
  (void)mkdir(PRESETS_DIR, 0777);
  char path[64];
  presetPath(path, idx);
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
  uint8_t active = cfg->getEqActiveSlot();
  if (active == 0xFF) cJSON_AddNullToObject(root, "active");
  else                cJSON_AddNumberToObject(root, "active", active);

  cJSON* slots = cJSON_CreateArray();
  for (size_t i = 0; i < PRESET_SLOT_COUNT; ++i) {
    cJSON* s = cJSON_CreateObject();
    cJSON_AddNumberToObject(s, "index", i);
    PresetFileV4 pf{};
    const bool saved = presetReadV4(i, &pf);
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
    if ((uint8_t)idx == cfg->getEqActiveSlot()) cfg->setEqActiveSlot(0xFF);
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
    PresetFileV4 pf{};
    pf.magic = kPresetMagic;
    pf.version = kPresetVersion;
    pf.band_count = cfg->getEqActiveBands();
    pf.channel_mode = cfg->getEqChannelMode() == EqChannelMode::Split ? 1 : 0;
    if (name && *name) strncpy(pf.name, name, PRESET_NAME_MAX - 1);
    else snprintf(pf.name, PRESET_NAME_MAX, "Preset %u", (unsigned)(idx + 1));
    pf.name[PRESET_NAME_MAX - 1] = '\0';
    cfg->getEqBands(pf.bands);
    cfg->getEqBandsR(pf.bands_r);
    const bool ok = presetWriteV4(idx, &pf);
    cJSON_Delete(body);
    if (!ok) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    cfg->setEqActiveSlot((uint8_t)idx);
    cJSON* rj = cJSON_CreateObject();
    cJSON_AddNumberToObject(rj, "index", (double)idx);
    cJSON_AddStringToObject(rj, "name", pf.name);
    cJSON_AddBoolToObject(rj, "saved", true);
    return send_json(req, rj);
  }

  if (!strcmp(action, "load")) {
    PresetFileV4 pf{};
    if (!presetReadV4(idx, &pf)) return send_err_json(req, HTTPD_404_NOT_FOUND, "slot empty");
    const bool split = (pf.channel_mode == 1);
    cfg->setEqChannelMode(split ? EqChannelMode::Split : EqChannelMode::Ganged);
    cfg->setEqBands(pf.bands);
    if (cfg->getEqChannelMode() == EqChannelMode::Split) {
      cfg->setEqBandsR(pf.bands_r);
    }
    uint8_t bc = pf.band_count;
    if (bc > HAL_EQ_MAX_BANDS_ACTIVE) bc = HAL_EQ_MAX_BANDS_ACTIVE;
    cfg->setEqActiveBands(bc);
    cfg->setEqActiveSlot((uint8_t)idx);
    if (&on_eq_changed) on_eq_changed();
    cJSON* rj = cJSON_CreateObject();
    cJSON_AddNumberToObject(rj, "index", (double)idx);
    pf.name[PRESET_NAME_MAX - 1] = '\0';
    cJSON_AddStringToObject(rj, "name", pf.name);
    cJSON* eqj = cJSON_CreateObject();
    fill_eq_object(eqj, cfg);
    cJSON_AddItemToObject(rj, "eq", eqj);
    return send_json(req, rj);
  }

  if (!strcmp(action, "rename")) {
    cJSON* body = parse_body(req);
    if (!body) return ESP_FAIL;
    const char* name = jstr(body, "name", nullptr);
    if (!name || !*name) { cJSON_Delete(body); return send_err_json(req, HTTPD_400_BAD_REQUEST, "name required"); }
    PresetFileV4 pf{};
    if (!presetReadV4(idx, &pf)) { cJSON_Delete(body); return send_err_json(req, HTTPD_404_NOT_FOUND, "slot empty"); }
    strncpy(pf.name, name, PRESET_NAME_MAX - 1);
    pf.name[PRESET_NAME_MAX - 1] = '\0';
    const bool ok = presetWriteV4(idx, &pf);
    cJSON_Delete(body);
    if (!ok) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    cJSON* rj = cJSON_CreateObject();
    cJSON_AddNumberToObject(rj, "index", (double)idx);
    cJSON_AddStringToObject(rj, "name", pf.name);
    return send_json(req, rj);
  }

  return send_err_json(req, HTTPD_404_NOT_FOUND, "unknown preset action");
}

// ---- /api/test-tone -------------------------------------------------------

static esp_err_t handle_get_test_tone(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");
  cJSON* r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "freq_hz", cfg->getTestToneFreqHz());
  cJSON_AddNumberToObject(r, "amp_db", cfg->getTestToneAmpDb());
  cJSON_AddNumberToObject(r, "pan", cfg->getTestTonePan());
  cJSON_AddNumberToObject(r, "mode", cfg->getTestToneMode());
  cJSON_AddNumberToObject(r, "sweep_s", cfg->getTestToneSweepS());
  cJSON_AddNumberToObject(r, "sweep_lo_hz", cfg->getTestToneSweepLoHz());
  cJSON_AddNumberToObject(r, "sweep_hi_hz", cfg->getTestToneSweepHiHz());
  cJSON_AddNumberToObject(r, "dual_f2_hz", cfg->getTestToneDualF2Hz());
  return send_json(req, r);
}

static esp_err_t handle_post_test_tone(httpd_req_t* req) {
  auto* cfg = (ConfigManager*)req->user_ctx;
  if (!cfg) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no cfg");
  cJSON* body = parse_body(req);
  if (!body) return ESP_FAIL;

  const float freq = (float)jdouble(body, "freq_hz", cfg->getTestToneFreqHz());
  const float amp = (float)jdouble(body, "amp_db", cfg->getTestToneAmpDb());
  const float pan = (float)jdouble(body, "pan", cfg->getTestTonePan());
  int mode_i = jint(body, "mode", (int)cfg->getTestToneMode());
  if (mode_i < 0) mode_i = 0;
  if (mode_i > 10) mode_i = 10;
  const float sweep_s = (float)jdouble(body, "sweep_s", cfg->getTestToneSweepS());
  const float slo = (float)jdouble(body, "sweep_lo_hz", cfg->getTestToneSweepLoHz());
  const float shi = (float)jdouble(body, "sweep_hi_hz", cfg->getTestToneSweepHiHz());
  const float df2 = (float)jdouble(body, "dual_f2_hz", cfg->getTestToneDualF2Hz());
  cfg->setTestToneParams(freq, amp, pan, (uint8_t)mode_i, sweep_s, slo, shi, df2);
  cJSON_Delete(body);

  if (&testToneNotifyParamsUpdated) testToneNotifyParamsUpdated();

  cJSON* r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", true);
  cJSON_AddNumberToObject(r, "freq_hz", cfg->getTestToneFreqHz());
  cJSON_AddNumberToObject(r, "amp_db", cfg->getTestToneAmpDb());
  cJSON_AddNumberToObject(r, "pan", cfg->getTestTonePan());
  cJSON_AddNumberToObject(r, "mode", cfg->getTestToneMode());
  cJSON_AddNumberToObject(r, "sweep_s", cfg->getTestToneSweepS());
  cJSON_AddNumberToObject(r, "sweep_lo_hz", cfg->getTestToneSweepLoHz());
  cJSON_AddNumberToObject(r, "sweep_hi_hz", cfg->getTestToneSweepHiHz());
  cJSON_AddNumberToObject(r, "dual_f2_hz", cfg->getTestToneDualF2Hz());
  return send_json(req, r);
}

// ---- /api/wifi/scan + /api/wifi/state ------------------------------------

// `esp_wifi_scan_start` requires STA or APSTA mode. The captive AP fallback in
// `idf_wifi_manager.cpp` already runs APSTA; in normal STA mode we are also
// fine. We block (synchronous scan) for ~3 s to keep the handler simple.
static esp_err_t handle_get_wifi_scan(httpd_req_t* req) {
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&mode);
  if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
    return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan needs STA/APSTA mode");
  }

  wifi_scan_config_t scan{};
  scan.show_hidden = false;
  // Block until results are in (~3 s, depending on channel dwell). Captive
  // portal browsers tolerate this; the scan endpoint is not on the audio path.
  esp_err_t e = esp_wifi_scan_start(&scan, true);
  if (e != ESP_OK) {
    return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(e));
  }

  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  if (n > 32) n = 32;  // cap to keep the JSON small
  wifi_ap_record_t* recs = nullptr;
  if (n > 0) {
    recs = (wifi_ap_record_t*)calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) {
      esp_wifi_clear_ap_list();
      return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
      free(recs);
      return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan get failed");
    }
  }

  // Dedupe by SSID, keep strongest RSSI per name. Vendors broadcast the same
  // SSID on 2.4 / 5 GHz; we pick the best signal.
  struct Entry { char ssid[33]; int8_t rssi; uint8_t channel; uint8_t auth; };
  Entry uniq[32];
  size_t uniq_n = 0;
  for (uint16_t i = 0; i < n; ++i) {
    const wifi_ap_record_t& r = recs[i];
    if (r.ssid[0] == '\0') continue;
    bool merged = false;
    for (size_t j = 0; j < uniq_n; ++j) {
      if (strncmp(uniq[j].ssid, (const char*)r.ssid, 32) == 0) {
        if (r.rssi > uniq[j].rssi) {
          uniq[j].rssi = r.rssi;
          uniq[j].channel = r.primary;
          uniq[j].auth = (uint8_t)r.authmode;
        }
        merged = true;
        break;
      }
    }
    if (!merged && uniq_n < 32) {
      strncpy(uniq[uniq_n].ssid, (const char*)r.ssid, sizeof(uniq[uniq_n].ssid) - 1);
      uniq[uniq_n].ssid[sizeof(uniq[uniq_n].ssid) - 1] = '\0';
      uniq[uniq_n].rssi = r.rssi;
      uniq[uniq_n].channel = r.primary;
      uniq[uniq_n].auth = (uint8_t)r.authmode;
      uniq_n++;
    }
  }
  free(recs);

  // Sort by RSSI desc (small N, simple insertion sort).
  for (size_t i = 1; i < uniq_n; ++i) {
    Entry tmp = uniq[i];
    size_t j = i;
    while (j > 0 && uniq[j - 1].rssi < tmp.rssi) {
      uniq[j] = uniq[j - 1];
      --j;
    }
    uniq[j] = tmp;
  }
  if (uniq_n > 24) uniq_n = 24;

  cJSON* root = cJSON_CreateObject();
  cJSON* arr = cJSON_CreateArray();
  for (size_t i = 0; i < uniq_n; ++i) {
    cJSON* it = cJSON_CreateObject();
    cJSON_AddStringToObject(it, "ssid", uniq[i].ssid);
    cJSON_AddNumberToObject(it, "rssi", uniq[i].rssi);
    cJSON_AddNumberToObject(it, "channel", uniq[i].channel);
    cJSON_AddNumberToObject(it, "auth", uniq[i].auth);
    cJSON_AddItemToArray(arr, it);
  }
  cJSON_AddItemToObject(root, "networks", arr);
  return send_json(req, root);
}

static esp_err_t handle_get_wifi_state(httpd_req_t* req) {
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&mode);

  bool ap_running = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
  bool sta_connected = false;
  char ssid[33] = {0};
  int rssi = 0;
  uint8_t channel = 0;
  wifi_ap_record_t ap{};
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    sta_connected = true;
    strncpy(ssid, (const char*)ap.ssid, sizeof(ssid) - 1);
    rssi = ap.rssi;
    channel = ap.primary;
  }

  char ip_buf[16] = "0.0.0.0";
  esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (nif) {
    esp_netif_ip_info_t ip{};
    if (esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr != 0) {
      snprintf(ip_buf, sizeof(ip_buf), "%u.%u.%u.%u",
               (unsigned)((ip.ip.addr >> 0) & 0xff),
               (unsigned)((ip.ip.addr >> 8) & 0xff),
               (unsigned)((ip.ip.addr >> 16) & 0xff),
               (unsigned)((ip.ip.addr >> 24) & 0xff));
    }
  }

  char ap_ssid[33] = {0};
  esp_netif_t* ap_nif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  char ap_ip_buf[16] = "0.0.0.0";
  if (ap_running && ap_nif) {
    esp_netif_ip_info_t ip{};
    if (esp_netif_get_ip_info(ap_nif, &ip) == ESP_OK) {
      snprintf(ap_ip_buf, sizeof(ap_ip_buf), "%u.%u.%u.%u",
               (unsigned)((ip.ip.addr >> 0) & 0xff),
               (unsigned)((ip.ip.addr >> 8) & 0xff),
               (unsigned)((ip.ip.addr >> 16) & 0xff),
               (unsigned)((ip.ip.addr >> 24) & 0xff));
    }
    wifi_config_t wc{};
    if (esp_wifi_get_config(WIFI_IF_AP, &wc) == ESP_OK) {
      strncpy(ap_ssid, (const char*)wc.ap.ssid, sizeof(ap_ssid) - 1);
    }
  }

  cJSON* root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ap_only", ap_running && !sta_connected);
  cJSON_AddBoolToObject(root, "ap_running", ap_running);
  cJSON_AddBoolToObject(root, "sta_connected", sta_connected);
  cJSON_AddStringToObject(root, "ssid", ssid);
  cJSON_AddNumberToObject(root, "rssi", rssi);
  cJSON_AddNumberToObject(root, "channel", channel);
  cJSON_AddStringToObject(root, "ip", ip_buf);
  cJSON_AddStringToObject(root, "ap_ssid", ap_ssid);
  cJSON_AddStringToObject(root, "ap_ip", ap_ip_buf);
  cJSON_AddStringToObject(root, "state",
                          s_wifi_mgr ? s_wifi_mgr->probeStateName() : "idle");
  return send_json(req, root);
}

static esp_err_t handle_post_wifi_validate(httpd_req_t* req) {
  if (!s_wifi_mgr) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no wifi mgr");
  cJSON* body = parse_body(req);
  if (!body) return ESP_FAIL;
  const char* ssid = jstr(body, "ssid", "");
  const char* pass = jstr(body, "pass", "");
  WifiProbeResult r{};
  bool ok = s_wifi_mgr->probeCredentials(ssid, pass, 12000, &r);
  cJSON_Delete(body);
  cJSON* out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "ok", ok);
  cJSON_AddStringToObject(out, "reason", r.reason);
  cJSON_AddStringToObject(out, "detail", r.detail);
  if (r.ip[0]) cJSON_AddStringToObject(out, "ip", r.ip);
  return send_json(req, out);
}

static esp_err_t handle_post_wifi_commit(httpd_req_t* req) {
  if (!s_wifi_mgr) return send_err_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no wifi mgr");
  (void)req;
  if (!s_wifi_mgr->commitValidated()) {
    return send_err_json(req, HTTPD_400_BAD_REQUEST, "no successful validate in this session");
  }
  cJSON* out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "ok", true);
  cJSON_AddBoolToObject(out, "reboot", true);
  esp_err_t e = send_json(req, out);
  scheduleRestart(800);
  return e;
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
  else if (!strcmp(scope, "bluetooth"))    { a2dpWipeBonds();
                                             cfg->resetBluetoothPairings(); reboot = true; }
  else if (!strcmp(scope, "snapcast"))     { cfg->resetSnapcast();
                                             if (&on_snapcast_config_changed) on_snapcast_config_changed(); }
  else if (!strcmp(scope, "dlna"))         { cfg->resetDlna(); }
  else if (!strcmp(scope, "eq"))           { cfg->resetEq();
                                             if (&on_eq_changed) on_eq_changed(); }
  else if (!strcmp(scope, "audio"))        { cfg->resetAudio();
                                             if (&on_volume_changed) on_volume_changed();
                                             if (&on_channels_changed) on_channels_changed();
                                             if (&on_led_changed) on_led_changed(); }
  else if (!strcmp(scope, "all") || !strcmp(scope, "factory")) {
    a2dpWipeBonds();
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
    esp_err_t e = httpd_register_uri_handler(h, &u);
    if (e != ESP_OK) {
      ESP_LOGE(TAG, "httpd_register_uri_handler failed uri=%s method=%d: %s", uri, (int)method,
               esp_err_to_name(e));
    }
  };

  reg("/api/state",              HTTP_GET,    handle_get_state);
  reg("/api/system",             HTTP_GET,    handle_get_system);
  reg("/api/system/metrics",     HTTP_GET,    handle_get_system_metrics);
  reg("/api/system/identity",    HTTP_GET,    handle_get_system_identity);
  reg("/api/system/summary",     HTTP_GET,    handle_get_system_summary);
  reg("/api/system/tasks",       HTTP_GET,    handle_get_system_tasks);
  reg("/api/test-tone",          HTTP_GET,    handle_get_test_tone);
  reg("/api/test-tone",          HTTP_POST,   handle_post_test_tone);
  reg("/api/volume",             HTTP_GET,    handle_get_volume);
  reg("/api/volume",             HTTP_POST,   handle_post_volume);
  reg("/api/channels",           HTTP_POST,   handle_post_channels);
  reg("/api/led",                HTTP_POST,   handle_post_led);
  reg("/api/mode",               HTTP_POST,   handle_post_mode);
  reg("/api/snapcast",           HTTP_POST,   handle_post_snapcast);  // Overrides legacy form handler; JSON-only.
  reg("/api/dlna",               HTTP_POST,   handle_post_dlna);
  reg("/api/voice-feedback",     HTTP_POST,   handle_post_voice_feedback);
  reg("/api/device-name",        HTTP_POST,   handle_post_device_name);
  reg("/api/identity",           HTTP_GET,    handle_get_identity);
  reg("/api/identity",           HTTP_POST,   handle_post_identity);
  reg("/api/bluetooth",          HTTP_GET,    handle_get_bluetooth);
  reg("/api/bluetooth",          HTTP_POST,   handle_post_bluetooth);
  reg("/api/eq",                 HTTP_POST,   handle_post_eq);
  reg("/api/eq/headroom",        HTTP_POST,   handle_post_eq_headroom);
  reg("/api/eq/presets",         HTTP_GET,    handle_get_presets);
  // Preset item CRUD: we register a single wildcard entry and dispatch on
  // the parsed index/action inside handle_presets_route. Saves URI slots.
  reg("/api/eq/presets/*",       HTTP_POST,   handle_presets_route);
  reg("/api/eq/presets/*",       HTTP_DELETE, handle_presets_route);
  reg("/api/reset",              HTTP_POST,   handle_post_reset);
  reg("/api/wifi/scan",          HTTP_GET,    handle_get_wifi_scan);
  reg("/api/wifi/state",         HTTP_GET,    handle_get_wifi_state);
  reg("/api/wifi/validate",      HTTP_POST,   handle_post_wifi_validate);
  reg("/api/wifi/commit",        HTTP_POST,   handle_post_wifi_commit);
  reg("/api/wifi/power-save",    HTTP_POST,   handle_post_wifi_power_save);
}

}  // namespace geelen_idf

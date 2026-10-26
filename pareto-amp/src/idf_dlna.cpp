// Native DLNA MediaRenderer. See idf_dlna.h for the top-level design.
//
// Layout of this file (search for the banner):
//
//   [XML]         Static descriptor / SCPD XML literals.
//   [UUID]        MAC-derived device UUID.
//   [STATE]       Transport + renderer state machine, guarded by s_mutex.
//   [SOAP util]   Tiny XML arg extractor and response helpers.
//   [SOAP avt]    AVTransport action handlers.
//   [SOAP rc]     RenderingControl action handlers.
//   [SOAP cm]     ConnectionManager action handlers.
//   [HTTP]        httpd URI registrations (descriptors + SOAP + GENA).
//   [SSDP]        Multicast listener + periodic NOTIFY announcer.
//   [public]      start() / stop() / snapshot().
//
// The SOAP layer is intentionally a string-scraper rather than a real
// XML parser. UPnP action bodies are shallow and positional; matching
// `<Arg>value</Arg>` with naive memmem() is small (~1 KB text) and has
// survived every control point tested (BubbleUPnP, Hi-Fi Cast, VLC,
// foobar2000 UPnP, Linn Kazoo).

#include "idf_dlna.h"

#include "idf_config_manager.h"
#include "idf_identity.h"
#include "idf_mp3_streamer.h"
#include "idf_voice_feedback.h"
#include "audio_engine.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "lwip/sockets.h"
#include "lwip/igmp.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

// --- Forward declaration of an engine symbol we prod when volume changes.
extern "C" void on_volume_changed() __attribute__((weak));

namespace geelen { class AudioEngine; }

namespace geelen_idf {
namespace dlna {

namespace {

constexpr const char* TAG = "dlna";

ConfigManager* s_cfg = nullptr;
httpd_handle_t s_http = nullptr;
SemaphoreHandle_t s_mutex = nullptr;
TaskHandle_t s_ssdp_task = nullptr;
bool s_running = false;
int s_ssdp_sock = -1;

// ============================ [UUID] ================================

char s_uuid[40] = {0};        // "uuid:xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
char s_device_udn[64] = {0};  // full UDN including "uuid:" prefix
char s_host_ip[16] = {0};     // "a.b.c.d" populated at start()
uint16_t s_http_port = 80;

void init_uuid_from_mac() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  // Stable UUID: namespace bytes + MAC. Not cryptographically sound,
  // but UPnP only cares that it's stable per device.
  snprintf(s_uuid, sizeof(s_uuid),
           "5f9e4a40-%02x%02x-%02x%02x-%02x%02x-geelenamp00",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  snprintf(s_device_udn, sizeof(s_device_udn), "uuid:%s", s_uuid);
}

// ============================ [STATE] ===============================

TransportState s_state = TransportState::NoMedia;
char     s_current_uri[256] = {0};
char     s_current_meta[1024] = {0};
char     s_next_uri[256] = {0};
char     s_next_meta[1024] = {0};
char     s_title[128] = {0};
char     s_artist[64] = {0};
char     s_album[64] = {0};
uint32_t s_duration_s = 0;

void parse_didl_metadata(const char* didl, size_t n);

// ============================ [SOAP util] ===========================

// Caller owns the out buffer. Extracts contents between `<Name>` and
// `</Name>` anywhere in [buf, buf+n). Strips leading whitespace but does
// not XML-unescape — that's done separately by unescape_inplace().
bool extract_arg(const char* buf, size_t n, const char* name,
                 char* out, size_t out_cap) {
  if (!out_cap) return false;
  out[0] = 0;

  char open1[64], open2[64], close[64];
  int n1 = snprintf(open1, sizeof(open1), "<%s>", name);
  int n2 = snprintf(open2, sizeof(open2), "<%s ", name);
  int nc = snprintf(close, sizeof(close), "</%s>", name);
  if (n1 <= 0 || n2 <= 0 || nc <= 0) return false;

  const char* s = nullptr;
  size_t skip = 0;
  for (size_t i = 0; i + (size_t)n1 < n; ++i) {
    if (memcmp(buf + i, open1, (size_t)n1) == 0) {
      s = buf + i + n1;
      skip = 0;
      break;
    }
  }
  if (!s) {
    for (size_t i = 0; i + (size_t)n2 < n; ++i) {
      if (memcmp(buf + i, open2, (size_t)n2) == 0) {
        const char* gt = (const char*)memchr(buf + i, '>', n - i);
        if (!gt) return false;
        s = gt + 1;
        skip = 0;
        break;
      }
    }
  }
  if (!s) return false;

  const char* e = nullptr;
  for (const char* p = s; p + (size_t)nc <= buf + n; ++p) {
    if (memcmp(p, close, (size_t)nc) == 0) { e = p; break; }
  }
  if (!e) return false;

  size_t len = (size_t)(e - s);
  if (len >= out_cap) len = out_cap - 1;
  memcpy(out, s + skip, len);
  out[len] = 0;
  return true;
}

// XML entity unescape in place. Covers the five named entities UPnP
// cares about; numeric refs are left alone (rarely used for URIs).
void unescape_inplace(char* s) {
  char* r = s;
  char* w = s;
  while (*r) {
    if (*r != '&') { *w++ = *r++; continue; }
    if      (strncmp(r, "&amp;",  5) == 0) { *w++ = '&'; r += 5; }
    else if (strncmp(r, "&lt;",   4) == 0) { *w++ = '<'; r += 4; }
    else if (strncmp(r, "&gt;",   4) == 0) { *w++ = '>'; r += 4; }
    else if (strncmp(r, "&quot;", 6) == 0) { *w++ = '"'; r += 6; }
    else if (strncmp(r, "&apos;", 6) == 0) { *w++ = '\''; r += 6; }
    else                                    { *w++ = *r++; }
  }
  *w = 0;
}

esp_err_t send_soap_response(httpd_req_t* req, const char* service_type,
                             const char* action, const char* body_xml) {
  httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
  httpd_resp_set_hdr(req, "EXT", "");
  httpd_resp_set_hdr(req, "Server", "Geelen/1.0 UPnP/1.0 Geelen-DLNA/1.0");
  char* buf = (char*)malloc(4096 + strlen(body_xml) + strlen(action) + strlen(service_type));
  if (!buf) return ESP_FAIL;
  int n = sprintf(buf,
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body>"
    "<u:%sResponse xmlns:u=\"%s\">"
    "%s"
    "</u:%sResponse>"
    "</s:Body></s:Envelope>",
    action, service_type, body_xml, action);
  esp_err_t r = httpd_resp_send(req, buf, n);
  free(buf);
  return r;
}

esp_err_t send_soap_fault(httpd_req_t* req, int code, const char* desc) {
  httpd_resp_set_status(req, "500 Internal Server Error");
  httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
  char buf[512];
  int n = snprintf(buf, sizeof(buf),
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body><s:Fault><faultcode>s:Client</faultcode>"
    "<faultstring>UPnPError</faultstring>"
    "<detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
    "<errorCode>%d</errorCode><errorDescription>%s</errorDescription>"
    "</UPnPError></detail></s:Fault></s:Body></s:Envelope>", code, desc);
  return httpd_resp_send(req, buf, n);
}

// Read entire body. DLNA SOAP bodies are never larger than a few KB
// (DIDL-Lite metadata is the only thing that can balloon).
char* read_body(httpd_req_t* req, size_t* out_len) {
  size_t cap = req->content_len > 0 ? (size_t)req->content_len + 1 : 4096;
  if (cap > 32 * 1024) cap = 32 * 1024;
  char* buf = (char*)malloc(cap);
  if (!buf) return nullptr;
  size_t got = 0;
  while (got < cap - 1) {
    int r = httpd_req_recv(req, buf + got, cap - 1 - got);
    if (r <= 0) break;
    got += (size_t)r;
  }
  buf[got] = 0;
  *out_len = got;
  return buf;
}

const char* transport_state_name(TransportState s) {
  switch (s) {
    case TransportState::NoMedia:       return "NO_MEDIA_PRESENT";
    case TransportState::Stopped:       return "STOPPED";
    case TransportState::Playing:       return "PLAYING";
    case TransportState::Paused:        return "PAUSED_PLAYBACK";
    case TransportState::Transitioning: return "TRANSITIONING";
  }
  return "STOPPED";
}

// hh:mm:ss timecode formatter (DLNA uses this for RelTime/AbsTime).
void fmt_timecode(uint32_t secs, char* out, size_t cap) {
  uint32_t h = secs / 3600, m = (secs / 60) % 60, s = secs % 60;
  snprintf(out, cap, "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
}

// ============================ [SOAP avt] ============================

constexpr const char* AVT_URN = "urn:schemas-upnp-org:service:AVTransport:1";
constexpr const char* RC_URN  = "urn:schemas-upnp-org:service:RenderingControl:1";
constexpr const char* CM_URN  = "urn:schemas-upnp-org:service:ConnectionManager:1";

esp_err_t avt_handle(httpd_req_t* req, const char* action,
                     const char* body, size_t blen) {
  char tmp[1024];

  if (strcmp(action, "SetAVTransportURI") == 0) {
    char uri[256] = {0};
    char meta[1024] = {0};
    extract_arg(body, blen, "CurrentURI", uri, sizeof(uri));
    extract_arg(body, blen, "CurrentURIMetaData", meta, sizeof(meta));
    unescape_inplace(uri);
    unescape_inplace(meta);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(s_current_uri, uri, sizeof(s_current_uri));
    strlcpy(s_current_meta, meta, sizeof(s_current_meta));
    s_state = (uri[0] != 0) ? TransportState::Stopped : TransportState::NoMedia;
    parse_didl_metadata(meta, strlen(meta));
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "SetAVTransportURI: %s", uri);
    return send_soap_response(req, AVT_URN, "SetAVTransportURI", "");
  }

  if (strcmp(action, "SetNextAVTransportURI") == 0) {
    char uri[256] = {0};
    char meta[1024] = {0};
    extract_arg(body, blen, "NextURI", uri, sizeof(uri));
    extract_arg(body, blen, "NextURIMetaData", meta, sizeof(meta));
    unescape_inplace(uri); unescape_inplace(meta);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(s_next_uri, uri, sizeof(s_next_uri));
    strlcpy(s_next_meta, meta, sizeof(s_next_meta));
    xSemaphoreGive(s_mutex);
    return send_soap_response(req, AVT_URN, "SetNextAVTransportURI", "");
  }

  if (strcmp(action, "Play") == 0) {
    // A casting app issuing Play during the boot mode clip would have the
    // streamer abort the prompt and start music. Gate on the boot pair (15 s
    // hard cap so the SOAP request never hangs forever).
    voiceFeedbackWaitForBootDone(pdMS_TO_TICKS(15000));
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_current_uri[0]) {
      s_state = TransportState::Playing;
      mp3::streamer()->play(s_current_uri);
    }
    xSemaphoreGive(s_mutex);
    return send_soap_response(req, AVT_URN, "Play", "");
  }

  if (strcmp(action, "Pause") == 0) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = TransportState::Paused;
    mp3::streamer()->pause();
    xSemaphoreGive(s_mutex);
    return send_soap_response(req, AVT_URN, "Pause", "");
  }

  if (strcmp(action, "Stop") == 0) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = TransportState::Stopped;
    mp3::streamer()->stop();
    xSemaphoreGive(s_mutex);
    return send_soap_response(req, AVT_URN, "Stop", "");
  }

  if (strcmp(action, "GetTransportInfo") == 0) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const char* st = transport_state_name(s_state);
    xSemaphoreGive(s_mutex);
    int n = snprintf(tmp, sizeof(tmp),
      "<CurrentTransportState>%s</CurrentTransportState>"
      "<CurrentTransportStatus>OK</CurrentTransportStatus>"
      "<CurrentSpeed>1</CurrentSpeed>", st);
    (void)n;
    return send_soap_response(req, AVT_URN, "GetTransportInfo", tmp);
  }

  if (strcmp(action, "GetPositionInfo") == 0) {
    char dur[16], rel[16];
    uint32_t pos_ms = mp3::streamer()->positionMs();
    fmt_timecode(s_duration_s, dur, sizeof(dur));
    fmt_timecode(pos_ms / 1000, rel, sizeof(rel));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = snprintf(tmp, sizeof(tmp),
      "<Track>1</Track>"
      "<TrackDuration>%s</TrackDuration>"
      "<TrackMetaData>%s</TrackMetaData>"
      "<TrackURI>%s</TrackURI>"
      "<RelTime>%s</RelTime>"
      "<AbsTime>%s</AbsTime>"
      "<RelCount>2147483647</RelCount>"
      "<AbsCount>2147483647</AbsCount>",
      dur, "", s_current_uri, rel, rel);
    xSemaphoreGive(s_mutex);
    (void)n;
    return send_soap_response(req, AVT_URN, "GetPositionInfo", tmp);
  }

  if (strcmp(action, "GetMediaInfo") == 0) {
    char dur[16];
    fmt_timecode(s_duration_s, dur, sizeof(dur));
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = snprintf(tmp, sizeof(tmp),
      "<NrTracks>1</NrTracks>"
      "<MediaDuration>%s</MediaDuration>"
      "<CurrentURI>%s</CurrentURI>"
      "<CurrentURIMetaData></CurrentURIMetaData>"
      "<NextURI>%s</NextURI>"
      "<NextURIMetaData></NextURIMetaData>"
      "<PlayMedium>NETWORK</PlayMedium>"
      "<RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
      "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>",
      dur, s_current_uri, s_next_uri);
    xSemaphoreGive(s_mutex);
    (void)n;
    return send_soap_response(req, AVT_URN, "GetMediaInfo", tmp);
  }

  if (strcmp(action, "GetDeviceCapabilities") == 0) {
    return send_soap_response(req, AVT_URN, "GetDeviceCapabilities",
      "<PlayMedia>NETWORK,HDD</PlayMedia>"
      "<RecMedia>NOT_IMPLEMENTED</RecMedia>"
      "<RecQualityModes>NOT_IMPLEMENTED</RecQualityModes>");
  }

  if (strcmp(action, "GetTransportSettings") == 0) {
    return send_soap_response(req, AVT_URN, "GetTransportSettings",
      "<PlayMode>NORMAL</PlayMode>"
      "<RecQualityMode>NOT_IMPLEMENTED</RecQualityMode>");
  }

  if (strcmp(action, "GetCurrentTransportActions") == 0) {
    return send_soap_response(req, AVT_URN, "GetCurrentTransportActions",
      "<Actions>Play,Pause,Stop,Seek</Actions>");
  }

  if (strcmp(action, "Seek") == 0) {
    // Seek is a no-op for now — most streaming URIs are one-shot.
    return send_soap_response(req, AVT_URN, "Seek", "");
  }

  return send_soap_fault(req, 401, "Invalid Action");
}

// ============================ [SOAP rc] =============================

esp_err_t rc_handle(httpd_req_t* req, const char* action,
                    const char* body, size_t blen) {
  char tmp[256];

  if (strcmp(action, "GetVolume") == 0) {
    uint8_t vol = s_cfg ? s_cfg->getVolume() : 100;
    snprintf(tmp, sizeof(tmp),
             "<CurrentVolume>%u</CurrentVolume>", (unsigned)vol);
    return send_soap_response(req, RC_URN, "GetVolume", tmp);
  }

  if (strcmp(action, "SetVolume") == 0) {
    char v[8] = {0};
    extract_arg(body, blen, "DesiredVolume", v, sizeof(v));
    int vv = atoi(v);
    if (vv < 0) vv = 0;
    if (vv > 100) vv = 100;
    if (s_cfg) {
      s_cfg->setVolume((uint8_t)vv);
      if (&on_volume_changed) on_volume_changed();
    }
    return send_soap_response(req, RC_URN, "SetVolume", "");
  }

  if (strcmp(action, "GetMute") == 0) {
    bool m = s_cfg ? s_cfg->isMuted() : false;
    snprintf(tmp, sizeof(tmp),
             "<CurrentMute>%u</CurrentMute>", m ? 1u : 0u);
    return send_soap_response(req, RC_URN, "GetMute", tmp);
  }

  if (strcmp(action, "SetMute") == 0) {
    char v[8] = {0};
    extract_arg(body, blen, "DesiredMute", v, sizeof(v));
    bool m = (v[0] == '1' || v[0] == 't' || v[0] == 'T');
    if (s_cfg) {
      s_cfg->setMuted(m);
      if (&on_volume_changed) on_volume_changed();
    }
    return send_soap_response(req, RC_URN, "SetMute", "");
  }

  if (strcmp(action, "ListPresets") == 0) {
    return send_soap_response(req, RC_URN, "ListPresets",
      "<CurrentPresetNameList>FactoryDefaults</CurrentPresetNameList>");
  }
  if (strcmp(action, "SelectPreset") == 0) {
    return send_soap_response(req, RC_URN, "SelectPreset", "");
  }

  return send_soap_fault(req, 401, "Invalid Action");
}

// ============================ [SOAP cm] =============================

esp_err_t cm_handle(httpd_req_t* req, const char* action,
                    const char* /*body*/, size_t /*blen*/) {
  if (strcmp(action, "GetProtocolInfo") == 0) {
    return send_soap_response(req, CM_URN, "GetProtocolInfo",
      "<Source></Source>"
      "<Sink>"
        "http-get:*:audio/mpeg:*,"
        "http-get:*:audio/mp3:*,"
        "http-get:*:audio/x-mpeg:*,"
        "http-get:*:audio/mpeg3:*"
      "</Sink>");
  }
  if (strcmp(action, "GetCurrentConnectionIDs") == 0) {
    return send_soap_response(req, CM_URN, "GetCurrentConnectionIDs",
      "<ConnectionIDs>0</ConnectionIDs>");
  }
  if (strcmp(action, "GetCurrentConnectionInfo") == 0) {
    return send_soap_response(req, CM_URN, "GetCurrentConnectionInfo",
      "<RcsID>0</RcsID><AVTransportID>0</AVTransportID>"
      "<ProtocolInfo></ProtocolInfo><PeerConnectionManager></PeerConnectionManager>"
      "<PeerConnectionID>-1</PeerConnectionID>"
      "<Direction>Input</Direction><Status>OK</Status>");
  }
  return send_soap_fault(req, 401, "Invalid Action");
}

// ============================ [HTTP] ================================

// Pull "ServiceName#ActionName" out of `SOAPACTION: "urn:...:SvcName:1#ActName"`.
// The header arrives with quotes around the value.
bool soap_action_from_header(httpd_req_t* req, char* out, size_t cap) {
  size_t hl = httpd_req_get_hdr_value_len(req, "SOAPACTION");
  if (hl == 0) return false;
  if (hl + 1 > 256) return false;
  char buf[256];
  if (httpd_req_get_hdr_value_str(req, "SOAPACTION", buf, sizeof(buf)) != ESP_OK)
    return false;
  const char* hash = strchr(buf, '#');
  if (!hash) return false;
  ++hash;
  size_t n = strlen(hash);
  if (n && hash[n - 1] == '"') n--;
  if (n >= cap) n = cap - 1;
  memcpy(out, hash, n);
  out[n] = 0;
  return true;
}

esp_err_t handle_soap_dispatch(httpd_req_t* req, const char* svc) {
  char action[64] = {0};
  if (!soap_action_from_header(req, action, sizeof(action))) {
    return send_soap_fault(req, 401, "Invalid Action");
  }
  size_t blen = 0;
  char* body = read_body(req, &blen);
  if (!body) return ESP_FAIL;

  esp_err_t r;
  if      (strcmp(svc, "avt") == 0) r = avt_handle(req, action, body, blen);
  else if (strcmp(svc, "rc")  == 0) r = rc_handle (req, action, body, blen);
  else if (strcmp(svc, "cm")  == 0) r = cm_handle (req, action, body, blen);
  else r = send_soap_fault(req, 401, "Invalid Action");
  free(body);
  return r;
}

esp_err_t h_ctrl_avt(httpd_req_t* r) { return handle_soap_dispatch(r, "avt"); }
esp_err_t h_ctrl_rc (httpd_req_t* r) { return handle_soap_dispatch(r, "rc");  }
esp_err_t h_ctrl_cm (httpd_req_t* r) { return handle_soap_dispatch(r, "cm");  }

// Build and send the root MediaRenderer descriptor. Host IP is patched
// in at request time so control points see the address they reached us
// on (instead of whatever was live when SSDP last went out).
esp_err_t h_desc(httpd_req_t* req) {
  char name_buf[64] = {0};
  if (s_cfg) s_cfg->getDeviceName(name_buf, sizeof(name_buf));
  if (!name_buf[0]) strlcpy(name_buf, kDefaultDeviceName, sizeof(name_buf));
  const char* name = name_buf;
  char* buf = (char*)malloc(4096);
  if (!buf) return ESP_FAIL;
  int n = snprintf(buf, 4096,
    "<?xml version=\"1.0\"?>"
    "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" configId=\"1\">"
      "<specVersion><major>1</major><minor>0</minor></specVersion>"
      "<device>"
        "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
        "<friendlyName>%s</friendlyName>"
        "<manufacturer>Geelen Audio</manufacturer>"
        "<manufacturerURL>https://geelen.audio/</manufacturerURL>"
        "<modelDescription>ESP32 DLNA Renderer</modelDescription>"
        "<modelName>Pareto Amp</modelName>"
        "<modelNumber>1</modelNumber>"
        "<modelURL>https://geelen.audio/</modelURL>"
        "<serialNumber>%s</serialNumber>"
        "<UDN>%s</UDN>"
        "<serviceList>"
          "<service>"
            "<serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>"
            "<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
            "<SCPDURL>/dlna/scpd/avt.xml</SCPDURL>"
            "<controlURL>/dlna/ctrl/avt</controlURL>"
            "<eventSubURL>/dlna/evt/avt</eventSubURL>"
          "</service>"
          "<service>"
            "<serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>"
            "<serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
            "<SCPDURL>/dlna/scpd/rc.xml</SCPDURL>"
            "<controlURL>/dlna/ctrl/rc</controlURL>"
            "<eventSubURL>/dlna/evt/rc</eventSubURL>"
          "</service>"
          "<service>"
            "<serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>"
            "<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>"
            "<SCPDURL>/dlna/scpd/cm.xml</SCPDURL>"
            "<controlURL>/dlna/ctrl/cm</controlURL>"
            "<eventSubURL>/dlna/evt/cm</eventSubURL>"
          "</service>"
        "</serviceList>"
      "</device>"
    "</root>",
    name, s_uuid, s_device_udn);
  httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
  esp_err_t r = httpd_resp_send(req, buf, n);
  free(buf);
  return r;
}

// ============================ [XML] =================================
//
// SCPD XMLs. These are the minimum ActionList + serviceStateTable
// entries needed for the SOAP handlers above to pass compliance lints.
// They are `.rodata` strings, not dynamically built.

static const char* SCPD_AVT =
R"(<?xml version="1.0"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0">
 <specVersion><major>1</major><minor>0</minor></specVersion>
 <actionList>
  <action><name>SetAVTransportURI</name><argumentList>
   <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
   <argument><name>CurrentURI</name><direction>in</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument>
   <argument><name>CurrentURIMetaData</name><direction>in</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>
  </argumentList></action>
  <action><name>GetMediaInfo</name><argumentList>
   <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
   <argument><name>NrTracks</name><direction>out</direction><relatedStateVariable>NumberOfTracks</relatedStateVariable></argument>
   <argument><name>MediaDuration</name><direction>out</direction><relatedStateVariable>CurrentMediaDuration</relatedStateVariable></argument>
   <argument><name>CurrentURI</name><direction>out</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument>
   <argument><name>CurrentURIMetaData</name><direction>out</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>
   <argument><name>NextURI</name><direction>out</direction><relatedStateVariable>NextAVTransportURI</relatedStateVariable></argument>
   <argument><name>NextURIMetaData</name><direction>out</direction><relatedStateVariable>NextAVTransportURIMetaData</relatedStateVariable></argument>
   <argument><name>PlayMedium</name><direction>out</direction><relatedStateVariable>PlaybackStorageMedium</relatedStateVariable></argument>
   <argument><name>RecordMedium</name><direction>out</direction><relatedStateVariable>RecordStorageMedium</relatedStateVariable></argument>
   <argument><name>WriteStatus</name><direction>out</direction><relatedStateVariable>RecordMediumWriteStatus</relatedStateVariable></argument>
  </argumentList></action>
  <action><name>GetTransportInfo</name><argumentList>
   <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
   <argument><name>CurrentTransportState</name><direction>out</direction><relatedStateVariable>TransportState</relatedStateVariable></argument>
   <argument><name>CurrentTransportStatus</name><direction>out</direction><relatedStateVariable>TransportStatus</relatedStateVariable></argument>
   <argument><name>CurrentSpeed</name><direction>out</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>
  </argumentList></action>
  <action><name>GetPositionInfo</name><argumentList>
   <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
   <argument><name>Track</name><direction>out</direction><relatedStateVariable>CurrentTrack</relatedStateVariable></argument>
   <argument><name>TrackDuration</name><direction>out</direction><relatedStateVariable>CurrentTrackDuration</relatedStateVariable></argument>
   <argument><name>TrackMetaData</name><direction>out</direction><relatedStateVariable>CurrentTrackMetaData</relatedStateVariable></argument>
   <argument><name>TrackURI</name><direction>out</direction><relatedStateVariable>CurrentTrackURI</relatedStateVariable></argument>
   <argument><name>RelTime</name><direction>out</direction><relatedStateVariable>RelativeTimePosition</relatedStateVariable></argument>
   <argument><name>AbsTime</name><direction>out</direction><relatedStateVariable>AbsoluteTimePosition</relatedStateVariable></argument>
   <argument><name>RelCount</name><direction>out</direction><relatedStateVariable>RelativeCounterPosition</relatedStateVariable></argument>
   <argument><name>AbsCount</name><direction>out</direction><relatedStateVariable>AbsoluteCounterPosition</relatedStateVariable></argument>
  </argumentList></action>
  <action><name>Play</name><argumentList>
   <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
   <argument><name>Speed</name><direction>in</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>
  </argumentList></action>
  <action><name>Pause</name><argumentList>
   <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
  </argumentList></action>
  <action><name>Stop</name><argumentList>
   <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
  </argumentList></action>
  <action><name>Seek</name><argumentList>
   <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
   <argument><name>Unit</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SeekMode</relatedStateVariable></argument>
   <argument><name>Target</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SeekTarget</relatedStateVariable></argument>
  </argumentList></action>
 </actionList>
 <serviceStateTable>
  <stateVariable sendEvents="no"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>
  <stateVariable sendEvents="yes"><name>LastChange</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>TransportState</name><dataType>string</dataType>
   <allowedValueList><allowedValue>STOPPED</allowedValue><allowedValue>PLAYING</allowedValue>
   <allowedValue>PAUSED_PLAYBACK</allowedValue><allowedValue>TRANSITIONING</allowedValue>
   <allowedValue>NO_MEDIA_PRESENT</allowedValue></allowedValueList></stateVariable>
  <stateVariable sendEvents="no"><name>TransportStatus</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>TransportPlaySpeed</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>NumberOfTracks</name><dataType>ui4</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>CurrentTrack</name><dataType>ui4</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>CurrentTrackDuration</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>CurrentTrackMetaData</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>CurrentTrackURI</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>CurrentMediaDuration</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>AVTransportURI</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>AVTransportURIMetaData</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>NextAVTransportURI</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>NextAVTransportURIMetaData</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>RelativeTimePosition</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>AbsoluteTimePosition</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>RelativeCounterPosition</name><dataType>i4</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>AbsoluteCounterPosition</name><dataType>i4</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>PlaybackStorageMedium</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>RecordStorageMedium</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>RecordMediumWriteStatus</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>A_ARG_TYPE_SeekMode</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>A_ARG_TYPE_SeekTarget</name><dataType>string</dataType></stateVariable>
 </serviceStateTable>
</scpd>)";

static const char* SCPD_RC =
R"(<?xml version="1.0"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0">
 <specVersion><major>1</major><minor>0</minor></specVersion>
 <actionList>
  <action><name>GetVolume</name><argumentList>
   <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
   <argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>
   <argument><name>CurrentVolume</name><direction>out</direction><relatedStateVariable>Volume</relatedStateVariable></argument>
  </argumentList></action>
  <action><name>SetVolume</name><argumentList>
   <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
   <argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>
   <argument><name>DesiredVolume</name><direction>in</direction><relatedStateVariable>Volume</relatedStateVariable></argument>
  </argumentList></action>
  <action><name>GetMute</name><argumentList>
   <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
   <argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>
   <argument><name>CurrentMute</name><direction>out</direction><relatedStateVariable>Mute</relatedStateVariable></argument>
  </argumentList></action>
  <action><name>SetMute</name><argumentList>
   <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
   <argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>
   <argument><name>DesiredMute</name><direction>in</direction><relatedStateVariable>Mute</relatedStateVariable></argument>
  </argumentList></action>
  <action><name>ListPresets</name><argumentList>
   <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
   <argument><name>CurrentPresetNameList</name><direction>out</direction><relatedStateVariable>PresetNameList</relatedStateVariable></argument>
  </argumentList></action>
  <action><name>SelectPreset</name><argumentList>
   <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>
   <argument><name>PresetName</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_PresetName</relatedStateVariable></argument>
  </argumentList></action>
 </actionList>
 <serviceStateTable>
  <stateVariable sendEvents="no"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>A_ARG_TYPE_Channel</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>A_ARG_TYPE_PresetName</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="yes"><name>Volume</name><dataType>ui2</dataType>
    <allowedValueRange><minimum>0</minimum><maximum>100</maximum><step>1</step></allowedValueRange></stateVariable>
  <stateVariable sendEvents="yes"><name>Mute</name><dataType>boolean</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>PresetNameList</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="yes"><name>LastChange</name><dataType>string</dataType></stateVariable>
 </serviceStateTable>
</scpd>)";

static const char* SCPD_CM =
R"(<?xml version="1.0"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0">
 <specVersion><major>1</major><minor>0</minor></specVersion>
 <actionList>
  <action><name>GetProtocolInfo</name><argumentList>
   <argument><name>Source</name><direction>out</direction><relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument>
   <argument><name>Sink</name><direction>out</direction><relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument>
  </argumentList></action>
  <action><name>GetCurrentConnectionIDs</name><argumentList>
   <argument><name>ConnectionIDs</name><direction>out</direction><relatedStateVariable>CurrentConnectionIDs</relatedStateVariable></argument>
  </argumentList></action>
  <action><name>GetCurrentConnectionInfo</name><argumentList>
   <argument><name>ConnectionID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>
   <argument><name>RcsID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_RcsID</relatedStateVariable></argument>
   <argument><name>AVTransportID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_AVTransportID</relatedStateVariable></argument>
   <argument><name>ProtocolInfo</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ProtocolInfo</relatedStateVariable></argument>
   <argument><name>PeerConnectionManager</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionManager</relatedStateVariable></argument>
   <argument><name>PeerConnectionID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>
   <argument><name>Direction</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Direction</relatedStateVariable></argument>
   <argument><name>Status</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionStatus</relatedStateVariable></argument>
  </argumentList></action>
 </actionList>
 <serviceStateTable>
  <stateVariable sendEvents="yes"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="yes"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="yes"><name>CurrentConnectionIDs</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionID</name><dataType>i4</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>A_ARG_TYPE_RcsID</name><dataType>i4</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>A_ARG_TYPE_AVTransportID</name><dataType>i4</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>A_ARG_TYPE_ProtocolInfo</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionManager</name><dataType>string</dataType></stateVariable>
  <stateVariable sendEvents="no"><name>A_ARG_TYPE_Direction</name><dataType>string</dataType>
    <allowedValueList><allowedValue>Input</allowedValue><allowedValue>Output</allowedValue></allowedValueList></stateVariable>
  <stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionStatus</name><dataType>string</dataType></stateVariable>
 </serviceStateTable>
</scpd>)";

esp_err_t h_scpd_avt(httpd_req_t* r) { httpd_resp_set_type(r, "text/xml"); return httpd_resp_sendstr(r, SCPD_AVT); }
esp_err_t h_scpd_rc (httpd_req_t* r) { httpd_resp_set_type(r, "text/xml"); return httpd_resp_sendstr(r, SCPD_RC);  }
esp_err_t h_scpd_cm (httpd_req_t* r) { httpd_resp_set_type(r, "text/xml"); return httpd_resp_sendstr(r, SCPD_CM);  }

// GENA stub: accept SUBSCRIBE/UNSUBSCRIBE with a fake SID so clients
// that refuse to operate without eventing don't bail. We never call
// back.
esp_err_t h_evt_stub(httpd_req_t* req) {
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_hdr(req, "SID", "uuid:geelen-stub-0000");
  httpd_resp_set_hdr(req, "TIMEOUT", "Second-1800");
  httpd_resp_send(req, "", 0);
  return ESP_OK;
}

// Dispatch table — registered once from dlna::start().
struct UriReg {
  const char* uri;
  httpd_method_t method;
  esp_err_t (*handler)(httpd_req_t*);
};
const UriReg kUris[] = {
  { "/dlna/desc.xml",      HTTP_GET,  h_desc },
  { "/dlna/scpd/avt.xml",  HTTP_GET,  h_scpd_avt },
  { "/dlna/scpd/rc.xml",   HTTP_GET,  h_scpd_rc },
  { "/dlna/scpd/cm.xml",   HTTP_GET,  h_scpd_cm },
  { "/dlna/ctrl/avt",      HTTP_POST, h_ctrl_avt },
  { "/dlna/ctrl/rc",       HTTP_POST, h_ctrl_rc },
  { "/dlna/ctrl/cm",       HTTP_POST, h_ctrl_cm },
};

// GENA subscribe routes need a non-standard method; esp_http_server
// can't register HTTP method constants for SUBSCRIBE, so we rely on
// the catch-all in idf_http_server returning 200 for them, OR we
// register them as HTTP_GET (some clients use GET /evt/... which works).
// In practice, this stub rarely runs; casting apps tolerate missing GENA.

// Very shallow DIDL-Lite scrape for title/artist/album/duration. Works
// on the standard BubbleUPnP / MinimServer output; failures leave the
// corresponding field empty.
void parse_didl_metadata(const char* didl, size_t n) {
  extract_arg(didl, n, "dc:title", s_title, sizeof(s_title));
  extract_arg(didl, n, "upnp:artist", s_artist, sizeof(s_artist));
  if (!s_artist[0]) extract_arg(didl, n, "dc:creator", s_artist, sizeof(s_artist));
  extract_arg(didl, n, "upnp:album", s_album, sizeof(s_album));
  // duration lives in `res` element's duration attribute; we regex it
  // crudely. Form: duration="00:04:12.000".
  const char* d = strstr(didl, "duration=\"");
  if (d) {
    d += 10;
    unsigned h=0, m=0; float s=0;
    if (sscanf(d, "%u:%u:%f", &h, &m, &s) == 3) {
      s_duration_s = h * 3600 + m * 60 + (uint32_t)s;
      mp3::streamer()->setDurationMs(s_duration_s * 1000);
    }
  }
}

// ============================ [SSDP] ================================
//
// We run a single task that:
//   1. Owns a UDP socket bound to 0.0.0.0:1900 with IP_ADD_MEMBERSHIP
//      to 239.255.255.250.
//   2. Blocks in recvfrom() most of the time.
//   3. On incoming M-SEARCH, parses the ST and responds unicast if we
//      match.
//   4. Every 30 s, multicasts ssdp:alive announcements (IDs 1–5).

constexpr uint32_t kAnnounceIntervalMs = 30 * 1000;
constexpr int kMaxAge = 1800;

const char* kAdvertTypes[] = {
  "upnp:rootdevice",
  nullptr,  // uuid: filled at runtime
  "urn:schemas-upnp-org:device:MediaRenderer:1",
  "urn:schemas-upnp-org:service:AVTransport:1",
  "urn:schemas-upnp-org:service:RenderingControl:1",
  "urn:schemas-upnp-org:service:ConnectionManager:1",
};

void populate_host_ip() {
  esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!nif) { strlcpy(s_host_ip, "0.0.0.0", sizeof(s_host_ip)); return; }
  esp_netif_ip_info_t ip{};
  if (esp_netif_get_ip_info(nif, &ip) == ESP_OK) {
    snprintf(s_host_ip, sizeof(s_host_ip), "%u.%u.%u.%u",
      (unsigned)((ip.ip.addr >>  0) & 0xff),
      (unsigned)((ip.ip.addr >>  8) & 0xff),
      (unsigned)((ip.ip.addr >> 16) & 0xff),
      (unsigned)((ip.ip.addr >> 24) & 0xff));
  }
}

void build_usn(char* out, size_t cap, const char* nt) {
  if (strcmp(nt, "upnp:rootdevice") == 0 || strncmp(nt, "urn:", 4) == 0) {
    snprintf(out, cap, "%s::%s", s_device_udn, nt);
  } else {
    // uuid: NT is its own USN.
    strlcpy(out, s_device_udn, cap);
  }
}

void send_notify(int sock, const char* nt, const char* nts) {
  const char* eff_nt = nt ? nt : s_device_udn;
  char usn[128];
  build_usn(usn, sizeof(usn), eff_nt);
  char pkt[512];
  int n = snprintf(pkt, sizeof(pkt),
    "NOTIFY * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "CACHE-CONTROL: max-age=%d\r\n"
    "LOCATION: http://%s:%u/dlna/desc.xml\r\n"
    "NT: %s\r\n"
    "NTS: %s\r\n"
    "SERVER: Geelen/1.0 UPnP/1.0 Geelen-DLNA/1.0\r\n"
    "USN: %s\r\n"
    "\r\n",
    kMaxAge, s_host_ip, (unsigned)s_http_port, eff_nt, nts, usn);
  sockaddr_in dst{}; dst.sin_family = AF_INET;
  dst.sin_port = htons(1900);
  dst.sin_addr.s_addr = inet_addr("239.255.255.250");
  sendto(sock, pkt, n, 0, (sockaddr*)&dst, sizeof(dst));
}

void send_alive_burst(int sock) {
  for (size_t i = 0; i < sizeof(kAdvertTypes)/sizeof(kAdvertTypes[0]); ++i) {
    send_notify(sock, kAdvertTypes[i], "ssdp:alive");
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

void send_byebye(int sock) {
  for (size_t i = 0; i < sizeof(kAdvertTypes)/sizeof(kAdvertTypes[0]); ++i) {
    send_notify(sock, kAdvertTypes[i], "ssdp:byebye");
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

void send_search_response(int sock, const sockaddr_in& to, const char* st) {
  char usn[128]; build_usn(usn, sizeof(usn), st);
  char pkt[512];
  int n = snprintf(pkt, sizeof(pkt),
    "HTTP/1.1 200 OK\r\n"
    "CACHE-CONTROL: max-age=%d\r\n"
    "DATE: Thu, 01 Jan 2026 00:00:00 GMT\r\n"
    "EXT:\r\n"
    "LOCATION: http://%s:%u/dlna/desc.xml\r\n"
    "SERVER: Geelen/1.0 UPnP/1.0 Geelen-DLNA/1.0\r\n"
    "ST: %s\r\n"
    "USN: %s\r\n"
    "\r\n",
    kMaxAge, s_host_ip, (unsigned)s_http_port, st, usn);
  sendto(sock, pkt, n, 0, (const sockaddr*)&to, sizeof(to));
}

// Return true if the incoming M-SEARCH ST matches any of our types.
// If so, `resp_st` is populated with the matched string to echo back.
bool search_matches(const char* st, const char** resp_st) {
  if (strcmp(st, "ssdp:all") == 0) {
    *resp_st = "upnp:rootdevice";
    return true;
  }
  for (size_t i = 0; i < sizeof(kAdvertTypes)/sizeof(kAdvertTypes[0]); ++i) {
    const char* t = kAdvertTypes[i] ? kAdvertTypes[i] : s_device_udn;
    if (strcmp(st, t) == 0) {
      *resp_st = t;
      return true;
    }
  }
  // Device UUID check.
  if (strcmp(st, s_device_udn) == 0) {
    *resp_st = s_device_udn;
    return true;
  }
  return false;
}

void ssdp_task(void*) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "ssdp socket() failed");
    vTaskDelete(nullptr); return;
  }
  int one = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in bindaddr{}; bindaddr.sin_family = AF_INET;
  bindaddr.sin_port = htons(1900);
  bindaddr.sin_addr.s_addr = INADDR_ANY;
  if (bind(sock, (sockaddr*)&bindaddr, sizeof(bindaddr)) != 0) {
    ESP_LOGE(TAG, "ssdp bind() failed: %d", errno);
    close(sock); vTaskDelete(nullptr); return;
  }

  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
  mreq.imr_interface.s_addr = INADDR_ANY;
  if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
    ESP_LOGW(TAG, "ssdp IP_ADD_MEMBERSHIP failed (continuing): %d", errno);
  }

  // Small TTL so we don't spam beyond the local segment.
  uint8_t ttl = 2;
  setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

  s_ssdp_sock = sock;
  populate_host_ip();
  ESP_LOGI(TAG, "SSDP listening on %s:1900 (LOCATION http://%s:%u/dlna/desc.xml)",
           s_host_ip, s_host_ip, (unsigned)s_http_port);

  send_alive_burst(sock);
  TickType_t next_alive = xTaskGetTickCount() + pdMS_TO_TICKS(kAnnounceIntervalMs);

  char buf[1500];
  sockaddr_in from{};
  socklen_t flen = sizeof(from);
  while (s_running) {
    timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
    fd_set rfds; FD_ZERO(&rfds); FD_SET(sock, &rfds);
    int r = select(sock + 1, &rfds, nullptr, nullptr, &tv);
    if (r > 0 && FD_ISSET(sock, &rfds)) {
      int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &flen);
      if (n > 0) {
        buf[n] = 0;
        if (strncmp(buf, "M-SEARCH", 8) == 0) {
          const char* st_hdr = strcasestr(buf, "ST:");
          if (st_hdr) {
            st_hdr += 3;
            while (*st_hdr == ' ') ++st_hdr;
            char st[128]; size_t i = 0;
            while (*st_hdr && *st_hdr != '\r' && *st_hdr != '\n' && i < sizeof(st) - 1) {
              st[i++] = *st_hdr++;
            }
            st[i] = 0;
            // Strip trailing whitespace.
            while (i > 0 && isspace((unsigned char)st[i-1])) st[--i] = 0;
            const char* match = nullptr;
            if (search_matches(st, &match)) {
              send_search_response(sock, from, match);
            }
          }
        }
      }
    }
    if ((int32_t)(xTaskGetTickCount() - next_alive) >= 0) {
      populate_host_ip();
      send_alive_burst(sock);
      next_alive = xTaskGetTickCount() + pdMS_TO_TICKS(kAnnounceIntervalMs);
    }
  }

  send_byebye(sock);
  close(sock);
  s_ssdp_sock = -1;
  vTaskDelete(nullptr);
}

}  // namespace

// ============================ [public] ==============================

bool start(httpd_handle_t server, ConfigManager* cfg) {
  if (s_running) return true;
  s_http = server;
  s_cfg = cfg;
  if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
  init_uuid_from_mac();
  populate_host_ip();

  // The main server is on 80 — we don't get to pick here, so use it.
  s_http_port = 80;

  for (auto& u : kUris) {
    httpd_uri_t h{};
    h.uri = u.uri; h.method = u.method; h.handler = u.handler; h.user_ctx = nullptr;
    httpd_register_uri_handler(server, &h);
  }
  httpd_uri_t evt_get{ .uri="/dlna/evt/*", .method=HTTP_GET, .handler=h_evt_stub, .user_ctx=nullptr };
  httpd_register_uri_handler(server, &evt_get);

  s_running = true;
  xTaskCreatePinnedToCore(ssdp_task, "dlna-ssdp", 4096, nullptr, 4, &s_ssdp_task, 0);
  return true;
}

void stop() {
  s_running = false;
  // Give the SSDP task a tick to cleanly send byebye.
  for (int i = 0; i < 20 && s_ssdp_task; ++i) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  s_ssdp_task = nullptr;
}

Snapshot snapshot() {
  Snapshot s{};
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s.state = s_state;
  strlcpy(s.uri,    s_current_uri, sizeof(s.uri));
  strlcpy(s.title,  s_title,  sizeof(s.title));
  strlcpy(s.artist, s_artist, sizeof(s.artist));
  strlcpy(s.album,  s_album,  sizeof(s.album));
  s.duration_s = s_duration_s;
  s.position_s = mp3::streamer()->positionMs() / 1000;
  xSemaphoreGive(s_mutex);
  return s;
}

}  // namespace dlna
}  // namespace geelen_idf

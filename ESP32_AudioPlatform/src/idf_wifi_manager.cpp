// ESP-IDF WiFi manager implementation (STA + AP fallback + minimal captive DNS)

#include "idf_events.h"
#include "idf_wifi_manager.h"
#include "idf_config_manager.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"
#include <cstring>
#include <cstdio>

namespace geelen_idf {

static const char* TAG = "WiFi";
static const char* HOSTNAME = "Geelen-Audio";
static const char* AP_SSID = "Geelen-Audio";
static const char* AP_PASS = "geelen123";

static constexpr int WIFI_CONNECTED_BIT = BIT0;
static constexpr int WIFI_GOT_IP_BIT = BIT1;
static constexpr int WIFI_FAIL_BIT = BIT2;

static const char* reasonToCode_(uint8_t reason) {
  // ESP-IDF wifi_err_reason_t. Grouped into the SPA-facing codes in the plan.
  switch (reason) {
    case 201:  // WIFI_REASON_NO_AP_FOUND
      return "NO_AP_FOUND";
    case 2:    // WIFI_REASON_AUTH_EXPIRE
    case 14:   // WIFI_REASON_MIC_FAILURE
    case 15:   // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
    case 23:   // WIFI_REASON_802_1X_AUTH_FAILED
    case 36:   // IEEE "element / 4-way mismatch" seen on bad PSK
    case 202:  // WIFI_REASON_AUTH_FAIL
    case 204:  // WIFI_REASON_HANDSHAKE_TIMEOUT
    case 205:  // WIFI_REASON_CONNECTION_FAIL
      return "AUTH_FAIL";
    case 4:    // WIFI_REASON_ASSOC_EXPIRE
    case 5:    // WIFI_REASON_ASSOC_TOOMANY
    case 203:  // WIFI_REASON_ASSOC_FAIL
      return "ASSOC_FAIL";
    default:
      return "FAIL";
  }
}

static void fillDetail_(WifiProbeResult* out, uint8_t reason) {
  if (!out) return;
  std::snprintf(out->detail, sizeof(out->detail), "wifi disconnect reason %u", (unsigned)reason);
}

// Phase 1 reconnect ladder. Each entry is the delay (µs) BEFORE the
// next `esp_wifi_connect()` attempt after a STA_DISCONNECTED. Indexed
// by `reconnect_attempts_`; the last entry is reused for every attempt
// past `kMaxReconnectAttempts` if we somehow stay in retry mode (we
// drop into AP-fallback well before that today).
//
// Rationale: the previous implementation waited a flat 8 s and then
// flipped straight to AP fallback — i.e. zero reconnect attempts. The
// most common Wi-Fi blip on this device is a 2–5 s router DHCP renew or
// a Bluetooth coexistence stall; the new ladder retries before either
// would clear, so the user almost never sees the captive AP unless the
// SSID is genuinely unreachable.
static constexpr int64_t kReconnectDelayUs[] = {
    500'000,        //  500 ms — first attempt absorbs router blips
    1'000'000,      //  1 s
    2'000'000,      //  2 s
    5'000'000,      //  5 s
    10'000'000,     // 10 s
    30'000'000,     // 30 s — cap reused for every subsequent attempt
};
static constexpr size_t kReconnectLadderLen =
    sizeof(kReconnectDelayUs) / sizeof(kReconnectDelayUs[0]);
// After this many consecutive failed attempts (~50 s of cumulative
// backoff), trigger the AP fallback so the user can reconfigure.
static constexpr uint8_t kMaxReconnectAttempts = 6;

static uint32_t ipToU32_(const esp_ip4_addr_t& ip) {
  return ip.addr;
}

bool WifiManager::begin(ConfigManager* cfg) {
  cfg_ = cfg;
  if (!cfg_) return false;

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  sta_netif_ = esp_netif_create_default_wifi_sta();
  ap_netif_ = esp_netif_create_default_wifi_ap();

  // Hostname for STA interface (what your router often shows)
  if (sta_netif_) {
    esp_netif_set_hostname((esp_netif_t*)sta_netif_, HOSTNAME);
  }

  wifi_init_config_t cfg_wifi = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg_wifi));

  // Phase 5 — apply persisted Wi-Fi power-save mode. Default `None` keeps
  // the legacy behaviour (best for streaming, highest power draw); users
  // who care about battery life can opt into `MinModem` from the SPA.
  // We translate via a switch (rather than casting straight to wifi_ps_type_t)
  // so an unknown enum value can never reach the driver and so the IDF
  // header is the only place that knows the underlying integer codes.
  wifi_ps_type_t ps = WIFI_PS_NONE;
  const char* ps_name = "NONE";
  switch (cfg_->getWifiPsMode()) {
    case WifiPsMode::None:     ps = WIFI_PS_NONE;       ps_name = "NONE"; break;
    case WifiPsMode::MinModem: ps = WIFI_PS_MIN_MODEM;  ps_name = "MIN_MODEM"; break;
    case WifiPsMode::MaxModem: ps = WIFI_PS_MAX_MODEM;  ps_name = "MAX_MODEM"; break;
  }
  esp_err_t ps_err = esp_wifi_set_ps(ps);
  if (ps_err != ESP_OK) {
    ESP_LOGW(TAG, "esp_wifi_set_ps(%s) failed: %s — falling back to NONE",
             ps_name, esp_err_to_name(ps_err));
    esp_wifi_set_ps(WIFI_PS_NONE);
  } else {
    ESP_LOGI(TAG, "Wi-Fi power-save: %s", ps_name);
  }

  event_group_ = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiManager::wifiEventHandler_, this, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiManager::wifiEventHandler_, this, nullptr));

  esp_timer_create_args_t tmr{};
  tmr.callback = &WifiManager::reconnectTimerCb_;
  tmr.arg = this;
  tmr.name = "wifi_sta_reconn";
  tmr.dispatch_method = ESP_TIMER_TASK;
  esp_timer_handle_t th = nullptr;
  if (esp_timer_create(&tmr, &th) != ESP_OK) {
    ESP_LOGW(TAG, "STA reconnect timer create failed — runtime reconnect disabled");
  } else {
    reconnect_timer_ = th;
  }

  char ssid[WIFI_SSID_MAX] = {0};
  char pass[WIFI_PASS_MAX] = {0};
  cfg_->getWifiSsid(ssid, sizeof(ssid));
  cfg_->getWifiPass(pass, sizeof(pass));

  if (ssid[0] != '\0') {
    // Unverified creds (explicit NVS flag = 0) skip STA entirely so a
    // device that was flashed with garbage credentials never oscillates
    // AP → STA-fail → AP. A missing key (upgrade from pre-refactor
    // firmware) still tries STA once; GOT_IP then sets the flag.
    if (cfg_->wifiEverConnectedFlagPresent() && !cfg_->getWifiEverConnectedOk()) {
      ESP_LOGW(TAG, "SSID '%s' saved but never verified — starting AP, skipping STA", ssid);
      startApFallback_();
      return true;
    }
    startSta_(ssid, pass);
    if (waitForStaIp_(10000)) {
      status_.sta_connected = true;
      status_.ap_running = false;
      cfg_->setWifiEverConnectedOk(true);
      cfg_->save();
      ESP_LOGI(TAG, "STA up, AP not started");
      return true;
    }
    ESP_LOGW(TAG, "STA failed, falling back to AP");
  }

  startApFallback_();
  return true;
}

void WifiManager::startSta_(const char* ssid, const char* pass) {
  wifi_config_t wifi_config{};
  std::strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
  std::strncpy((char*)wifi_config.sta.password, pass ? pass : "", sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "Connecting STA to %s", ssid);
  ESP_ERROR_CHECK(esp_wifi_connect());
}

bool WifiManager::waitForStaIp_(uint32_t timeout_ms) {
  EventBits_t bits = xEventGroupWaitBits((EventGroupHandle_t)event_group_,
                                        WIFI_GOT_IP_BIT,
                                        pdFALSE,
                                        pdFALSE,
                                        pdMS_TO_TICKS(timeout_ms));
  return (bits & WIFI_GOT_IP_BIT) != 0;
}

void WifiManager::enableSoftApPhy_() {
  wifi_config_t ap_config{};
  std::strncpy((char*)ap_config.ap.ssid, AP_SSID, sizeof(ap_config.ap.ssid) - 1);
  std::strncpy((char*)ap_config.ap.password, AP_PASS, sizeof(ap_config.ap.password) - 1);
  ap_config.ap.ssid_len = std::strlen(AP_SSID);
  ap_config.ap.channel = 1;
  ap_config.ap.max_connection = 4;
  ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
  if (std::strlen(AP_PASS) == 0) ap_config.ap.authmode = WIFI_AUTH_OPEN;

  // APSTA (not pure AP) so `esp_wifi_scan_start` works while we are showing
  // the onboarding portal — `esp_wifi_scan_*` requires STA or APSTA mode.
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

  esp_err_t st = esp_wifi_start();
  if (st != ESP_OK) {
    // Often ESP_ERR_WIFI_CONN / ESP_ERR_INVALID_STATE when the driver is
    // already running and we only changed mode — safe to continue.
    ESP_LOGD(TAG, "esp_wifi_start after soft-AP config: %s", esp_err_to_name(st));
  }
}

void WifiManager::cancelStaReconnectTimer_() {
  if (!reconnect_timer_) return;
  esp_timer_stop((esp_timer_handle_t)reconnect_timer_);
}

void WifiManager::scheduleStaReconnect_() {
  if (!reconnect_timer_) return;
  esp_timer_stop((esp_timer_handle_t)reconnect_timer_);

  const size_t idx = (reconnect_attempts_ < kReconnectLadderLen)
                         ? (size_t)reconnect_attempts_
                         : (kReconnectLadderLen - 1);
  const int64_t delay = kReconnectDelayUs[idx];
  ESP_LOGI(TAG, "STA reconnect attempt %u in %lld ms",
           (unsigned)(reconnect_attempts_ + 1), (long long)(delay / 1000));
  esp_err_t e = esp_timer_start_once((esp_timer_handle_t)reconnect_timer_, delay);
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "STA reconnect timer start: %s", esp_err_to_name(e));
  }
}

void WifiManager::reconnectTimerCb_(void* arg) {
  auto* self = static_cast<WifiManager*>(arg);
  if (!self || !self->cfg_) return;
  if (self->status_.sta_connected) return;
  if (self->status_.ap_running) return;
  if (self->probing_) return;

  char ssid[WIFI_SSID_MAX]{};
  self->cfg_->getWifiSsid(ssid, sizeof(ssid));
  if (ssid[0] == '\0') return;  // never configured — nothing to reconnect to

  ++self->reconnect_attempts_;

  // Threshold reached → flip to AP fallback (same outcome as the legacy
  // 8 s timer, but only after we've actually exhausted the ladder).
  if (self->reconnect_attempts_ > kMaxReconnectAttempts) {
    ESP_LOGW(TAG, "STA reconnect ladder exhausted (%u attempts) — starting soft AP",
             (unsigned)self->reconnect_attempts_);
    self->startApFallback_();
    return;
  }

  ESP_LOGI(TAG, "STA reconnect attempt %u: esp_wifi_connect()",
           (unsigned)self->reconnect_attempts_);
  esp_err_t r = esp_wifi_connect();
  if (r != ESP_OK) {
    // Common when driver is mid-disconnect; just queue the next ladder step.
    ESP_LOGD(TAG, "esp_wifi_connect() returned %s", esp_err_to_name(r));
    self->scheduleStaReconnect_();
  }
}

void WifiManager::startApFallback_() {
  if (status_.ap_running) {
    ESP_LOGD(TAG, "startApFallback_: soft AP already running");
    return;
  }

  enableSoftApPhy_();

  status_.ap_running = true;
  status_.sta_connected = false;
  status_.sta_ip = 0;

  ESP_LOGI(TAG, "AP started: %s / %s (IP is usually 192.168.4.1)", AP_SSID, AP_PASS);

  // Phase 1: notify the supervisor (and any future Mode Manager / DLNA
  // SSDP gating) that we are now AP-only. Posted from both the cold-boot
  // fallback path (`begin()`) and the runtime ladder-exhausted path so
  // every transition into AP-only generates exactly one event.
  eventPost(EV_WIFI_AP_FALLBACK, nullptr, 0);

  // Start captive DNS responder task
  xTaskCreatePinnedToCore(&WifiManager::dnsTask_, "dns", 4096, this, 3, nullptr, 0);
}

void WifiManager::stopAp_() {
  if (!status_.ap_running) return;
  status_.ap_running = false;

  if (dns_sock_ >= 0) {
    close(dns_sock_);
    dns_sock_ = -1;
  }

  ESP_LOGI(TAG, "AP stopped");
}

void WifiManager::wifiEventHandler_(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  auto* self = static_cast<WifiManager*>(arg);
  if (!self) return;

  if (event_base == WIFI_EVENT) {
    if (event_id == WIFI_EVENT_STA_START) {
      ESP_LOGI(TAG, "STA start");
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
      ESP_LOGI(TAG, "STA connected");
      xEventGroupSetBits((EventGroupHandle_t)self->event_group_, WIFI_CONNECTED_BIT);
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
      auto* disc = (wifi_event_sta_disconnected_t*)event_data;
      uint8_t reason = disc ? (uint8_t)disc->reason : 0;
      ESP_LOGW(TAG, "STA disconnected (reason=%u)%s", (unsigned)reason,
               self->probing_ ? " [probe]" : "");
      self->status_.sta_connected = false;
      self->status_.sta_ip = 0;
      xEventGroupClearBits((EventGroupHandle_t)self->event_group_, WIFI_CONNECTED_BIT | WIFI_GOT_IP_BIT);

      if (self->probing_) {
        // Ignore the disconnect we just issued ourselves to leave the
        // previous AP (`ASSOC_LEAVE` / `AUTH_LEAVE`). Those are not a
        // probe result — the real AUTH_FAIL / NO_AP_FOUND arrives next.
        if (reason == 3 || reason == 8) {
          return;
        }
        self->probe_disc_reason_ = reason;
        xEventGroupSetBits((EventGroupHandle_t)self->event_group_, WIFI_FAIL_BIT);
        return;
      }

      ev_wifi_down_t payload{reason};
      eventPost(EV_WIFI_DOWN, &payload, sizeof(payload));

      // Phase 1 reconnect ladder. Skip the ladder entirely once the soft
      // AP is up: at that point the user is reconfiguring, and we don't
      // want a stale STA reconnect to flip the radio out from under the
      // captive portal.
      if (self->cfg_ && !self->status_.ap_running) {
        char ssid[WIFI_SSID_MAX]{};
        self->cfg_->getWifiSsid(ssid, sizeof(ssid));
        if (ssid[0] != '\0') {
          self->scheduleStaReconnect_();
        }
      }
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    auto* event = (ip_event_got_ip_t*)event_data;
    if (event) {
      self->status_.sta_ip = ipToU32_(event->ip_info.ip);
      ESP_LOGI(TAG, "STA got IP: %s%s",
               ip4addr_ntoa((const ip4_addr_t*)&event->ip_info.ip),
               self->probing_ ? " [probe]" : "");
    }
    xEventGroupSetBits((EventGroupHandle_t)self->event_group_, WIFI_GOT_IP_BIT);

    if (self->probing_) {
      // Keep the AP alive for the duration of onboarding. The HTTP
      // handler decides whether to persist + reboot.
      return;
    }

    self->cancelStaReconnectTimer_();
    self->reconnect_attempts_ = 0;
    self->status_.sta_connected = true;
    if (self->cfg_) self->cfg_->setWifiEverConnectedOk(true);

    ev_wifi_up_t payload{self->status_.sta_ip};
    eventPost(EV_WIFI_UP, &payload, sizeof(payload));

    self->stopAp_();
  }
}

// Minimal DNS responder: replies with A record pointing to AP IP (192.168.4.1).
// This is not a full DNS implementation; it's enough for captive-portal-ish behavior.
void WifiManager::dnsTask_(void* arg) {
  auto* self = static_cast<WifiManager*>(arg);
  if (!self) vTaskDelete(nullptr);

  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) vTaskDelete(nullptr);
  self->dns_sock_ = sock;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(53);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
    close(sock);
    self->dns_sock_ = -1;
    vTaskDelete(nullptr);
  }

  // AP default is 192.168.4.1
  const uint32_t ap_ip = inet_addr("192.168.4.1");

  while (self->status_.ap_running) {
    uint8_t buf[512];
    sockaddr_in from{};
    socklen_t fromlen = sizeof(from);
    int n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromlen);
    if (n < 12) continue;

    // DNS header: set QR=1, RA=1, copy ID, QDCOUNT=1, ANCOUNT=1
    buf[2] |= 0x80;  // QR=1
    buf[3] |= 0x80;  // RA=1
    buf[6] = 0x00; buf[7] = 0x01;  // ANCOUNT = 1
    buf[8] = 0x00; buf[9] = 0x00;  // NSCOUNT = 0
    buf[10] = 0x00; buf[11] = 0x00; // ARCOUNT = 0

    // Find end of question section (QNAME ends with 0 byte)
    int i = 12;
    while (i < n && buf[i] != 0) i++;
    i += 1;  // zero
    if (i + 4 > n) continue;  // QTYPE+QCLASS
    int q_end = i + 4;

    // Append answer: name as pointer to 0x0c, type A, class IN, TTL 30, RDLEN 4, RDATA ap_ip
    uint8_t ans[16];
    ans[0] = 0xC0; ans[1] = 0x0C;
    ans[2] = 0x00; ans[3] = 0x01; // A
    ans[4] = 0x00; ans[5] = 0x01; // IN
    ans[6] = 0x00; ans[7] = 0x00; ans[8] = 0x00; ans[9] = 0x1E; // TTL 30
    ans[10] = 0x00; ans[11] = 0x04; // RDLEN
    std::memcpy(&ans[12], &ap_ip, 4);

    // Send response
    if (q_end + (int)sizeof(ans) <= (int)sizeof(buf)) {
      std::memcpy(buf + q_end, ans, sizeof(ans));
      sendto(sock, buf, q_end + sizeof(ans), 0, (sockaddr*)&from, fromlen);
    }
  }

  close(sock);
  self->dns_sock_ = -1;
  vTaskDelete(nullptr);
}

const char* WifiManager::probeStateName() const {
  switch (probe_state_) {
    case WifiProbeState::Probing: return "probing";
    case WifiProbeState::Ok:      return "ok";
    case WifiProbeState::Failed:  return "failed";
    case WifiProbeState::Idle:
    default:                      return "idle";
  }
}

bool WifiManager::probeCredentials(const char* ssid, const char* pass, uint32_t timeout_ms,
                                   WifiProbeResult* out) {
  WifiProbeResult local{};
  if (!out) out = &local;
  *out = WifiProbeResult{};
  if (!ssid || ssid[0] == '\0') {
    std::strncpy(out->reason, "FAIL", sizeof(out->reason) - 1);
    std::strncpy(out->detail, "missing ssid", sizeof(out->detail) - 1);
    probe_state_ = WifiProbeState::Failed;
    probe_ok_ = false;
    return false;
  }
  if (probing_) {
    std::strncpy(out->reason, "FAIL", sizeof(out->reason) - 1);
    std::strncpy(out->detail, "probe already in progress", sizeof(out->detail) - 1);
    return false;
  }

  cancelStaReconnectTimer_();

  wifi_config_t saved{};
  (void)esp_wifi_get_config(WIFI_IF_STA, &saved);

  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&mode);
  if (mode == WIFI_MODE_AP || mode == WIFI_MODE_NULL) {
    enableSoftApPhy_();  // APSTA so STA probe works while the portal stays up
  } else if (mode == WIFI_MODE_STA) {
    // Keep STA mode: probing from the SPA while already joined.
  }

  wifi_config_t trial{};
  std::strncpy((char*)trial.sta.ssid, ssid, sizeof(trial.sta.ssid) - 1);
  std::strncpy((char*)trial.sta.password, pass ? pass : "", sizeof(trial.sta.password) - 1);
  trial.sta.threshold.authmode = WIFI_AUTH_OPEN;
  trial.sta.pmf_cfg.capable = true;
  trial.sta.pmf_cfg.required = false;

  probing_ = true;
  probe_state_ = WifiProbeState::Probing;
  probe_ok_ = false;
  probe_disc_reason_ = 0;
  xEventGroupClearBits((EventGroupHandle_t)event_group_,
                       WIFI_CONNECTED_BIT | WIFI_GOT_IP_BIT | WIFI_FAIL_BIT);

  eventPost(EV_WIFI_PROBE_START, nullptr, 0);
  ESP_LOGI(TAG, "probe start ssid='%s'", ssid);

  (void)esp_wifi_set_config(WIFI_IF_STA, &trial);
  (void)esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(50));
  esp_err_t cr = esp_wifi_connect();
  if (cr != ESP_OK) {
    ESP_LOGW(TAG, "probe esp_wifi_connect: %s", esp_err_to_name(cr));
  }

  EventBits_t bits = xEventGroupWaitBits(
      (EventGroupHandle_t)event_group_,
      WIFI_GOT_IP_BIT | WIFI_FAIL_BIT,
      pdTRUE,   // clear on exit
      pdFALSE,  // either bit
      pdMS_TO_TICKS(timeout_ms));

  bool ok = (bits & WIFI_GOT_IP_BIT) != 0;
  if (ok) {
    std::strncpy(out->reason, "OK", sizeof(out->reason) - 1);
    std::strncpy(out->detail, "got IP", sizeof(out->detail) - 1);
    uint32_t ip = status_.sta_ip;
    std::snprintf(out->ip, sizeof(out->ip), "%u.%u.%u.%u",
                  (unsigned)(ip & 0xff), (unsigned)((ip >> 8) & 0xff),
                  (unsigned)((ip >> 16) & 0xff), (unsigned)((ip >> 24) & 0xff));
    std::strncpy(probe_ssid_, ssid, sizeof(probe_ssid_) - 1);
    std::strncpy(probe_pass_, pass ? pass : "", sizeof(probe_pass_) - 1);
    probe_ok_ = true;
    probe_state_ = WifiProbeState::Ok;
  } else if ((bits & WIFI_FAIL_BIT) != 0) {
    std::strncpy(out->reason, reasonToCode_(probe_disc_reason_), sizeof(out->reason) - 1);
    fillDetail_(out, probe_disc_reason_);
    probe_ok_ = false;
    probe_state_ = WifiProbeState::Failed;
  } else {
    std::strncpy(out->reason, "TIMEOUT", sizeof(out->reason) - 1);
    std::strncpy(out->detail, "no IP within probe window", sizeof(out->detail) - 1);
    probe_ok_ = false;
    probe_state_ = WifiProbeState::Failed;
  }

  ev_wifi_probe_result_t payload{(uint8_t)(ok ? 1 : 0), probe_disc_reason_};
  eventPost(EV_WIFI_PROBE_RESULT, &payload, sizeof(payload));

  // Tear the trial STA down and restore the previous config so the
  // captive AP (or the previously-joined STA) stays reachable.
  (void)esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(50));
  (void)esp_wifi_set_config(WIFI_IF_STA, &saved);
  probing_ = false;
  xEventGroupClearBits((EventGroupHandle_t)event_group_,
                       WIFI_CONNECTED_BIT | WIFI_GOT_IP_BIT | WIFI_FAIL_BIT);

  if (!ok && saved.sta.ssid[0] != '\0' && !status_.ap_running) {
    (void)esp_wifi_connect();
  }

  ESP_LOGI(TAG, "probe %s ssid='%s' reason=%s", ok ? "ok" : "fail", ssid, out->reason);
  return ok;
}

bool WifiManager::commitValidated() {
  if (!probe_ok_ || probe_ssid_[0] == '\0' || !cfg_) return false;
  cfg_->setWifiSsid(probe_ssid_);
  cfg_->setWifiPass(probe_pass_);
  cfg_->setWifiEverConnectedOk(true);
  cfg_->save();
  ESP_LOGI(TAG, "commit validated ssid='%s'", probe_ssid_);
  return true;
}

}  // namespace geelen_idf


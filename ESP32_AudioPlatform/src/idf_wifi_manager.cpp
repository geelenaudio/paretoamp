// ESP-IDF WiFi manager implementation (STA + AP fallback + minimal captive DNS)

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

namespace geelen_idf {

static const char* TAG = "WiFi";
static const char* HOSTNAME = "Geelen-Audio";
static const char* AP_SSID = "Geelen-Audio";
static const char* AP_PASS = "geelen123";

static constexpr int WIFI_CONNECTED_BIT = BIT0;
static constexpr int WIFI_GOT_IP_BIT = BIT1;

// After STA disconnects, wait this long before starting the soft AP so a
// quick router reboot / DHCP renew does not drop the user into captive mode.
static constexpr int64_t kStaRecoverDelayUs = 8 * 1000 * 1000;

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

  // Audio streaming needs deterministic scheduling; disable WiFi power save.
  // This matches typical Arduino-core behavior and avoids long-term audio drift/overflows.
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  event_group_ = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiManager::wifiEventHandler_, this, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiManager::wifiEventHandler_, this, nullptr));

  esp_timer_create_args_t tmr{};
  tmr.callback = &WifiManager::recoveryTimerCb_;
  tmr.arg = this;
  tmr.name = "wifi_sta_recover";
  tmr.dispatch_method = ESP_TIMER_TASK;
  esp_timer_handle_t th = nullptr;
  if (esp_timer_create(&tmr, &th) != ESP_OK) {
    ESP_LOGW(TAG, "STA recovery timer create failed — runtime AP fallback disabled");
  } else {
    recovery_timer_ = th;
  }

  char ssid[WIFI_SSID_MAX] = {0};
  char pass[WIFI_PASS_MAX] = {0};
  cfg_->getWifiSsid(ssid, sizeof(ssid));
  cfg_->getWifiPass(pass, sizeof(pass));

  if (ssid[0] != '\0') {
    startSta_(ssid, pass);
    if (waitForStaIp_(10000)) {
      status_.sta_connected = true;
      status_.ap_running = false;
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

void WifiManager::cancelStaRecoveryTimer_() {
  if (!recovery_timer_) return;
  esp_timer_stop((esp_timer_handle_t)recovery_timer_);
}

void WifiManager::scheduleStaRecoveryCheck_() {
  if (!recovery_timer_) return;
  esp_timer_stop((esp_timer_handle_t)recovery_timer_);
  esp_err_t e = esp_timer_start_once((esp_timer_handle_t)recovery_timer_, kStaRecoverDelayUs);
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "STA recovery timer start: %s", esp_err_to_name(e));
  }
}

void WifiManager::recoveryTimerCb_(void* arg) {
  auto* self = static_cast<WifiManager*>(arg);
  if (!self || !self->cfg_) return;
  if (self->status_.sta_connected) return;
  if (self->status_.ap_running) return;

  char ssid[WIFI_SSID_MAX]{};
  self->cfg_->getWifiSsid(ssid, sizeof(ssid));
  if (ssid[0] == '\0') return;

  ESP_LOGW(TAG, "STA still down after disconnect — starting soft AP for reconfiguration");
  self->startApFallback_();
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
      int reason = disc ? (int)disc->reason : -1;
      ESP_LOGW(TAG, "STA disconnected (reason=%d)", reason);
      self->status_.sta_connected = false;
      self->status_.sta_ip = 0;
      xEventGroupClearBits((EventGroupHandle_t)self->event_group_, WIFI_CONNECTED_BIT | WIFI_GOT_IP_BIT);

      // Runtime recovery: once STA drops and stays down, bring the setup AP
      // back (same as cold-boot fallback) so the user can fix credentials.
      if (self->cfg_ && !self->status_.ap_running) {
        char ssid[WIFI_SSID_MAX]{};
        self->cfg_->getWifiSsid(ssid, sizeof(ssid));
        if (ssid[0] != '\0') {
          self->scheduleStaRecoveryCheck_();
        }
      }
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    self->cancelStaRecoveryTimer_();
    auto* event = (ip_event_got_ip_t*)event_data;
    if (event) {
      self->status_.sta_ip = ipToU32_(event->ip_info.ip);
      ESP_LOGI(TAG, "STA got IP: %s", ip4addr_ntoa((const ip4_addr_t*)&event->ip_info.ip));
    }
    self->status_.sta_connected = true;
    xEventGroupSetBits((EventGroupHandle_t)self->event_group_, WIFI_GOT_IP_BIT);
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

}  // namespace geelen_idf


// ESP-IDF WiFi manager: STA connect with AP fallback + captive DNS.

#pragma once

#include <cstdint>
#include "esp_event.h"

namespace geelen_idf {

class ConfigManager;

struct WifiStatus {
  bool sta_connected = false;
  bool ap_running = false;
  uint32_t sta_ip = 0;  // IPv4 address in network byte order
};

class WifiManager {
 public:
  WifiManager() = default;
  bool begin(ConfigManager* cfg);
  WifiStatus status() const { return status_; }
  /** True while the device is running the captive AP and STA has no IP yet. */
  bool isApOnly() const { return status_.ap_running && !status_.sta_connected; }

 private:
  void startSta_(const char* ssid, const char* pass);
  bool waitForStaIp_(uint32_t timeout_ms);
  /** Bring up Geelen-Audio soft AP + DNS (no-op if already running). */
  void startApFallback_();
  void stopAp_();
  void enableSoftApPhy_();
  void scheduleStaRecoveryCheck_();
  void cancelStaRecoveryTimer_();
  static void recoveryTimerCb_(void* arg);

  static void wifiEventHandler_(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
  static void dnsTask_(void* arg);

  ConfigManager* cfg_{nullptr};
  WifiStatus status_{};
  void* sta_netif_{nullptr};
  void* ap_netif_{nullptr};

  void* event_group_{nullptr};
  int dns_sock_{-1};
  void* recovery_timer_{nullptr};  // esp_timer_handle_t
};

}  // namespace geelen_idf


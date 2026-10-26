// ESP-IDF WiFi manager: STA connect with AP fallback + captive DNS.
//
// Onboarding is a two-step validate-then-commit flow:
//   1. `probeCredentials()` tries the supplied SSID/PSK without persisting
//      and without tearing down the captive AP. The phone stays associated.
//   2. `commitValidated()` persists the last successful probe and the
//      caller reboots into STA.
//
// Root-cause of the pre-refactor cycling: POST /api/wifi persisted then
// rebooted immediately. On boot the device tried STA for 10 s (no AP),
// failed, then flipped to APSTA — which kicked the phone off Geelen-Audio
// mid-onboarding. Validate-before-commit plus the `wifi_ever_connected_ok`
// NVS flag (skip STA on unverified creds) stop that loop.

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

enum class WifiProbeState : uint8_t {
  Idle = 0,
  Probing = 1,
  Ok = 2,
  Failed = 3,
};

struct WifiProbeResult {
  bool ok = false;
  char reason[24] = {0};   // AUTH_FAIL | NO_AP_FOUND | TIMEOUT | ASSOC_FAIL | FAIL
  char detail[80] = {0};
  char ip[16] = {0};
};

class WifiManager {
 public:
  WifiManager() = default;
  bool begin(ConfigManager* cfg);
  WifiStatus status() const { return status_; }
  /** True while the device is running the captive AP and STA has no IP yet. */
  bool isApOnly() const { return status_.ap_running && !status_.sta_connected; }

  /** DHCP hostname on the STA netif. No-op if STA netif is not up yet. */
  void setStaHostname(const char* hostname);

  WifiProbeState probeState() const { return probe_state_; }
  const char* probeStateName() const;

  /**
   * Try `ssid`/`pass` on the STA interface without persisting to NVS and
   * without taking the AP down. Blocks up to `timeout_ms` (typically ≥12 s).
   * On either outcome the previous STA config is restored so an already-
   * joined STA session (SPA `/wifi` page) stays reachable.
   */
  bool probeCredentials(const char* ssid, const char* pass, uint32_t timeout_ms,
                        WifiProbeResult* out);

  /**
   * Persist the last successful probe into NVS and mark
   * `wifi_ever_connected_ok`. Returns false if no probe succeeded in this
   * boot (caller should 400). Does NOT reboot — the HTTP handler does.
   */
  bool commitValidated();

 private:
  void startSta_(const char* ssid, const char* pass);
  bool waitForStaIp_(uint32_t timeout_ms);
  /** Bring up Geelen-Audio soft AP + DNS (no-op if already running). */
  void startApFallback_();
  void stopAp_();
  void enableSoftApPhy_();
  /** Schedule the next STA reconnect with exponential backoff (Phase 1). */
  void scheduleStaReconnect_();
  void cancelStaReconnectTimer_();
  static void reconnectTimerCb_(void* arg);

  static void wifiEventHandler_(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
  static void dnsTask_(void* arg);

  ConfigManager* cfg_{nullptr};
  WifiStatus status_{};
  void* sta_netif_{nullptr};
  void* ap_netif_{nullptr};

  void* event_group_{nullptr};
  int dns_sock_{-1};
  void* reconnect_timer_{nullptr};  // esp_timer_handle_t
  /** Consecutive reconnect attempts since the last successful IP acquisition.
   *  Drives the backoff ladder in `scheduleStaReconnect_`; reset to 0 on
   *  IP_EVENT_STA_GOT_IP and on cold start. */
  uint8_t reconnect_attempts_{0};

  bool probing_{false};
  WifiProbeState probe_state_{WifiProbeState::Idle};
  uint8_t probe_disc_reason_{0};
  char probe_ssid_[32]{};
  char probe_pass_[64]{};
  bool probe_ok_{false};
};

}  // namespace geelen_idf

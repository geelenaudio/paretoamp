// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors

#include "idf_mdns.h"

#include "idf_config_manager.h"
#include "idf_events.h"
#include "idf_identity.h"

#include "esp_log.h"
#include "mdns.h"

#include <cstring>

namespace geelen_idf {

static const char* TAG = "mdns";
static ConfigManager* s_cfg = nullptr;
static bool s_up = false;

static void start_locked_() {
  if (s_up || !s_cfg) return;
  char host[HOSTNAME_MAX];
  char name[DEV_NAME_MAX];
  s_cfg->getHostname(host, sizeof(host));
  s_cfg->getDeviceName(name, sizeof(name));
  if (!host[0]) {
    ESP_LOGW(TAG, "no hostname; skipping mDNS");
    return;
  }

  esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
    return;
  }
  err = mdns_hostname_set(host);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mdns_hostname_set(%s) failed: %s", host, esp_err_to_name(err));
    mdns_free();
    return;
  }
  if (name[0]) {
    (void)mdns_instance_name_set(name);
  }
  err = mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mdns_service_add _http._tcp failed: %s", esp_err_to_name(err));
  }
  s_up = true;
  ESP_LOGI(TAG, "advertising http://%s.local/ instance=\"%s\"", host,
           name[0] ? name : host);
}

void mdnsStop() {
  if (!s_up) return;
  mdns_free();
  s_up = false;
  ESP_LOGI(TAG, "stopped");
}

void mdnsApplyIdentity() {
  if (!s_up || !s_cfg) return;
  char host[HOSTNAME_MAX];
  char name[DEV_NAME_MAX];
  s_cfg->getHostname(host, sizeof(host));
  s_cfg->getDeviceName(name, sizeof(name));
  if (host[0]) (void)mdns_hostname_set(host);
  if (name[0]) (void)mdns_instance_name_set(name);
  ESP_LOGI(TAG, "identity updated host=%s instance=%s", host,
           name[0] ? name : "");
}

bool mdnsIsUp() { return s_up; }

static void on_wifi_up(void*, esp_event_base_t, int32_t, void*) {
  start_locked_();
}

static void on_wifi_down(void*, esp_event_base_t, int32_t, void*) {
  mdnsStop();
}

void mdnsBegin(ConfigManager* cfg) {
  s_cfg = cfg;
  eventRegister(EV_WIFI_UP, &on_wifi_up, nullptr);
  eventRegister(EV_WIFI_DOWN, &on_wifi_down, nullptr);
  eventRegister(EV_WIFI_AP_FALLBACK, &on_wifi_down, nullptr);
  ESP_LOGI(TAG, "responder armed (starts on STA IP)");
}

}  // namespace geelen_idf

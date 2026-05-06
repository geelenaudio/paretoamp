// ESP-IDF HTTP server: Web UI + REST API.

#pragma once

namespace geelen_idf {

class ConfigManager;
class WifiManager;

class HttpServer {
 public:
  HttpServer() = default;
  /** `wifi` is optional; when present, AP-only mode swaps the SPA shell for
   *  the inline Wi‑Fi onboarding page (also captures captive-portal probes). */
  bool begin(ConfigManager* cfg, WifiManager* wifi = nullptr);
  /** Opaque esp_http_server handle for registering extra URI handlers. */
  void* nativeHandle() const { return server_; }

 private:
  ConfigManager* cfg_{nullptr};
  WifiManager* wifi_{nullptr};
  void* server_{nullptr};
};

}  // namespace geelen_idf


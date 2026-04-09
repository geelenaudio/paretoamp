// ESP-IDF HTTP server: Web UI + REST API.

#pragma once

namespace geelen_idf {

class ConfigManager;

class HttpServer {
 public:
  HttpServer() = default;
  bool begin(ConfigManager* cfg);
  /** Opaque esp_http_server handle for registering extra URI handlers. */
  void* nativeHandle() const { return server_; }

 private:
  ConfigManager* cfg_{nullptr};
  void* server_{nullptr};
};

}  // namespace geelen_idf


#pragma once

#include <memory>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace geelen {
class AudioEngine;
}

namespace cspot {
class LoginBlob;
}

namespace geelen_idf {

class ConfigManager;
class HttpServer;

/** Spotify Connect (cspot): Zeroconf pairing, NVS credentials, PCM into AudioEngine. */
class SpotifyService {
 public:
  SpotifyService() = default;

  void setEngine(geelen::AudioEngine* eng) { engine_ = eng; }
  void setConfig(ConfigManager* cfg) { cfg_ = cfg; }

  bool attachHttpServer(HttpServer* http);
  bool begin();

  void notifyPairingComplete();

  /** Used by /spotify_info HTTP handlers (not member functions). */
  void ensurePairingBlob();
  std::shared_ptr<cspot::LoginBlob> loginBlob();

 private:
  static void spotifyMainTask(void* arg);

  geelen::AudioEngine* engine_{nullptr};
  ConfigManager* cfg_{nullptr};
  void* httpd_{nullptr};
  bool http_registered_{false};

  std::shared_ptr<cspot::LoginBlob> blob_;
  SemaphoreHandle_t pairing_sem_{nullptr};
};

}  // namespace geelen_idf

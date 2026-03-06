/**
 * Geelen Web Server – Async HTTP server, REST API, static UI.
 */

#ifndef GEELEN_WEB_SERVER_H
#define GEELEN_WEB_SERVER_H

namespace geelen {

class ConfigManager;
class AudioEngine;
class Snapclient;

class WebServer {
 public:
  WebServer() : config_(nullptr), engine_(nullptr), snapclient_(nullptr), started_(false) {}
  void setConfig(ConfigManager* c) { config_ = c; }
  void setEngine(AudioEngine* e) { engine_ = e; }
  void setSnapclient(Snapclient* s) { snapclient_ = s; }
  bool begin();
  void tick();

 private:
  void setupRoutes();
  ConfigManager* config_;
  AudioEngine* engine_;
  Snapclient* snapclient_;
  bool started_;
};

}  // namespace geelen

#endif  // GEELEN_WEB_SERVER_H

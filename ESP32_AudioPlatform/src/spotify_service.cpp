// Spotify Connect (cspot): Zeroconf, NVS credentials, PCM -> AudioEngine.

#include "spotify_service.h"

#include "audio_engine.h"
#include "idf_config_manager.h"
#include "idf_http_server.h"

#include "BellLogger.h"
#include "CSpotContext.h"
#include "LoginBlob.h"
#include "Logger.h"
#include "MDNSService.h"
#include "SpircHandler.h"
#include "TrackPlayer.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "mdns.h"

#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace geelen_idf {

namespace {

static const char* TAG = "Spotify";
static constexpr const char* kDeviceName = "Geelen-Amp";
static constexpr int kSpotifyPort = 80;

static std::string makeMdnsHostname(std::string_view in) {
  // RFC-ish: lowercase, digits, hyphen. Avoid empty hostname.
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-');
    if (ok) out.push_back(c);
    else if (c == '_' || c == ' ' || c == '.') out.push_back('-');
  }
  // Trim leading/trailing hyphens.
  while (!out.empty() && out.front() == '-') out.erase(out.begin());
  while (!out.empty() && out.back() == '-') out.pop_back();
  if (out.empty()) out = "geelen-amp";
  return out;
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return -1;
}

static std::string urlDecode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '+') {
      out += ' ';
    } else if (in[i] == '%' && i + 2 < in.size()) {
      int h1 = hexVal(in[i + 1]);
      int h2 = hexVal(in[i + 2]);
      if (h1 >= 0 && h2 >= 0) {
        out += (char)((h1 << 4) | h2);
        i += 2;
      } else {
        out += in[i];
      }
    } else {
      out += in[i];
    }
  }
  return out;
}

static bool parseFormUrlencoded(const char* body, std::map<std::string, std::string>& out) {
  out.clear();
  if (!body) return false;
  std::string b = body;
  size_t pos = 0;
  while (pos < b.size()) {
    size_t amp = b.find('&', pos);
    std::string pair = (amp == std::string::npos) ? b.substr(pos) : b.substr(pos, amp - pos);
    pos = (amp == std::string::npos) ? b.size() : amp + 1;
    size_t eq = pair.find('=');
    if (eq == std::string::npos) continue;
    std::string key = urlDecode(pair.substr(0, eq));
    std::string val = urlDecode(pair.substr(eq + 1));
    out[key] = val;
  }
  return true;
}

static void runSpotifySession(geelen::AudioEngine* engine, ConfigManager* cfg,
                              std::shared_ptr<cspot::LoginBlob> blob, bool save_creds) {
  auto ctx = cspot::Context::createFromBlob(blob);
  ctx->session->connectWithRandomAp();
  ctx->config.authData = ctx->session->authenticate(blob);

  if (ctx->config.authData.empty()) {
    ESP_LOGE(TAG, "Spotify authentication failed");
    return;
  }

  if (save_creds && cfg) {
    std::string cred = ctx->getCredentialsJson();
    cfg->setSpotifyCredentials(cred.c_str());
    cfg->save();
    ESP_LOGI(TAG, "Stored Spotify credentials in NVS");
  }

  ctx->session->startTask();
  auto handler = std::make_shared<cspot::SpircHandler>(ctx);
  // If the session is already connected by the time we construct the handler,
  // the connected-handler hook inside `SpircHandler` may never fire. Subscribe
  // explicitly so Spotify Connect control frames are received.
  handler->subscribeToMercury();

  handler->getTrackPlayer()->setDataCallback(
      [engine](uint8_t* data, size_t bytes, std::string_view) -> size_t {
        if (!engine || bytes == 0) return 0;
        engine->feedPCM(data, bytes);
        return bytes;
      });

  handler->setEventHandler(
      [engine](std::unique_ptr<cspot::SpircHandler::Event> event) {
        if (!engine) return;
        switch (event->eventType) {
          case cspot::SpircHandler::EventType::PLAYBACK_START:
          case cspot::SpircHandler::EventType::FLUSH:
          case cspot::SpircHandler::EventType::SEEK:
          case cspot::SpircHandler::EventType::PLAY_PAUSE:
          case cspot::SpircHandler::EventType::DISC:
          case cspot::SpircHandler::EventType::TRACK_INFO:
          case cspot::SpircHandler::EventType::DEPLETED:
          case cspot::SpircHandler::EventType::NEXT:
          case cspot::SpircHandler::EventType::PREV:
            // Keep the handler alive; these events are useful signals that the
            // Connect control plane is working, but AudioEngine doesn't need
            // explicit handling for them right now.
            break;
          case cspot::SpircHandler::EventType::VOLUME: {
            int v = std::get<int>(event->data);
            if (v < 0) v = 0;
            if (v > 65535) v = 65535;
            uint8_t pct = (uint8_t)((int64_t)v * 100 / 65535);
            engine->setVolume(pct);
            break;
          }
          default:
            break;
        }
      });

  ESP_LOGI(TAG, "Spotify session running");
  while (true) {
    ctx->session->handlePacket();
  }
}

}  // namespace

void SpotifyService::ensurePairingBlob() {
  if (!blob_) {
    blob_ = std::make_shared<cspot::LoginBlob>(kDeviceName);
  }
}

std::shared_ptr<cspot::LoginBlob> SpotifyService::loginBlob() { return blob_; }

void SpotifyService::spotifyMainTask(void* arg) {
  auto* self = static_cast<SpotifyService*>(arg);
  ConfigManager* cfg = self->cfg_;
  geelen::AudioEngine* engine = self->engine_;
  if (!cfg || !engine) {
    vTaskDelete(nullptr);
    return;
  }

  bell::setDefaultLogger();

  // Avoid large stack allocations in the Spotify task.
  auto cred = std::unique_ptr<char[]>(new char[SPOTIFY_CRED_JSON_MAX]);
  memset(cred.get(), 0, SPOTIFY_CRED_JSON_MAX);
  bool have_cred = cfg->getSpotifyCredentials(cred.get(), SPOTIFY_CRED_JSON_MAX);

  if (have_cred) {
    ESP_LOGI(TAG, "Starting Spotify from saved credentials");
    auto blob = std::make_shared<cspot::LoginBlob>(kDeviceName);
    try {
      blob->loadJson(std::string(cred.get()));
    } catch (...) {
      ESP_LOGE(TAG, "Invalid stored Spotify credentials");
      vTaskDelete(nullptr);
      return;
    }

    // Even with saved credentials, Spotify discovery requires mDNS advertisement.
    if (mdns_init() != ESP_OK && mdns_init() != ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "mdns_init issue (continuing)");
    }
    {
      std::string host = makeMdnsHostname(kDeviceName);
      esp_err_t e1 = mdns_hostname_set(host.c_str());
      esp_err_t e2 = mdns_instance_name_set(kDeviceName);
      if (e1 != ESP_OK) ESP_LOGW(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(e1));
      if (e2 != ESP_OK) ESP_LOGW(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(e2));
      ESP_LOGI(TAG, "mDNS hostname: %s", host.c_str());
    }
    std::map<std::string, std::string> txt = {
        {"VERSION", "1.0"},
        {"CPath", "/spotify_info"},
        {"Stack", "SP"},
    };
    auto mdns_keep = bell::MDNSService::registerService(
        blob->getDeviceName(), "_spotify-connect", "_tcp", "", kSpotifyPort, txt);
    ESP_LOGI(TAG, "mDNS: _spotify-connect._tcp port %d", kSpotifyPort);

    try {
      runSpotifySession(engine, cfg, blob, false);
    } catch (const std::exception& e) {
      ESP_LOGE(TAG, "Spotify session error: %s", e.what());
    } catch (...) {
      ESP_LOGE(TAG, "Spotify session error (unknown)");
    }

    (void)mdns_keep;
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI(TAG, "Waiting for Zeroconf pairing (Spotify app -> Connect to a device)");
  if (mdns_init() != ESP_OK && mdns_init() != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "mdns_init issue (continuing)");
  }

  // Ensure we set hostname/instance for robust mDNS advertisement (some networks/reflectors
  // ignore services without a proper host identity).
  {
    std::string host = makeMdnsHostname(kDeviceName);
    esp_err_t e1 = mdns_hostname_set(host.c_str());
    esp_err_t e2 = mdns_instance_name_set(kDeviceName);
    if (e1 != ESP_OK) ESP_LOGW(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(e1));
    if (e2 != ESP_OK) ESP_LOGW(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(e2));
    ESP_LOGI(TAG, "mDNS hostname: %s", host.c_str());
  }

  self->ensurePairingBlob();
  auto blob = self->loginBlob();
  if (!blob || !self->pairing_sem_) {
    ESP_LOGE(TAG, "Pairing not initialized");
    vTaskDelete(nullptr);
    return;
  }

  std::map<std::string, std::string> txt = {
      {"VERSION", "1.0"},
      {"CPath", "/spotify_info"},
      {"Stack", "SP"},
  };
  auto mdns_keep = bell::MDNSService::registerService(
      blob->getDeviceName(), "_spotify-connect", "_tcp", "", kSpotifyPort, txt);

  ESP_LOGI(TAG, "mDNS: _spotify-connect._tcp port %d", kSpotifyPort);

  xSemaphoreTake(self->pairing_sem_, portMAX_DELAY);
  ESP_LOGI(TAG, "Zeroconf credentials received, authenticating");

  try {
    runSpotifySession(engine, cfg, blob, true);
  } catch (const std::exception& e) {
    ESP_LOGE(TAG, "Spotify session error: %s", e.what());
  } catch (...) {
    ESP_LOGE(TAG, "Spotify session error (unknown)");
  }

  (void)mdns_keep;
  vTaskDelete(nullptr);
}

static esp_err_t spotifyGetHandler(httpd_req_t* req) {
  auto* self = static_cast<SpotifyService*>(req->user_ctx);
  if (!self) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ctx");
  self->ensurePairingBlob();
  auto blob = self->loginBlob();
  if (!blob) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "pairing not ready");
  }
  std::string json = blob->buildZeroconfInfo();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json.c_str(), json.size());
}

static esp_err_t spotifyPostHandler(httpd_req_t* req) {
  auto* self = static_cast<SpotifyService*>(req->user_ctx);
  if (!self) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ctx");

  // Avoid large stack allocations in the HTTPD thread.
  size_t cap = 4096;
  size_t want = (req->content_len > 0 && (size_t)req->content_len < cap) ? (size_t)req->content_len : (cap - 1);
  auto body = std::unique_ptr<char[]>(new char[want + 1]);
  size_t got = 0;
  while (got < want) {
    int r = httpd_req_recv(req, body.get() + got, (int)(want - got));
    if (r < 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
    if (r == 0) break;
    got += (size_t)r;
  }
  body[got] = '\0';

  std::map<std::string, std::string> form;
  parseFormUrlencoded(body.get(), form);

  self->ensurePairingBlob();
  auto blob = self->loginBlob();
  if (!blob) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "blob");

  try {
    blob->loadZeroconfQuery(form);
  } catch (...) {
    ESP_LOGE(TAG, "loadZeroconfQuery failed");
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
  }

  const char* ok = "{\"status\":101,\"spotifyError\":0,\"statusString\":\"ERROR-OK\"}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, ok, HTTPD_RESP_USE_STRLEN);

  self->notifyPairingComplete();
  return ESP_OK;
}

void SpotifyService::notifyPairingComplete() {
  if (pairing_sem_) {
    xSemaphoreGive(pairing_sem_);
  }
}

bool SpotifyService::attachHttpServer(HttpServer* http) {
  if (!http || http_registered_) return http_registered_;
  httpd_ = http->nativeHandle();
  if (!httpd_) return false;

  httpd_uri_t get_uri = {.uri = "/spotify_info",
                         .method = HTTP_GET,
                         .handler = spotifyGetHandler,
                         .user_ctx = this};
  httpd_uri_t post_uri = {.uri = "/spotify_info",
                          .method = HTTP_POST,
                          .handler = spotifyPostHandler,
                          .user_ctx = this};
  httpd_handle_t h = (httpd_handle_t)httpd_;
  if (httpd_register_uri_handler(h, &get_uri) != ESP_OK) return false;
  if (httpd_register_uri_handler(h, &post_uri) != ESP_OK) return false;
  http_registered_ = true;
  ESP_LOGI(TAG, "Registered /spotify_info for Zeroconf");
  return true;
}

bool SpotifyService::begin() {
  if (!engine_ || !cfg_) {
    ESP_LOGW(TAG, "begin: engine or config not set");
    return false;
  }
  if (cfg_->getAudioSource() != AudioSource::Spotify) {
    ESP_LOGI(TAG, "Audio source is not Spotify; skipping cspot");
    return true;
  }
  if (!http_registered_) {
    ESP_LOGE(TAG, "attachHttpServer was not called before begin()");
    return false;
  }

  // Avoid large stack allocations in the ESP-IDF main task.
  auto cred = std::unique_ptr<char[]>(new char[SPOTIFY_CRED_JSON_MAX]);
  memset(cred.get(), 0, SPOTIFY_CRED_JSON_MAX);
  bool have_cred = cfg_->getSpotifyCredentials(cred.get(), SPOTIFY_CRED_JSON_MAX);
  if (!have_cred) {
    ensurePairingBlob();
    if (!pairing_sem_) {
      pairing_sem_ = xSemaphoreCreateBinary();
    }
    if (!pairing_sem_) {
      ESP_LOGE(TAG, "Failed to create pairing semaphore");
      return false;
    }
  }

  BaseType_t ok =
      // FreeRTOS expects stack depth in *words*, not bytes.
      // 8192 words ~= 32KB stack.
  // cspot auth/TLS can be stack-hungry; give it a bit more headroom.
      xTaskCreatePinnedToCore(spotifyMainTask, "spotify", 16384, this, 7, nullptr, 0);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create spotify task");
    return false;
  }
  return true;
}

}  // namespace geelen_idf

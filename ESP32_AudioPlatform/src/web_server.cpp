/**
 * Geelen Web Server – REST API and static UI.
 */

#include "web_server.h"
#include "config_manager.h"
#include "audio_engine.h"
#include "snapclient.h"
#include <ESPAsyncWebServer.h>
#include <Arduino.h>
#include <ArduinoJson.h>

static AsyncWebServer* server = nullptr;
static const uint16_t WEB_PORT = 80;

static const char INDEX_HTML[] PROGMEM = R"raw(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Geelen Audio</title></head><body>
<h1>Geelen Audio Amp</h1>
<p>Volume: <span id="vol">--</span> <button onclick="setVol(0)">0</button><button onclick="setVol(50)">50</button><button onclick="setVol(100)">100</button></p>
<p>Source: <span id="src">--</span> <button onclick="setSource(0)">Bluetooth</button><button onclick="setSource(1)">Network</button></p>
<p>WiFi: <input id="ssid" placeholder="SSID"/><input id="pass" type="password" placeholder="Password"/><button onclick="saveWifi()">Save</button></p>
<p><a href="/api/status">API status</a></p>
<script>
function saveWifi(){ var s=document.getElementById('ssid').value; var p=document.getElementById('pass').value; fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)}).then(r=>r.json()).then(d=>alert(d.ok?'Saved. Reconnect device to WiFi.':'')); }
</script>
function setVol(v){ fetch('/api/volume?v='+v).then(r=>r.json()).then(d=>{ document.getElementById('vol').textContent=d.volume; }); }
function setSource(s){ fetch('/api/source?s='+s,{method:'POST'}).then(r=>r.json()).then(d=>{ document.getElementById('src').textContent=d.source===0?'Bluetooth':'Network'; }); }
fetch('/api/status').then(r=>r.json()).then(d=>{
  document.getElementById('vol').textContent=d.volume||'--';
  document.getElementById('src').textContent=d.source===0?'Bluetooth':'Network';
});
</script></body></html>
)raw";

namespace geelen {

bool WebServer::begin() {
  if (started_) return true;
  if (!config_ || !engine_) return false;
  server = new AsyncWebServer(WEB_PORT);
  setupRoutes();
  server->begin();
  started_ = true;
  return true;
}

void WebServer::tick() {
  (void)config_;
}

void WebServer::setupRoutes() {
  server->on("/", HTTP_GET, [this](AsyncWebServerRequest* req) {
    req->send(200, "text/html", INDEX_HTML);
  });

  server->on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (!config_ || !engine_) { req->send(500, "application/json", "{}"); return; }
    StaticJsonDocument<256> doc;
    doc["volume"] = config_->getVolume();
    doc["source"] = (uint8_t)config_->getAudioSource();
    doc["preset"] = config_->getPresetId();
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server->on("/api/volume", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (!config_ || !engine_) { req->send(500, "application/json", "{}"); return; }
    if (req->hasParam("v")) {
      uint8_t v = (uint8_t)req->getParam("v")->value().toInt();
      if (v > 100) v = 100;
      config_->setVolume(v);
      engine_->setVolume(v);
    }
    StaticJsonDocument<64> doc;
    doc["volume"] = config_->getVolume();
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server->on("/api/source", HTTP_POST, [this](AsyncWebServerRequest* req) {
    if (!config_ || !req->hasParam("s")) { req->send(400, "application/json", "{}"); return; }
    int s = req->getParam("s")->value().toInt();
    config_->setAudioSource(s ? AudioSource::Network : AudioSource::Bluetooth);
    StaticJsonDocument<64> doc;
    doc["source"] = (uint8_t)config_->getAudioSource();
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server->on("/api/eq", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (!config_ || !engine_) { req->send(500, "application/json", "{}"); return; }
    int8_t gains[EQ_BANDS];
    config_->getEQGains(gains);
    StaticJsonDocument<512> doc;
    JsonArray arr = doc.createNestedArray("bands");
    for (size_t i = 0; i < EQ_BANDS; i++) arr.add(gains[i]);
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server->on("/api/wifi", HTTP_POST, [this](AsyncWebServerRequest* req) {
    if (!config_) { req->send(500, "application/json", "{}"); return; }
    if (req->hasParam("ssid", true)) {
      String ssid = req->getParam("ssid", true)->value();
      String pass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : "";
      config_->setWifiSsid(ssid.c_str());
      config_->setWifiPass(pass.c_str());
      config_->save();
    }
    StaticJsonDocument<64> doc;
    doc["ok"] = true;
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server->on("/api/snapcast", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (!config_) { req->send(500, "application/json", "{}"); return; }
    char host[SNAPCAST_HOST_MAX];
    config_->getSnapcastHost(host, sizeof(host));
    StaticJsonDocument<128> doc;
    doc["host"] = host;
    doc["port"] = config_->getSnapcastPort();
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server->on("/api/factory_reset", HTTP_POST, [this](AsyncWebServerRequest* req) {
    if (!config_) { req->send(500, "application/json", "{}"); return; }
    config_->factoryReset();
    StaticJsonDocument<64> doc;
    doc["ok"] = true;
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
    delay(500);
    ESP.restart();
  });

  server->onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "Not found");
  });
}

}  // namespace geelen

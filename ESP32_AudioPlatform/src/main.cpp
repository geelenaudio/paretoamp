/**
 * Geelen Audio Amp – Amped-ESP32 (non-plus, non-S)
 * I2S init → stable clock → GPIO 13 (Amp Enable) → Config → Audio Engine → A2DP Sink.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include "config_manager.h"
#include "audio_engine.h"
#include "snapclient.h"
#include "web_server.h"
#define A2DP_I2S_AUDIOTOOLS 0  /* Use Print output, not AudioTools */
#include "BluetoothA2DPSink.h"

#ifndef PIN_I2S_WS
#define PIN_I2S_WS  25
#endif
#ifndef PIN_I2S_BCK
#define PIN_I2S_BCK 26
#endif
#ifndef PIN_I2S_DATA
#define PIN_I2S_DATA 22
#endif
#ifndef PIN_AMP_ENABLE
#define PIN_AMP_ENABLE 13
#endif

constexpr int SAMPLE_RATE   = 44100;
constexpr unsigned long I2S_STABLE_MS = 50;

static bool i2s_ok = false;
static i2s_chan_handle_t i2s_tx_chan = NULL;
static geelen::ConfigManager configManager;
static geelen::AudioEngine audioEngine;

class A2DPToEngine : public Print {
 public:
  void setEngine(geelen::AudioEngine* eng) { engine_ = eng; }
  size_t write(uint8_t c) override {
    if (engine_) engine_->feedPCM(&c, 1);
    return 1;
  }
  size_t write(const uint8_t* data, size_t len) override {
    if (engine_) engine_->feedPCM(data, len);
    return len;
  }
 private:
  geelen::AudioEngine* engine_ = nullptr;
};

static A2DPToEngine a2dpOutput;
static BluetoothA2DPSink a2dpSink(a2dpOutput);
static geelen::Snapclient snapclient;
static geelen::WebServer webServer;
static DNSServer dnsServer;
static bool apMode = false;
static const char* AP_SSID = "Geelen-Audio";
static const char* AP_PASS = "geelen123";
static const char* HOSTNAME = "Geelen-Audio";

static void stopApMode() {
  if (!apMode) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  apMode = false;
  Serial.println("[WiFi] AP stopped (STA connected).");
}

static void printMemoryStats(const char* tag) {
  Serial.printf("[%s] heap=%u min_heap=%u psram=%u heap8=%u\n",
                tag,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap(),
                (unsigned)ESP.getFreePsram(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

static bool initI2S() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  if (i2s_new_channel(&chan_cfg, &i2s_tx_chan, NULL) != ESP_OK) return false;

  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)PIN_I2S_BCK,
      .ws = (gpio_num_t)PIN_I2S_WS,
      .dout = (gpio_num_t)PIN_I2S_DATA,
      .din = I2S_GPIO_UNUSED,
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv = false,
      },
    },
  };
  if (i2s_channel_init_std_mode(i2s_tx_chan, &std_cfg) != ESP_OK) {
    i2s_del_channel(i2s_tx_chan);
    i2s_tx_chan = NULL;
    return false;
  }
  if (i2s_channel_enable(i2s_tx_chan) != ESP_OK) {
    i2s_del_channel(i2s_tx_chan);
    i2s_tx_chan = NULL;
    return false;
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nGeelen Audio Amp – Amped-ESP32");
  printMemoryStats("boot");

  // Ensure consistent DHCP hostname on your LAN (instead of esp32-<mac>)
  WiFi.setHostname(HOSTNAME);
  WiFi.softAPsetHostname(HOSTNAME);

  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_START:
        Serial.println("[WiFi] STA start");
        break;
      case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        Serial.println("[WiFi] STA connected");
        break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.printf("[WiFi] STA got IP: %s\n", WiFi.localIP().toString().c_str());
        stopApMode();
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        Serial.printf("[WiFi] STA disconnected (reason=%d)\n", (int)info.wifi_sta_disconnected.reason);
        break;
      default:
        break;
    }
  });

  if (!initI2S()) {
    Serial.println("I2S init failed");
    return;
  }
  i2s_ok = true;
  Serial.printf("I2S OK (WS=%d BCK=%d DATA=%d)\n", PIN_I2S_WS, PIN_I2S_BCK, PIN_I2S_DATA);

  delay(I2S_STABLE_MS);
  pinMode(PIN_AMP_ENABLE, OUTPUT);
  digitalWrite(PIN_AMP_ENABLE, HIGH);
  Serial.println("Amp enable (GPIO 13) HIGH");

  if (!configManager.begin()) {
    Serial.println("Config Manager init failed");
    return;
  }
  Serial.printf("Config: volume=%u preset=%u\n",
                configManager.getVolume(), configManager.getPresetId());

  if (!audioEngine.begin()) {
    Serial.println("Audio Engine init failed");
    return;
  }
  int8_t eq[geelen::EQ_BANDS];
  configManager.getEQGains(eq);
  static_assert(geelen::EQ_BANDS == geelen::AUDIO_EQ_BANDS, "EQ band count mismatch");
  audioEngine.setEQGains(eq);
  audioEngine.setVolume(configManager.getVolume());
  audioEngine.setI2SChannel(i2s_tx_chan);
  snapclient.setEngine(&audioEngine);

  char ssid[geelen::WIFI_SSID_MAX], pass[geelen::WIFI_PASS_MAX];
  if (configManager.getWifiSsid(ssid, sizeof(ssid)) && ssid[0] != '\0') {
    configManager.getWifiPass(pass, sizeof(pass));
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass[0] != '\0' ? pass : nullptr);
    Serial.printf("WiFi connecting to %s\n", ssid);
    for (int i = 0; i < 100 && WiFi.status() != WL_CONNECTED; i++) delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    dnsServer.start(53, "*", WiFi.softAPIP());
    apMode = true;
    Serial.printf("AP started: %s / %s at %s\n", AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
  } else {
    Serial.printf("[WiFi] STA up: %s\n", WiFi.localIP().toString().c_str());
  }

  printMemoryStats("pre_web");
  geelen::AudioSource src = configManager.getAudioSource();
  if (src == geelen::AudioSource::Network) {
    char host[geelen::SNAPCAST_HOST_MAX];
    if (configManager.getSnapcastHost(host, sizeof(host)) && host[0] != '\0') {
      snapclient.connect(host, configManager.getSnapcastPort());
      a2dpOutput.setEngine(nullptr);
    } else {
      src = geelen::AudioSource::Bluetooth;
      a2dpOutput.setEngine(&audioEngine);
    }
  }

  webServer.setConfig(&configManager);
  webServer.setEngine(&audioEngine);
  webServer.setSnapclient(&snapclient);
  if (webServer.begin()) {
    Serial.println("Web UI: http://<device-ip>/");
  }

  printMemoryStats("pre_audio_source");
  if (src == geelen::AudioSource::Bluetooth) {
    if (apMode && ESP.getFreeHeap() < 60000) {
      Serial.println("A2DP not started (low heap in AP mode). Configure WiFi first, then reboot.");
      printMemoryStats("a2dp_skipped");
      return;
    }
    a2dpOutput.setEngine(&audioEngine);
    a2dpSink.start("Geelen Audio");
    Serial.println("A2DP Sink started (Geelen Audio). Connect from phone.");
  } else {
    Serial.println("Source: Network (Snapcast).");
  }
  printMemoryStats("post_audio_source");
}

void loop() {
  if (!i2s_ok) {
    delay(1000);
    return;
  }
  if (apMode) dnsServer.processNextRequest();
  configManager.tick(millis());
  webServer.tick();
  if (configManager.getAudioSource() == geelen::AudioSource::Network) {
    snapclient.tick();
  }
  audioEngine.process();
  delay(1);
}

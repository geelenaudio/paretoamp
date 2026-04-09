// ESP-IDF entrypoint for the new pure-IDF PlatformIO environment.
// (Arduino env excludes this file via build_src_filter.)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include <cmath>

#include "audio_engine.h"
#include "idf_config_manager.h"
#include "idf_wifi_manager.h"
#include "idf_http_server.h"
#include "idf_snapclient.h"
#include "esp_timer.h"
#include "spotify_service.h"
#include "idf_a2dp_sink.h"

static constexpr gpio_num_t PIN_I2S_WS = GPIO_NUM_25;
static constexpr gpio_num_t PIN_I2S_BCK = GPIO_NUM_26;
static constexpr gpio_num_t PIN_I2S_DATA = GPIO_NUM_22;
static constexpr gpio_num_t PIN_AMP_ENABLE = GPIO_NUM_13;
static constexpr uint32_t I2S_STABLE_MS = 50;

static geelen::AudioEngine g_engine;
static i2s_chan_handle_t g_i2s_tx = nullptr;
static geelen_idf::ConfigManager g_cfg;
static geelen_idf::WifiManager g_wifi;
static geelen_idf::HttpServer g_http;
static geelen_idf::Snapclient g_snap;
static geelen_idf::SpotifyService g_spotify;
static geelen_idf::A2dpSink g_a2dp;

static bool init_i2s() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  // Increase DMA buffering to tolerate bursty A2DP delivery + WiFi/BT coexist jitter.
  // Defaults (6 * 240 frames) are only ~33ms of audio at 44.1kHz stereo 16-bit.
  // Keep this moderate so BT controller can still allocate internal RAM.
  // (~185ms: 16 * 512 frames)
  chan_cfg.dma_desc_num = 16;
  chan_cfg.dma_frame_num = 512;
  if (i2s_new_channel(&chan_cfg, &g_i2s_tx, nullptr) != ESP_OK) return false;

  i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(geelen::SAMPLE_RATE);
  // Force APLL for accurate audio clocks. If the I2S clock runs slow, audio pitch drops
  // and the ring buffer will overflow (producer faster than consumer).
  clk_cfg.clk_src = I2S_CLK_SRC_APLL;
  clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  i2s_std_config_t std_cfg = {
    .clk_cfg = clk_cfg,
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = PIN_I2S_BCK,
      .ws = PIN_I2S_WS,
      .dout = PIN_I2S_DATA,
      .din = I2S_GPIO_UNUSED,
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv = false,
      },
    },
  };

  if (i2s_channel_init_std_mode(g_i2s_tx, &std_cfg) != ESP_OK) return false;
  if (i2s_channel_enable(g_i2s_tx) != ESP_OK) return false;
  return true;
}

static void amp_enable_after_i2s_stable() {
  // Avoid power-on pops: enable amp only after I2S clock is stable.
  gpio_config_t io_conf = {};
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = 1ULL << PIN_AMP_ENABLE;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&io_conf);

  gpio_set_level(PIN_AMP_ENABLE, 0);
  vTaskDelay(pdMS_TO_TICKS(I2S_STABLE_MS));
  gpio_set_level(PIN_AMP_ENABLE, 1);
  ESP_LOGI("geelen", "Amp enable (GPIO %d) HIGH", (int)PIN_AMP_ENABLE);
}

static void audio_task(void*) {
  while (true) {
    g_engine.process();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

static void tone_task(void*) {
  constexpr float freq = 440.f;
  float phase = 0.f;
  constexpr float two_pi = 6.28318530718f;

  // Produce ~10ms at a time
  constexpr int frames = geelen::SAMPLE_RATE / 100;
  int16_t buf[frames * 2];

  while (true) {
    for (int i = 0; i < frames; i++) {
      float s = sinf(phase) * 12000.f;
      int16_t v = (int16_t)s;
      buf[i * 2] = v;
      buf[i * 2 + 1] = v;
      phase += two_pi * freq / (float)geelen::SAMPLE_RATE;
      if (phase > two_pi) phase -= two_pi;
    }
    g_engine.feedPCM((const uint8_t*)buf, sizeof(buf));
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

extern "C" void app_main(void) {
  ESP_LOGI("geelen", "Geelen Audio Amp – ESP-IDF env boot");
  g_cfg.begin();
  g_wifi.begin(&g_cfg);
  g_http.begin(&g_cfg);

  g_spotify.setConfig(&g_cfg);
  g_spotify.setEngine(&g_engine);
  g_spotify.attachHttpServer(&g_http);

  g_a2dp.setEngine(&g_engine);

  if (!g_engine.begin()) {
    ESP_LOGE("geelen", "AudioEngine begin failed");
    return;
  }
  if (!init_i2s()) {
    ESP_LOGE("geelen", "I2S init failed");
    return;
  }
  g_engine.setI2SChannel((void*)g_i2s_tx);
  amp_enable_after_i2s_stable();

  const geelen_idf::AudioSource src = g_cfg.getAudioSource();
  ESP_LOGI("geelen", "Selected source: %u", (unsigned)src);

  xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 20, nullptr, 1);

  // Start Spotify Connect after AudioEngine is ready (PCM ring + I2S).
  g_spotify.begin();

  // Start BT Classic A2DP sink only when BT is selected.
  if (src == geelen_idf::AudioSource::Bluetooth) g_a2dp.begin("Geelen Audio");

  char host[geelen_idf::SNAPCAST_HOST_MAX] = {0};
  g_cfg.getSnapcastHost(host, sizeof(host));
  const bool snap_on = host[0] != '\0';
  const bool spotify_on = src == geelen_idf::AudioSource::Spotify;

  // Only run Snapclient when Network source is selected *and* host is set.
  if (src == geelen_idf::AudioSource::Network && snap_on) {
    g_snap.setEngine(&g_engine);
    g_snap.connect(host, g_cfg.getSnapcastPort());
    xTaskCreatePinnedToCore([](void*) {
      while (true) {
        g_snap.tick();
        vTaskDelay(pdMS_TO_TICKS(1));
      }
    }, "snap", 4096, nullptr, 6, nullptr, 0);
  }

  // No tone fallback in normal operation. When no active source is configured/running,
  // the device should stay silent (and avoid filling the PCM ring buffer).
}

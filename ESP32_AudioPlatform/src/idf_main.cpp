// ESP-IDF entrypoint for the Amped-ESP32 firmware.

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
#include "idf_a2dp_sink.h"
#include "idf_cpu_sampler.h"
#include "idf_logs.h"
#include "idf_dlna.h"
#include "idf_mp3_streamer.h"

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
static geelen_idf::A2dpSink g_a2dp;

// Hook invoked by the HTTP server after POST /api/snapcast persists new
// host/port values. Drops the current socket so the snap task's retry loop
// picks up the freshly-saved config on its next iteration (< ~1 s).
// Declared weak on the caller side (idf_http_server.cpp) so that side stays
// decoupled from Snapclient.
void on_snapcast_config_changed() {
  g_snap.disconnect();
}

// Hook invoked by the HTTP server after POST /api/mode persists a new
// source selection. Until the runtime source manager lands (Phase 9 wires
// DLNA on top of it), there is no clean way to stop the Snap task or
// start DLNA in-place — tasks pinned at boot stick around. A brief reboot
// is the simplest correct answer: the on-disk NVS already reflects the
// new source, so app_main picks the right task graph after restart.
//
// The only transition that doesn't need the reboot is the no-op case
// (same->same), which the API layer never actually invokes this hook for.
namespace geelen_idf { void scheduleRestart(unsigned delay_ms); }

extern "C" void on_audio_source_changed(unsigned prev, unsigned next) {
  if (prev == next) return;
  ESP_LOGI("geelen", "Audio source %u -> %u: scheduling reboot to reconfigure task graph",
           prev, next);
  geelen_idf::scheduleRestart(500);
}

// Pull the latest per-channel volume + mute state out of NVS and push it
// into the audio engine. Invoked by idf_api after POST /api/volume and
// also called from app_main at boot so the engine starts with the saved
// values rather than its 100/100 constructor default.
static void apply_volume_from_cfg() {
  g_engine.setVolumeLR(g_cfg.getVolumeL(), g_cfg.getVolumeR());
  g_engine.setMuted(g_cfg.isMuted());
}
static void apply_channels_from_cfg() {
  g_engine.setChannels(
      g_cfg.getChannelLOn(),
      g_cfg.getChannelROn(),
      g_cfg.getStereoMode() == geelen_idf::StereoMode::Mono);
}

extern "C" void on_volume_changed()   { apply_volume_from_cfg(); }
extern "C" void on_channels_changed() { apply_channels_from_cfg(); }

// Push both EQ cascades from NVS into the engine, then apply headroom.
// In Auto mode the headroom dB equals -peak gain of the enabled cascade(s)
// so the post-EQ signal tops out at ≤ 0 dBFS regardless of input. In
// Manual mode the user's chosen cap applies verbatim. Either way we call
// setHeadroomDb *after* the curves are in place so the peak computation
// inside computeEqPeakGainDb() reflects the new bands.
static void apply_eq_from_cfg() {
  geelen_idf::ParametricBand cal_nvs[geelen_idf::PARAMETRIC_BANDS];
  geelen_idf::ParametricBand pref_nvs[geelen_idf::PARAMETRIC_BANDS];
  g_cfg.getEqCalibration(cal_nvs);
  g_cfg.getEqPreference(pref_nvs);

  geelen::EqBand cal[geelen::AUDIO_EQ_BANDS];
  geelen::EqBand pref[geelen::AUDIO_EQ_BANDS];
  for (size_t i = 0; i < geelen::AUDIO_EQ_BANDS; ++i) {
    cal[i]  = { cal_nvs[i].freq_hz,  cal_nvs[i].gain_db,  cal_nvs[i].q };
    pref[i] = { pref_nvs[i].freq_hz, pref_nvs[i].gain_db, pref_nvs[i].q };
  }
  g_engine.setEqCurve(geelen::EQ_LAYER_CALIBRATION, cal,
                      g_cfg.getEqCalibrationEnabled());
  g_engine.setEqCurve(geelen::EQ_LAYER_PREFERENCE, pref,
                      g_cfg.getEqPreferenceEnabled());

  float db = g_cfg.getHeadroomManualDb();
  if (g_cfg.getHeadroomMode() == geelen_idf::HeadroomMode::Auto) {
    const float peak = g_engine.computeEqPeakGainDb();
    db = peak > 0.f ? -peak : 0.f;
  }
  g_engine.setHeadroomDb(db);
}

extern "C" void on_eq_changed() { apply_eq_from_cfg(); }

// Query hook used by idf_api to put auto_db on the response. We return the
// raw peak dB of the enabled cascades; the API layer flips the sign.
extern "C" float compute_eq_peak_db() { return g_engine.computeEqPeakGainDb(); }

static bool init_i2s() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  // Increase DMA buffering to tolerate bursty A2DP delivery + WiFi/BT coexist jitter.
  // Defaults (6 * 240 frames) are only ~33ms of audio at 44.1kHz stereo 16-bit.
  // Keep this moderate so BT controller can still allocate internal RAM.
  // (~185ms: 16 * 512 frames)
  chan_cfg.dma_desc_num = 16;
  chan_cfg.dma_frame_num = 512;
  // When the engine stops writing (source went idle, Snapcast stream dropped,
  // A2DP paused) the DMA descriptors must NOT keep looping their last
  // contents — that's what caused the "sine wave that won't stop until the
  // device is unplugged" bug. With auto_clear=true, the driver zero-fills
  // the DMA buffers on TX underrun so the amp goes silent automatically.
  chan_cfg.auto_clear = true;
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
  // During steady playback, g_engine.process() blocks inside i2s_channel_write
  // until a DMA descriptor drains (~3 ms per 128-frame chunk) — that blocking
  // write is what paces this loop to realtime. Adding a vTaskDelay(1) on top
  // of it would cap the drain at ~128 frames / 10 ms ≈ 12.8 kHz, i.e. ~29% of
  // realtime; the Snapclient producer (44.1 kHz) would then out-run us and
  // peg the ring buffer full of stale audio (buf=~88200, overflows climbing,
  // hearable as constant "Knister" crackling).
  //
  // Only sleep a tick when process() early-returned, i.e. prefill / rebuffer
  // / I2S backpressure with written=0 — in those cases nothing blocked
  // internally, so we'd otherwise busy-spin Core 1 and starve IDLE1.
  while (true) {
    if (!g_engine.process()) {
      vTaskDelay(1);
    }
  }
}

static void configure_uart_log_levels() {
  // Keep global default at INFO and only adjust tags we care about (avoids httpd/wifi/lwip spam).
  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("geelen", ESP_LOG_INFO);
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("httpd", ESP_LOG_WARN);
  esp_log_level_set("httpd_uri", ESP_LOG_WARN);
  esp_log_level_set("httpd_txrx", ESP_LOG_WARN);
  esp_log_level_set("httpd_parse", ESP_LOG_WARN);
  esp_log_level_set("httpd_sess", ESP_LOG_WARN);
  esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
  esp_log_level_set("event", ESP_LOG_WARN);
}

extern "C" void app_main(void) {
  configure_uart_log_levels();
  // Hook the logger *before* the first INFO so the live-stream ring captures
  // the full boot log, including NVS / Wi-Fi banner lines.
  geelen_idf::logsInit();
  ESP_LOGI("geelen", "Geelen Audio Amp – ESP-IDF env boot");
  g_cfg.begin();
  // Start the CPU sampler early so its calibration baseline has seconds of
  // mostly-idle time to settle before WiFi/BT/audio kick in.
  geelen_idf::cpuSamplerStart();
  g_wifi.begin(&g_cfg);
  g_http.begin(&g_cfg);

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
  // Apply the saved audio state to the engine before the amp goes live so
  // the very first buffer played respects the user's persisted settings.
  apply_volume_from_cfg();
  apply_channels_from_cfg();
  apply_eq_from_cfg();
  amp_enable_after_i2s_stable();

  const geelen_idf::AudioSource src = g_cfg.getAudioSource();
  ESP_LOGI("geelen", "Selected source: %u", (unsigned)src);

  // Native DLNA renderer. SSDP advertising is always on when the user
  // enables DLNA in the config, regardless of the active source: this
  // way casting apps can "wake" the device by issuing Play.
  // TODO(phase9b): gate SSDP on WiFi IP acquisition event rather than
  // waiting here — on cold boot without saved credentials this runs
  // before netif has an IP and send_alive_burst() then spews a useless
  // 0.0.0.0 LOCATION until the next announce interval.
  if (g_cfg.getDlnaEnabled()) {
    geelen_idf::mp3::streamer()->setEngine(&g_engine);
    geelen_idf::dlna::start((httpd_handle_t)g_http.nativeHandle(), &g_cfg);
  }

  xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 20, nullptr, 1);

  // Start BT Classic A2DP sink only when BT is selected.
  if (src == geelen_idf::AudioSource::Bluetooth) g_a2dp.begin("Geelen Audio");

  char host[geelen_idf::SNAPCAST_HOST_MAX] = {0};
  g_cfg.getSnapcastHost(host, sizeof(host));
  const bool snap_on = host[0] != '\0';

  if (src == geelen_idf::AudioSource::Network) {
    if (snap_on) {
      ESP_LOGI("geelen", "Snapclient loop starting -> %s:%u", host,
               (unsigned)g_cfg.getSnapcastPort());
    } else {
      ESP_LOGW("geelen",
               "Source=Network but no Snapcast host configured yet; the snap "
               "task will idle until one is saved via POST /api/snapcast (or "
               "the Snapcast section of the Web UI).");
    }
    g_snap.setEngine(&g_engine);

    // Snapclient task:
    //
    //   while (true):
    //     if not connected: read host/port from NVS, try to connect, back off
    //     else:             tick() — blocks in recv() until bytes arrive
    //
    // Pinned to Core 1 (alongside audio_task) on purpose. Core 0 runs WiFi,
    // BT, and LwIP at priority 22–23; leaving Snap there caused the task to
    // get preempted for tens of ms at a time, the TCP socket buffer filled
    // up, the RX window closed, snapserver backed off, and the audio ring
    // ran dry. Core 1 has no WiFi/BT tasks so Snap runs the moment LwIP
    // signals data-ready. Priority 5 is below audio_task (20) so it cannot
    // starve I2S.
    //
    // tick() now uses blocking recv() (see idf_snapclient.cpp). This removes
    // the old 10 ms poll latency: the task sleeps between frames and wakes
    // immediately when a WireChunk arrives. No inner vTaskDelay needed —
    // blocking recv() yields CPU to IDLE1 while we're asleep.
    //
    // Putting the connect inside the task avoids two foot-guns we used to
    // hit with a one-shot g_snap.connect() in app_main:
    //   1. Boot race — the old code dialed ~60 ms after `STA got IP`, before
    //      ARP / routes had warmed up, and if that single SYN failed the
    //      client was silent forever. Now we just retry every 2 s.
    //   2. UI-saved host was never applied — POST /api/snapcast updated NVS
    //      but nothing re-read it. Now the task re-reads NVS on every
    //      reconnect, and the HTTP handler calls g_snap.disconnect() after
    //      a successful save (which also wakes our blocked recv) so the new
    //      host is picked up within ~1 s.
    xTaskCreatePinnedToCore(
        [](void*) {
          char h[geelen_idf::SNAPCAST_HOST_MAX];
          while (true) {
            if (!g_snap.isConnected()) {
              g_cfg.getSnapcastHost(h, sizeof(h));
              if (h[0] == '\0') {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
              }
              const uint16_t p = g_cfg.getSnapcastPort();
              if (!g_snap.connect(h, p)) {
                ESP_LOGW("geelen",
                         "Snap connect to %s:%u failed, retrying in 2 s", h,
                         (unsigned)p);
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
              }
            }
            g_snap.tick();
          }
        },
        "snap", 4096, nullptr, 5, nullptr, 1);
  }

  // No tone fallback in normal operation. When no active source is configured/running,
  // the device should stay silent (and avoid filling the PCM ring buffer).
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// ESP-IDF entrypoint. Pinouts and DAC control are indirected through the
// compile-time HAL under `src/hal/`, so this file compiles unchanged for
// every board variant; only the `BOARD_*` macro selects the actual pins.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"  // xTaskCreatePinnedToCoreWithCaps
#include "esp_heap_caps.h"           // MALLOC_CAP_SPIRAM
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include <cmath>

#include "audio_engine.h"
#include "idf_config_manager.h"
#include "idf_wifi_manager.h"
#include "idf_http_server.h"
#include "idf_snapclient.h"
#include "esp_timer.h"
#if CONFIG_BT_ENABLED
#include "idf_a2dp_sink.h"
#endif
#include "idf_cpu_sampler.h"
#include "idf_logs.h"
#include "idf_dlna.h"
#include "idf_mp3_streamer.h"
#include "idf_voice_feedback.h"
#include "idf_voice_cache.h"
#include "idf_test_tone.h"
#include "idf_supervisor.h"
#include "idf_state_stream.h"
#include "idf_mode_manager.h"
#include "idf_led.h"
#include "idf_encoder.h"
#include "idf_identity.h"
#include "idf_mdns.h"
#include "hal/hal_board.h"
#include "hal/dac.h"

// Board-specific pinouts come from the HAL. On the Amped-WROVER these are
// identical to the pre-refactor hard-coded values (BCK=26, WS=25, DATA=22,
// AMP_EN=13, I2S_STABLE_MS=50).
static constexpr gpio_num_t PIN_I2S_WS = HAL_PIN_I2S_WS;
static constexpr gpio_num_t PIN_I2S_BCK = HAL_PIN_I2S_BCK;
static constexpr gpio_num_t PIN_I2S_DATA = HAL_PIN_I2S_DATA;
static constexpr uint32_t I2S_STABLE_MS = HAL_I2S_STABLE_MS;

static geelen::AudioEngine g_engine;
static i2s_chan_handle_t g_i2s_tx = nullptr;
static geelen_idf::ConfigManager g_cfg;
static geelen_idf::WifiManager g_wifi;
static geelen_idf::HttpServer g_http;
static geelen_idf::Snapclient g_snap;
#if CONFIG_BT_ENABLED
static geelen_idf::A2dpSink g_a2dp;
#endif

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
// The reboot decision now lives inside `geelen_idf::ModeManager`, dispatched
// via `EV_MODE_CHANGE_REQUESTED` (see idf_mode_manager.cpp). This file only
// needs to expose `scheduleRestart` to other translation units.
namespace geelen_idf { void scheduleRestart(unsigned delay_ms); }

// Pull the latest per-channel volume + mute state out of NVS and push it
// into the audio engine. Invoked by idf_api after POST /api/volume and
// also called from app_main at boot so the engine starts with the saved
// values rather than its 100/100 constructor default.
static void apply_volume_from_cfg() {
  const bool muted = g_cfg.isMuted();
  const int live = geelen_idf::a2dpLiveVolumePct();
  const uint8_t pct = live >= 0 ? (uint8_t)live
                                : (g_cfg.getVolumeL() > g_cfg.getVolumeR() ? g_cfg.getVolumeL()
                                                                          : g_cfg.getVolumeR());
  if (geelen_hal::dacHasHwVolume()) {
    // Louder: TAS 0x4C is the fader. Engine stays at unity so PCM is not
    // faded twice. Mute is TAS 0xFF plus engine mute.
    g_engine.setVolumeLR(100, 100);
    g_engine.setMuted(muted);
    geelen_hal::dacSetVolume(muted ? 0 : pct);
  } else {
    g_engine.setVolumeLR(g_cfg.getVolumeL(), g_cfg.getVolumeR());
    g_engine.setMuted(muted);
    geelen_hal::dacSetVolume(100);
  }
  geelen_idf::a2dpNotifyLocalVolume();
  geelen_idf::ledShowVolume(pct, muted);
  ESP_LOGI("geelen", "volume L=%u R=%u muted=%d pct=%u hw_vol=%d live=%d",
           (unsigned)g_cfg.getVolumeL(), (unsigned)g_cfg.getVolumeR(),
           (int)muted, (unsigned)pct, (int)geelen_hal::dacHasHwVolume(), live);
}
static void apply_channels_from_cfg() {
  g_engine.setChannels(
      g_cfg.getChannelLOn(),
      g_cfg.getChannelROn(),
      g_cfg.getStereoMode() == geelen_idf::StereoMode::Mono);
  ESP_LOGI("geelen", "channels L=%d R=%d mono=%d",
           (int)g_cfg.getChannelLOn(), (int)g_cfg.getChannelROn(),
           (int)(g_cfg.getStereoMode() == geelen_idf::StereoMode::Mono));
  if (!g_cfg.getChannelLOn() || !g_cfg.getChannelROn()) {
    ESP_LOGW("geelen", "a speaker channel is off — Audio page L/R enables");
  }
}

extern "C" void on_volume_changed()   { apply_volume_from_cfg(); }
extern "C" void on_channels_changed() { apply_channels_from_cfg(); }
extern "C" void on_led_changed()      { geelen_idf::ledTakeoverUser(); }
extern "C" void on_encoder_volume_live(uint8_t pct, bool muted) {
  if (geelen_hal::dacHasHwVolume()) {
    g_engine.setVolumeLR(100, 100);
    g_engine.setMuted(muted);
    geelen_hal::dacSetVolume(muted ? 0 : pct);
  } else {
    g_engine.setVolumeLR(pct, pct);
    g_engine.setMuted(muted);
  }
  geelen_idf::ledShowVolume(pct, muted);
  geelen_idf::a2dpNotifyLocalVolume();
}

/** Called from `idf_api.cpp` appendSystemMetrics — forwards to `g_engine`. */
extern "C" void geelen_engine_read_output_meters(float* pl, float* pr, float* rl, float* rr) {
  float a, b, c, d;
  g_engine.readOutputMeters(a, b, c, d);
  if (pl) *pl = a;
  if (pr) *pr = b;
  if (rl) *rl = c;
  if (rr) *rr = d;
}

// Push EQ from NVS into the engine / TAS, then apply headroom.
// Louder: hardware PEQ (independent L/R banks when split). If I2C fails,
// software fallback stays ganged on the left curve — do not run two
// software cascades. Amped: one software cascade on the left/ganged curve.
extern "C" void geelen_eq_set_backend(const char* label);

static float peak_from_cfg() {
  if (!g_cfg.getEqEnabled()) return 0.f;
  geelen_idf::ParametricBand left[geelen_idf::PARAMETRIC_BANDS];
  geelen_idf::ParametricBand right[geelen_idf::PARAMETRIC_BANDS];
  g_cfg.getEqBands(left);
  g_cfg.getEqBandsR(right);
  const uint8_t n = g_cfg.getEqActiveBands();
  const auto* l = reinterpret_cast<const geelen::EqBand*>(left);
  const auto* r = reinterpret_cast<const geelen::EqBand*>(right);
  const float pL = geelen::eqPeakGainDb((float)geelen::SAMPLE_RATE, l, n);
  float pR = pL;
  if (g_cfg.getEqChannelMode() == geelen_idf::EqChannelMode::Split) {
    pR = geelen::eqPeakGainDb((float)geelen::SAMPLE_RATE, r, n);
  }
  return pL > pR ? pL : pR;
}

// In Auto mode the headroom dB equals -peak gain of the louder of L/R so
// the post-EQ signal tops out at ≤ 0 dBFS. Peak is taken from the band
// lists (not the software cascade cache) so Louder HW PEQ still pads PCM
// when the engine cascade is disabled. Manual mode applies the user's cap.
static void apply_eq_from_cfg() {
  geelen_idf::ParametricBand left[geelen_idf::PARAMETRIC_BANDS];
  geelen_idf::ParametricBand right[geelen_idf::PARAMETRIC_BANDS];
  g_cfg.getEqBands(left);
  g_cfg.getEqBandsR(right);
  const uint8_t band_count = g_cfg.getEqActiveBands();
  const bool enabled = g_cfg.getEqEnabled();
  const bool split = (g_cfg.getEqChannelMode() == geelen_idf::EqChannelMode::Split);
  const auto* eq_l = reinterpret_cast<const geelen::EqBand*>(left);
  const auto* eq_r = reinterpret_cast<const geelen::EqBand*>(right);

  bool hw_ok = false;
  if (geelen_hal::dacSupportsHwPeq()) {
    if (enabled) {
      hw_ok = geelen_hal::dacApplyPeqBandsLR(eq_l, band_count, eq_r, band_count, !split);
      if (!hw_ok) {
        ESP_LOGW("geelen",
                 "HW PEQ upload failed; falling back to software cascade (ganged L)");
        geelen_hal::dacSetHwPeqEnabled(false);
      }
    } else {
      geelen_hal::dacSetHwPeqEnabled(false);
    }
  }
  if (hw_ok) {
    g_engine.setEqCurve(eq_l, band_count, /*enabled*/ false);
    g_engine.setEqBackend(geelen::AudioEngine::EqBackend::Hardware);
    geelen_eq_set_backend("hardware");
  } else if (geelen_hal::dacSupportsHwPeq() && enabled) {
    g_engine.setEqCurve(eq_l, band_count, /*enabled*/ true);
    g_engine.setEqBackend(geelen::AudioEngine::EqBackend::Software);
    geelen_eq_set_backend("software (fallback)");
  } else {
    g_engine.setEqCurve(eq_l, band_count, enabled);
    g_engine.setEqBackend(geelen::AudioEngine::EqBackend::Software);
    geelen_eq_set_backend("software");
  }

  float db = g_cfg.getHeadroomManualDb();
  if (g_cfg.getHeadroomMode() == geelen_idf::HeadroomMode::Auto) {
    const float peak = peak_from_cfg();
    db = peak > 0.f ? -peak : 0.f;
  }
  g_engine.setHeadroomDb(db);
}

extern "C" void on_eq_changed() { apply_eq_from_cfg(); }

extern "C" float compute_eq_peak_db() { return peak_from_cfg(); }

static bool init_i2s() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  // Increase DMA buffering to tolerate bursty A2DP delivery + WiFi/BT coexist jitter.
  // Defaults (6 * 240 frames) are only ~33ms of audio at 44.1kHz stereo 16-bit.
  // Keep this moderate so BT controller can still allocate internal RAM.
  // (~185ms: 16 * 512 frames). If `i2s_channel_write` often returns ESP_ERR_TIMEOUT (263)
  // with written=0, try raising dma_desc_num / dma_frame_num (RAM tradeoff) or check
  // Wi‑Fi/BT coexistence and core pinning — see audio_task vs other high-priority work.
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
  // Force APLL for accurate audio clocks on ESP32. If the I2S clock runs slow, audio
  // pitch drops and the ring buffer will overflow (producer faster than consumer).
  // The S3 does not expose an APLL for I2S (only PLL_F160M/XTAL); use the default
  // clock source there — the S3 PLL is already tight enough for the ring drift math
  // the AudioEngine does.
#if SOC_I2S_SUPPORTS_APLL
  clk_cfg.clk_src = I2S_CLK_SRC_APLL;
#endif
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
  // Avoid power-on pops: bring the amp up only after the I2S clock is stable.
  // The DAC HAL owns the actual GPIO / TAS5825M state machine.
  vTaskDelay(pdMS_TO_TICKS(I2S_STABLE_MS));
  geelen_hal::dacAmpEnable(true);
}

/** Test tone shares the engine ring with voice prompts; start it only after boot clips finish. */
static void test_tone_after_voice_task(void*) {
  geelen_idf::voiceFeedbackWaitForBootDone(portMAX_DELAY);
  geelen_idf::testToneStart(&g_engine, &g_cfg);
  vTaskDelete(nullptr);
}

// APSTA onboarding keeps both Wi-Fi stacks resident; starting Bluedroid
// immediately can exhaust the BT OSI timer pool (osi_alarm_new err 0x101).
// We wait out the worst of boot/Wi-Fi churn, then start A2DP so users who
// never fix Wi-Fi can still pair for audio.
#if CONFIG_BT_ENABLED
static constexpr int kBtDeferPollMs = 500;
// 6 * 500 ms = 3 s settle after AP is up. Do not start Bluedroid in the
// same tick as the AP (osi_alarm OOM); do not wait a full minute either.
static constexpr int kBtDeferMaxPolls = 6;

static const char* a2dp_name_from_cfg() {
  static char name[geelen_idf::DEV_NAME_MAX];
  g_cfg.getDeviceName(name, sizeof(name));
  return name[0] ? name : "Geelen Audio";
}

static void deferred_a2dp_start_task(void*) {
  for (int i = 0; i < kBtDeferMaxPolls && g_wifi.isApOnly(); ++i) {
    vTaskDelay(pdMS_TO_TICKS(kBtDeferPollMs));
  }
  if (!g_wifi.isApOnly()) {
    ESP_LOGI("geelen", "Wi-Fi left AP-only mode; starting Bluetooth A2DP");
  } else {
    ESP_LOGW(
        "geelen",
        "Wi-Fi still in captive AP after %u s — starting Bluetooth anyway "
        "(pair for audio without fixing Wi-Fi)",
        (unsigned)((kBtDeferMaxPolls * kBtDeferPollMs) / 1000));
  }
  a2dpSetConfig(&g_cfg);
  g_a2dp.begin(a2dp_name_from_cfg());
  vTaskDelete(nullptr);
}
#endif  // CONFIG_BT_ENABLED

static void audio_task(void*) {
  // Steady playback: process() blocks in i2s_channel_write (portMAX_DELAY) —
  // DMA is the pacer; we must not vTaskDelay(1) on every iteration or we'd cap
  // drain rate and starve the ring (Snap/A2DP would see overflows / Pkt dropped).
  //
  // Prefill / rebuffer / empty-ring paths return false *without* touching I2S.
  // A tight loop there would peg Core 1 at 100 % and starve BT stack tasks
  // (symptom: BT_APPL "Pkt dropped", no audible voice prompts). Yield one tick
  // only on those early returns so producers and the controller can run.
  for (;;) {
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
  // The "httpd" tag fires `E httpd_accept_conn: error in accept (113)` +
  // `W httpd_server: error accepting new connection` whenever a browser
  // aborts a TCP connection between select() readiness and accept() — i.e.
  // standard LAN behaviour during page reloads or SPA retry storms. Both
  // are cosmetic (errno 113 = ERR_RTE = peer already gone). We silence the
  // tag entirely; httpd_start failures and per-route errors are surfaced
  // via our own "geelen" logging in idf_http_server.cpp / idf_api.cpp.
  esp_log_level_set("httpd", ESP_LOG_NONE);
  esp_log_level_set("httpd_uri", ESP_LOG_WARN);
  esp_log_level_set("httpd_txrx", ESP_LOG_WARN);
  esp_log_level_set("httpd_parse", ESP_LOG_WARN);
  esp_log_level_set("httpd_sess", ESP_LOG_WARN);
  esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
  esp_log_level_set("event", ESP_LOG_WARN);
}

// PSRAM-backed cJSON allocators. Every /api/* JSON handler builds a cJSON
// tree (~1-3 KiB of small alloc bursts) and immediately serializes it to a
// string (another 0.5-2 KiB). Doing this from internal DRAM directly
// competes with WiFi RX malloc and BT controller pbufs — measured: a single
// EQ-page asset burst (parallel /api/state + /api/eq/presets +
// /api/state/stream initial frame) tipped DRAM into the `wifi:m f null` /
// `BT_APPL: Pkt dropped` cascade with audio underruns. PSRAM has 2 MiB
// free at all times so steering cJSON there is essentially free.
//
// Safety: heap_caps_malloc(MALLOC_CAP_SPIRAM) is thread-safe and reentrant
// (internal mutex), and httpd_resp_send_chunk copies caller data into
// DMA-capable pbufs — it never DMAs from PSRAM directly. cJSON is never
// invoked from ISR context in this codebase.
static void* cjson_malloc_psram(size_t sz) {
  return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
}
static void cjson_free_psram(void* p) {
  heap_caps_free(p);
}

extern "C" void app_main(void) {
  configure_uart_log_levels();
  // Hook the logger *before* the first INFO so the live-stream ring captures
  // the full boot log, including NVS / Wi-Fi banner lines.
  geelen_idf::logsInit();
  // Steer cJSON allocations onto PSRAM before any handler runs (also before
  // ConfigManager's NVS hydration, which builds intermediate JSON for some
  // legacy fields). Hooks are global state in cJSON's translation unit;
  // setting them once here applies to every cJSON_* call thereafter.
  {
    cJSON_Hooks hooks{};
    hooks.malloc_fn = &cjson_malloc_psram;
    hooks.free_fn = &cjson_free_psram;
    cJSON_InitHooks(&hooks);
  }
  ESP_LOGI("geelen", "Geelen Audio Amp – ESP-IDF env boot");
  // Bring up the control-plane event loop early so any subsystem that
  // wants to post an event during init has somewhere to post it. Phase 0
  // only logs events; Phases 1–4 migrate Wi-Fi / voice / mode / state-diff
  // onto this bus. Failures are non-fatal — existing orchestration paths
  // still drive the firmware.
  geelen_idf::supervisorBegin();
  g_cfg.begin();
  geelen_idf::identityInit(&g_cfg);
  // Bring up the Mode Manager once the config is loaded so its log line
  // shows the persisted source. No producers are wired through it yet
  // (Phase 3 ships the FSM scaffold; later phases plug in concrete
  // `IProducer` implementations for snap / test-tone / DLNA-stream).
  geelen_idf::modeManager().begin(&g_cfg);
  geelen_idf::ledBegin(&g_cfg);
  geelen_idf::encoderBegin(&g_cfg);
  // SPIFFS is mounted by `g_cfg.begin()` — decode every voice prompt into
  // PSRAM right after, before any subsystem can ask for one. This replaces
  // the per-prompt Helix decode + streamer-singleton borrow with a small
  // read-only PCM table; see idf_voice_cache.h for the rationale.
  geelen_idf::voiceCacheInit();
  geelen_idf::configStartDebouncedSaveTask(&g_cfg);
  // Start the CPU sampler early so its calibration baseline has seconds of
  // mostly-idle time to settle before WiFi/BT/audio kick in.
  geelen_idf::cpuSamplerStart();
  geelen_idf::mdnsBegin(&g_cfg);
  g_wifi.begin(&g_cfg);
  geelen_idf::identitySetWifi(&g_wifi);
  // Bring up the SSE broadcaster before httpd so the URI handler installed
  // by `g_http.begin` sees an initialized broadcaster and never has to
  // 503 a connecting SPA tab during the boot window.
  geelen_idf::stateStreamInit(&g_cfg);
  g_http.begin(&g_cfg, &g_wifi);

#if CONFIG_BT_ENABLED
  g_a2dp.setEngine(&g_engine);
#endif

  // Bring the DAC HAL up before the AudioEngine so any hardware PEQ backend
  // is available when `apply_eq_from_cfg` runs. On Amped this is a no-op that
  // just configures the amp-enable GPIO; on Louder it initialises I2C and the
  // TAS5825M and leaves the DAC in HI-Z until `amp_enable_after_i2s_stable`.
  if (!geelen_hal::dacInit()) {
    ESP_LOGW("geelen", "DAC HAL init failed — continuing with software audio path");
  }
  ESP_LOGI("geelen", "Board: %s (EQ max %u bands, HW PEQ %s)",
           HAL_BOARD_NAME,
           (unsigned)HAL_EQ_MAX_BANDS_ACTIVE,
           geelen_hal::dacSupportsHwPeq() ? "yes" : "no");

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
  // Chip is in PLAY with host BQ RAM. Re-apply volume onto TAS 0x4C and
  // move EQ onto hardware when the DAC reports it is ready.
  apply_volume_from_cfg();
  apply_eq_from_cfg();
  geelen_hal::dacLogStatus();

  geelen_idf::mp3::streamer()->setEngine(&g_engine);
  geelen_idf::voiceFeedbackSetContext(&g_cfg, &g_wifi, &g_engine);
  // Schedules the boot prompts and spawns the voice task. PCM comes
  // straight from `voiceCacheInit()` — the prior 3.5 s "boot settle" wait
  // existed only to let the Helix decoder + MP3 streamer race finish
  // before the first prompt; with the cache there is nothing to race.
  geelen_idf::voiceFeedbackScheduleBoot();

  const geelen_idf::AudioSource src = g_cfg.getAudioSource();
  ESP_LOGI("geelen", "Selected source: %u", (unsigned)src);

  // Native DLNA renderer. SSDP advertising is always on when the user
  // enables DLNA in the config, regardless of the active source: this
  // way casting apps can "wake" the device by issuing Play.
  // TODO(phase9b): gate SSDP on WiFi IP acquisition event rather than
  // waiting here — on cold boot without saved credentials this runs
  // before netif has an IP and send_alive_burst() then spews a useless
  // 0.0.0.0 LOCATION until the next announce interval.
  if (g_cfg.getDlnaEnabled() && src != geelen_idf::AudioSource::TestTone) {
    geelen_idf::dlna::start((httpd_handle_t)g_http.nativeHandle(), &g_cfg);
  }

  // Stack sized for: process() locals (process_buf 512 B, work[] 1 KB) +
  // ESP_LOGI vsnprintf 256 B + i2s_channel_write internal frames + FreeRTOS
  // overhead. 4 KB was the historical value but left zero margin once voice
  // prompts started feeding the engine; 6 KB adds headroom without measurable
  // RAM cost. (The IirDelaySnapshot rollback that used to cost ~1.5 KB is gone.)
  // NOTE: stack stays in internal DRAM. PSRAM stack was attempted to free
  // ~6 KiB DRAM but the runtime heap actually got *more* fragmented (the
  // saved DRAM was eaten back by BT host + LwIP small allocs), so the move
  // didn't translate into useful headroom. Audio task perf is also more
  // sensitive to stack-access latency than other tasks; keep it internal.
  xTaskCreatePinnedToCore(audio_task, "audio", 6144, nullptr, 20, nullptr, 1);

  // Start BT Classic A2DP sink only when BT is selected. Defer if we are
  // still in captive APSTA onboarding — see deferred_a2dp_start_task.
#if CONFIG_BT_ENABLED
  if (src == geelen_idf::AudioSource::Bluetooth) {
    geelen_idf::a2dpSetConfig(&g_cfg);
    if (!g_wifi.isApOnly()) {
      g_a2dp.begin(a2dp_name_from_cfg());
    } else {
      ESP_LOGW("geelen",
               "AP-only Wi-Fi onboarding: deferring Bluetooth ~%u s (or until STA "
               "gets an IP) to avoid Bluedroid init OOM; then A2DP starts anyway",
               (unsigned)((kBtDeferMaxPolls * kBtDeferPollMs) / 1000));
      xTaskCreate(deferred_a2dp_start_task, "bt_defer", 4096, nullptr, 5, nullptr);
    }
  }
#else
  if (src == geelen_idf::AudioSource::Bluetooth) {
    ESP_LOGW("geelen", "Bluetooth source selected but BT is disabled in sdkconfig — ignoring");
  }
#endif

  // (Historical) Phase 2 made the eager `mp3::streamer()->ensureTaskNow()`
  // here obsolete: voice prompts no longer go through the MP3 streamer
  // (they're PCM-cached at boot — see idf_voice_cache), so the only
  // remaining caller is DLNA SOAP `Play`, which is initiated by an
  // external UPnP request and can afford the lazy 20 KiB allocation.
  //
  // Skipping the eager spawn frees the dlna-mp3 task's 8 KiB stack +
  // 12.5 KiB Helix in/out buffers (= 20 KiB internal DRAM) for the
  // entire lifetime of the boot when DLNA isn't used. That headroom
  // is what keeps WiFi RX malloc and httpd workers happy under
  // sustained BT playback (without it, runtime DRAM steady-states at
  // ~8 KiB free / ~3.5 KiB largest, and any TCP burst to httpd RSTs
  // mid-handler with `wifi:m f null` warnings).
  //
  // The task is now created on demand inside `mp3::Streamer::playFile`
  // via `ensureTaskNow()` — same call path, just deferred until first
  // DLNA Play. See idf_mp3_streamer.cpp for the lazy-spawn site.

  char host[geelen_idf::SNAPCAST_HOST_MAX] = {0};
  g_cfg.getSnapcastHost(host, sizeof(host));
  const bool snap_on = host[0] != '\0';

  if (src == geelen_idf::AudioSource::TestTone) {
    BaseType_t ok = xTaskCreatePinnedToCore(test_tone_after_voice_task, "tt-after-voice", 3072, nullptr,
                                            4, nullptr, 1);
    if (ok != pdPASS) {
      ESP_LOGW("geelen", "tt-after-voice task failed; starting test tone without voice wait");
      geelen_idf::testToneStart(&g_engine, &g_cfg);
    }
  } else if (src == geelen_idf::AudioSource::Network) {
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

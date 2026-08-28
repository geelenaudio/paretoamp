// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// ESP-IDF Bluetooth A2DP sink implementation. Whole file collapses to empty
// when BT Classic is disabled (S3 targets); see idf_a2dp_sink.h.

#include "sdkconfig.h"

#if CONFIG_BT_ENABLED

#include "idf_a2dp_sink.h"

#include "audio_engine.h"
#include "idf_config_manager.h"
#include "idf_events.h"
#include "idf_state_stream.h"
#include "idf_voice_feedback.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#if CONFIG_BT_AVRCP_ENABLED
#include "esp_avrc_api.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"

#include <atomic>
#include <vector>
#include <cstring>

// Bluedroid internal: force ACL out of sniff/park into active. QoS alone
// does not cancel sniff; HFP uses this same entry when a SCO comes up.
extern "C" void bta_dm_pm_active(uint8_t peer_addr[6]);

namespace geelen_idf {

static const char* TAG = "A2DP";
geelen::AudioEngine* A2dpSink::engine_ = nullptr;
static uint32_t s_bt_pcm_hz = 44100;
static uint32_t s_resample_step_q16 = 0;  // in_rate/out_rate in Q16.16
static bool s_have_carry_frame = false;
static int16_t s_carry_frame[2] = {0, 0};  // stereo
static uint32_t s_pos_q16 = 0;
static uint32_t s_step_adj_q16 = 0;  // multiplicative adjustment (Q16.16), around 1.0
static StreamBufferHandle_t s_pcm_sb = nullptr;
static std::atomic<bool> s_pcm_flush{false};
static constexpr size_t kPcmSbBytes = 32768;  // ~170 ms at 48 kHz stereo 16-bit (PSRAM)
static std::atomic<bool> s_a2dp_playing{false};
static bool s_a2dp_up = false;
static ConfigManager* s_cfg = nullptr;
static std::atomic<bool> s_connected{false};
static std::atomic<bool> s_discoverable{false};
static int64_t s_window_deadline_us = 0;
static esp_timer_handle_t s_pair_timer = nullptr;
static constexpr int64_t kPairingWindowUs = 180000000;  // 180 s
static esp_bd_addr_t s_peer_bda{};
static bool s_peer_valid = false;
#if CONFIG_BT_AVRCP_ENABLED
static std::atomic<bool> s_avrcp_vol_ntf{false};
static std::atomic<bool> s_avrcp_from_phone{false};
// Latest phone volume (0–100). -1 = none pending. BTC only stores; pump applies.
static std::atomic<int> s_phone_vol_pct{-1};
// UI/engine ganged volume. Once set, never cleared to -1: persist must not
// throw the SPA back at a stale NVS value while the phone/engine have moved on.
static std::atomic<int> s_live_vol_pct{-1};
static std::atomic<int> s_vol_persist{-1};
static std::atomic<int64_t> s_vol_persist_deadline_us{0};
static std::atomic<int> s_last_avrcp_abs{-1};
static constexpr int64_t kVolPersistUs = 250000;
#endif

bool a2dpPlaybackActive() {
  return s_a2dp_playing.load(std::memory_order_relaxed);
}

int a2dpLiveVolumePct() {
#if CONFIG_BT_AVRCP_ENABLED
  return s_live_vol_pct.load(std::memory_order_relaxed);
#else
  return -1;
#endif
}

bool a2dpIsUp() { return s_a2dp_up; }
bool a2dpIsConnected() { return s_connected.load(std::memory_order_relaxed); }
bool a2dpIsDiscoverable() { return s_discoverable.load(std::memory_order_relaxed); }

void a2dpSetConfig(ConfigManager* cfg) { s_cfg = cfg; }

// Bluedroid PM sniffs the ACL (~400–800 slots, ~0.5 s) a few seconds after
// A2DP connect while audio is idle. Sniff stalls SBC, the engine underruns,
// and Core 0 sits in BT/Wi-Fi recovery so the SPA stops loading pages.
// `esp_bt_gap_set_qos` does not exit sniff; `bta_dm_pm_active` does.
static void keep_acl_active() {
  if (!s_peer_valid) {
    ESP_LOGW(TAG, "keep_acl_active: no peer BDA");
    return;
  }
  bta_dm_pm_active(s_peer_bda);
  esp_err_t err = esp_bt_gap_set_qos(s_peer_bda, 40);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_bt_gap_set_qos failed: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "ACL active requested (exit sniff + QoS 25 ms)");
  }
}

static uint8_t volume_pct_from_cfg() {
  if (!s_cfg) return 0;
  const uint8_t l = s_cfg->getVolumeL();
  const uint8_t r = s_cfg->getVolumeR();
  return l > r ? l : r;
}

static uint8_t volume_pct_reported() {
#if CONFIG_BT_AVRCP_ENABLED
  const int live = s_live_vol_pct.load(std::memory_order_relaxed);
  if (live >= 0) return static_cast<uint8_t>(live);
#endif
  return volume_pct_from_cfg();
}

static void apply_gap_pin(const char* pin) {
  if (!pin || strlen(pin) != 4) pin = "1234";
  esp_bt_pin_code_t pin_code{};
  pin_code[0] = static_cast<uint8_t>(pin[0]);
  pin_code[1] = static_cast<uint8_t>(pin[1]);
  pin_code[2] = static_cast<uint8_t>(pin[2]);
  pin_code[3] = static_cast<uint8_t>(pin[3]);
  esp_err_t err = esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin_code);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_bt_gap_set_pin failed: %s", esp_err_to_name(err));
  }
}

static void wipe_bonds_now() {
  int n = esp_bt_gap_get_bond_device_num();
  if (n <= 0) {
    ESP_LOGI(TAG, "bond wipe: no peers");
    return;
  }
  if (n > 16) n = 16;
  esp_bd_addr_t list[16]{};
  int got = n;
  if (esp_bt_gap_get_bond_device_list(&got, list) != ESP_OK) {
    ESP_LOGW(TAG, "esp_bt_gap_get_bond_device_list failed");
    return;
  }
  for (int i = 0; i < got; ++i) {
    esp_err_t e = esp_bt_gap_remove_bond_device(list[i]);
    ESP_LOGI(TAG, "removed bond %02x:%02x:%02x:%02x:%02x:%02x (%s)",
             list[i][0], list[i][1], list[i][2], list[i][3], list[i][4], list[i][5],
             esp_err_to_name(e));
  }
}

void a2dpWipeBonds() {
  if (!s_a2dp_up) return;
  wipe_bonds_now();
}

bool a2dpApplyPin(const char* pin) {
  if (!s_a2dp_up) return false;
  apply_gap_pin(pin);
  return true;
}

static void close_pairing_window() {
  s_discoverable.store(false, std::memory_order_relaxed);
  s_window_deadline_us = 0;
  if (s_pair_timer) esp_timer_stop(s_pair_timer);
  if (s_a2dp_up) {
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
  }
  stateStreamMarkDirty();
}

static void pairing_timer_cb(void*) {
  ESP_LOGI(TAG, "pairing window closed");
  close_pairing_window();
}

static void ensure_pair_timer() {
  if (s_pair_timer) return;
  const esp_timer_create_args_t args = {
      .callback = &pairing_timer_cb,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "bt_pair",
      .skip_unhandled_events = true,
  };
  ESP_ERROR_CHECK(esp_timer_create(&args, &s_pair_timer));
}

static bool open_pairing_window() {
  if (!s_a2dp_up) return false;
  if (s_connected.load(std::memory_order_relaxed)) {
    ESP_LOGI(TAG, "pairing window skipped (already connected, ACL=1)");
    return true;
  }
  ensure_pair_timer();
  if (s_pair_timer) esp_timer_stop(s_pair_timer);
  esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
  s_discoverable.store(true, std::memory_order_relaxed);
  s_window_deadline_us = esp_timer_get_time() + kPairingWindowUs;
  if (s_pair_timer) {
    esp_timer_start_once(s_pair_timer, static_cast<uint64_t>(kPairingWindowUs));
  }
  stateStreamMarkDirty();
  ESP_LOGI(TAG, "pairing window open for 180 s");
  return true;
}

bool a2dpStartPairingWindow() { return open_pairing_window(); }

uint32_t a2dpPairingWindowMsLeft() {
  if (!s_discoverable.load(std::memory_order_relaxed)) return 0;
  const int64_t left = s_window_deadline_us - esp_timer_get_time();
  if (left <= 0) return 0;
  return static_cast<uint32_t>(left / 1000);
}

#if CONFIG_BT_AVRCP_ENABLED
static uint8_t vol100_to_avrcp(uint8_t pct) {
  if (pct > 100) pct = 100;
  return static_cast<uint8_t>((static_cast<unsigned>(pct) * 127u + 50u) / 100u);
}

static uint8_t avrcp_to_vol100(uint8_t v) {
  if (v > 127) v = 127;
  return static_cast<uint8_t>((static_cast<unsigned>(v) * 100u + 63u) / 127u);
}

void a2dpNotifyLocalVolume() {
  if (!s_a2dp_up) return;
  if (s_avrcp_from_phone.load(std::memory_order_relaxed)) return;
  // SPA/API is the new authority. Cancel a pending phone persist first so
  // it cannot write an older percent over this cfg value.
  s_vol_persist.store(-1, std::memory_order_relaxed);
  s_phone_vol_pct.store(-1, std::memory_order_relaxed);
  const uint8_t pct = volume_pct_from_cfg();
  s_live_vol_pct.store((int)pct, std::memory_order_relaxed);
  const int abs = (int)vol100_to_avrcp(pct);
  if (s_last_avrcp_abs.load(std::memory_order_relaxed) == abs) return;
  if (!s_avrcp_vol_ntf.load(std::memory_order_relaxed)) return;
  s_avrcp_vol_ntf.store(false, std::memory_order_relaxed);
  s_last_avrcp_abs.store(abs, std::memory_order_relaxed);
  esp_avrc_rn_param_t p{};
  p.volume = static_cast<uint8_t>(abs);
  esp_err_t err = esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &p);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "avrc volume CHANGED failed: %s", esp_err_to_name(err));
  }
}

static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t* param) {
  switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
      if (param) {
        ESP_LOGI(TAG, "AVRC TG %s", param->conn_stat.connected ? "connected" : "disconnected");
        if (!param->conn_stat.connected) {
          s_avrcp_vol_ntf.store(false, std::memory_order_relaxed);
        }
      }
      break;
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
      if (!param) break;
      {
        // Stash only. setVolume + SSE + logging here ran on BTC and
        // stalled SBC decode, which filled the PCM StreamBuffer and
        // dropped odd byte counts (stereo desync / clicks).
        const uint8_t abs = param->set_abs_vol.volume;
        if (s_last_avrcp_abs.load(std::memory_order_relaxed) == (int)abs) {
          break;  // echo of our CHANGED/INTERIM — do not fight the phone
        }
        s_last_avrcp_abs.store((int)abs, std::memory_order_relaxed);
        const uint8_t pct = avrcp_to_vol100(abs);
        s_phone_vol_pct.store((int)pct, std::memory_order_relaxed);
        s_live_vol_pct.store((int)pct, std::memory_order_relaxed);
      }
      break;
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
      if (param && param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
        s_avrcp_vol_ntf.store(true, std::memory_order_relaxed);
        const uint8_t abs = vol100_to_avrcp(volume_pct_reported());
        s_last_avrcp_abs.store((int)abs, std::memory_order_relaxed);
        esp_avrc_rn_param_t p{};
        p.volume = abs;
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &p);
      }
      break;
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
      // Volume only — ignore play/pause/next.
      break;
    default:
      break;
  }
}
#else
void a2dpNotifyLocalVolume() {}
#endif

static void reset_resample_state_() {
  s_pos_q16 = 0;
  s_have_carry_frame = false;
}

static void drain_pcm_sb_() {
  if (!s_pcm_sb) return;
  uint8_t dump[256];
  while (xStreamBufferReceive(s_pcm_sb, dump, sizeof(dump), 0) > 0) {
  }
}

#if CONFIG_BT_AVRCP_ENABLED
// Engine gain immediately; NVS/SSE only after the rocker settles. Called
// from a2dp_pump so BTC never does control-plane work in the audio path.
static void apply_deferred_volume_(geelen::AudioEngine* engine) {
  const int v = s_phone_vol_pct.exchange(-1, std::memory_order_relaxed);
  if (v >= 0 && engine) {
    engine->setVolumeLR(static_cast<uint8_t>(v), static_cast<uint8_t>(v));
    s_vol_persist.store(v, std::memory_order_relaxed);
    s_vol_persist_deadline_us.store(esp_timer_get_time() + kVolPersistUs,
                                    std::memory_order_relaxed);
  }
  int token = s_vol_persist.load(std::memory_order_relaxed);
  if (token < 0) return;
  if (esp_timer_get_time() < s_vol_persist_deadline_us.load(std::memory_order_relaxed)) {
    return;
  }
  if (!s_vol_persist.compare_exchange_strong(token, -1, std::memory_order_relaxed)) {
    return;
  }
  // Persist whatever the UI is showing now, not the stale token — a newer
  // phone step may have landed after this token was scheduled.
  int want = s_live_vol_pct.load(std::memory_order_relaxed);
  if (want < 0) want = token;
  const uint8_t pct = static_cast<uint8_t>(want);
  if (s_cfg && (s_cfg->getVolumeL() != pct || s_cfg->getVolumeR() != pct)) {
    s_avrcp_from_phone.store(true, std::memory_order_relaxed);
    s_cfg->setVolume(pct);
    s_avrcp_from_phone.store(false, std::memory_order_relaxed);
  }
  ESP_LOGI(TAG, "AVRC volume settled %u%%", (unsigned)pct);
}
#else
static void apply_deferred_volume_(geelen::AudioEngine*) {}
#endif

static uint32_t sbcSampleRateFromMask(uint8_t mask) {
  // Prefer higher rates if multiple bits are set.
  if (mask & ESP_A2D_SBC_CIE_SF_48K) return 48000;
  if (mask & ESP_A2D_SBC_CIE_SF_44K) return 44100;
  if (mask & ESP_A2D_SBC_CIE_SF_32K) return 32000;
  if (mask & ESP_A2D_SBC_CIE_SF_16K) return 16000;
  return 44100;
}

static void btAppGapCb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
  if (event == ESP_BT_GAP_AUTH_CMPL_EVT) {
    ESP_LOGI(TAG, "GAP auth complete");
  } else if (event == ESP_BT_GAP_PIN_REQ_EVT) {
    // Post-only: the supervisor handles pairing-clip enqueue + debounce so
    // BT host-task callbacks stay decoupled from the voice serializer.
    eventPost(EV_A2DP_PAIRING, nullptr, 0);
    if (param) {
      char pin[5] = "1234";
      if (s_cfg) s_cfg->getBtPin(pin, sizeof(pin));
      if (param->pin_req.min_16_digit) {
        ESP_LOGW(TAG, "16-digit PIN requested; declining (we only store 4 digits)");
        esp_bt_pin_code_t empty{};
        esp_bt_gap_pin_reply(param->pin_req.bda, false, 0, empty);
      } else {
        esp_bt_pin_code_t code{};
        memcpy(code, pin, 4);
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, code);
      }
    }
  } else if (event == ESP_BT_GAP_MODE_CHG_EVT && param) {
    ESP_LOGI(TAG, "GAP PM mode=%u interval=%u slots", (unsigned)param->mode_chg.mode,
             (unsigned)param->mode_chg.interval);
    if (param->mode_chg.mode == ESP_BT_PM_MD_SNIFF &&
        s_connected.load(std::memory_order_relaxed)) {
      keep_acl_active();
    }
  }
}

bool A2dpSink::begin(const char* device_name) {
  if (!device_name) device_name = "Geelen Audio";

  // Bluedroid's host-side init (gatt_init / btu_init) allocates ~210 B
  // queues + several KB of working memory from the BT-OSI heap, which is
  // backed by the internal-DRAM `MALLOC_CAP_INTERNAL` pool. On this build
  // (Wi-Fi up, A2DP, no BLE released) that pool is genuinely tight — the
  // observed failure mode when it dips too low is a NULL `fixed_queue`
  // returned silently by `osi_malloc` and a panic at the first enqueue
  // (`assert fixed_queue_enqueue (queue != NULL)`). Log free + largest
  // contiguous block here so future regressions are diagnosable from
  // the boot UART log without rebuilding for heap_caps tracing.
  const size_t free_int =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t lblk_int =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
  ESP_LOGI(TAG, "Bluedroid pre-init heap: int8=%u B free, %u B largest; dma=%u B free",
           (unsigned)free_int, (unsigned)lblk_int, (unsigned)free_dma);

  esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
  if (err != ESP_OK) {
    // Not fatal if already in use; log only.
    ESP_LOGW(TAG, "bt_controller_mem_release(BLE): %s", esp_err_to_name(err));
  }

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
  ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

  // Phase 5 NOTE: an earlier iteration called
  // `esp_coex_preference_set(ESP_COEX_PREFER_BT)` between
  // `esp_bt_controller_enable` and `esp_bluedroid_init`. On IDF 5.5 with
  // Classic-BT-only (BLE memory released above), that runtime call left
  // Bluedroid in a state where `gatt_init`'s host-side queue setup hit
  // a NULL `fixed_queue` and the device panicked on boot. The runtime
  // function is also deprecated on recent IDFs in favour of sdkconfig
  // tuning. Coex bias should now be configured via:
  //   CONFIG_ESP_COEX_SW_COEXIST_PREFER_BT (or _BALANCE / _WIFI)
  // in sdkconfig.amped-esp32-idf if we want to bias the arbiter.

  ESP_ERROR_CHECK(esp_bluedroid_init());
  ESP_ERROR_CHECK(esp_bluedroid_enable());

  ESP_ERROR_CHECK(esp_bt_gap_register_callback(btAppGapCb));
  ESP_ERROR_CHECK(esp_bt_gap_set_device_name(device_name));

  if (s_cfg && s_cfg->consumeBtWipeBonds()) {
    wipe_bonds_now();
  }

  char pin[5] = "1234";
  if (s_cfg) s_cfg->getBtPin(pin, sizeof(pin));
  apply_gap_pin(pin);

#if CONFIG_BT_AVRCP_ENABLED
  ESP_ERROR_CHECK(esp_avrc_tg_register_callback(&avrc_tg_cb));
  ESP_ERROR_CHECK(esp_avrc_tg_init());
  {
    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set,
                                       ESP_AVRC_RN_VOLUME_CHANGE);
    ESP_ERROR_CHECK(esp_avrc_tg_set_rn_evt_cap(&evt_set));
  }
#endif

  ESP_ERROR_CHECK(esp_a2d_register_callback(&A2dpSink::a2dpCb_));
  ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(&A2dpSink::a2dpDataCb_));
  ESP_ERROR_CHECK(esp_a2d_sink_init());

  // Both the PCM bounce buffer and the pump task stack live in PSRAM.
  // Putting them in internal DRAM (the IDF default) left ~3.7 KiB free
  // with a 736-byte largest block after Bluedroid init — too small for
  // lwip pbufs, so the SPA at :80 hung after boot. The data callback is
  // not an ISR, so PSRAM copies are safe.
  if (!s_pcm_sb) {
    s_pcm_sb = xStreamBufferCreateWithCaps(kPcmSbBytes, 4, MALLOC_CAP_SPIRAM);
    if (!s_pcm_sb) {
      ESP_LOGE(TAG, "a2dp pcm StreamBuffer create failed");
      return false;
    }
  }
  if (xTaskCreatePinnedToCoreWithCaps(&A2dpSink::pumpTask_, "a2dp_pump", 4096,
                                      nullptr, 12, nullptr, 1,
                                      MALLOC_CAP_SPIRAM) != pdPASS) {
    ESP_LOGE(TAG, "a2dp_pump task create failed");
    return false;
  }

  const size_t free_after =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t lblk_after =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  ESP_LOGI(TAG, "A2DP sink started as '%s' (post-init int8=%u B free, %u B largest)",
           device_name, (unsigned)free_after, (unsigned)lblk_after);
  s_a2dp_up = true;
#if CONFIG_BT_AVRCP_ENABLED
  s_live_vol_pct.store((int)volume_pct_from_cfg(), std::memory_order_relaxed);
#endif
  open_pairing_window();
  return true;
}

void A2dpSink::setAdvertisedName(const char* device_name) {
  a2dpSetAdvertisedName(device_name);
}

void a2dpSetAdvertisedName(const char* name) {
  if (!s_a2dp_up || !name || !name[0]) return;
  esp_err_t err = esp_bt_gap_set_device_name(name);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_bt_gap_set_device_name(%s) failed: %s", name,
             esp_err_to_name(err));
    return;
  }
  ESP_LOGI(TAG, "GAP name %s", name);
}

void A2dpSink::a2dpCb_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param) {
  switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
      if (param) {
        ESP_LOGI(TAG, "Conn state: %d", (int)param->conn_stat.state);
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
          memcpy(s_peer_bda, param->conn_stat.remote_bda, sizeof(s_peer_bda));
          s_peer_valid = true;
          s_connected.store(true, std::memory_order_relaxed);
          close_pairing_window();
          keep_acl_active();
          eventPost(EV_A2DP_CONNECTED, nullptr, 0);
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
          s_connected.store(false, std::memory_order_relaxed);
          s_peer_valid = false;
          s_pcm_flush.store(true, std::memory_order_relaxed);
          eventPost(EV_A2DP_DISCONNECTED, nullptr, 0);
          open_pairing_window();
        }
      }
      break;
    case ESP_A2D_AUDIO_STATE_EVT:
      if (param) {
        ESP_LOGI(TAG, "Audio state: %d", (int)param->audio_stat.state);
        s_a2dp_playing.store(param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED,
                             std::memory_order_relaxed);
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
          keep_acl_active();
        } else {
          // STOPPED / remote suspend: drop leftover PCM so the next track
          // cannot start mid-frame. Do not flush on STARTED — first packets
          // may already be in the StreamBuffer.
          s_pcm_flush.store(true, std::memory_order_relaxed);
        }
      }
      break;
    case ESP_A2D_AUDIO_CFG_EVT:
      if (param) {
        if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
          const uint32_t hz = sbcSampleRateFromMask(param->audio_cfg.mcc.cie.sbc_info.samp_freq);
          s_bt_pcm_hz = hz;
          s_resample_step_q16 = (uint32_t)(((uint64_t)s_bt_pcm_hz << 16) / (uint64_t)geelen::SAMPLE_RATE);
          s_pcm_flush.store(true, std::memory_order_relaxed);
          ESP_LOGI(TAG, "SBC PCM rate: %u Hz (resample -> %u Hz)", (unsigned)s_bt_pcm_hz,
                   (unsigned)geelen::SAMPLE_RATE);
        } else {
          ESP_LOGI(TAG, "Audio cfg evt (codec=%u)", (unsigned)param->audio_cfg.mcc.type);
        }
      } else {
        ESP_LOGI(TAG, "Audio cfg evt");
      }
      break;
    default:
      break;
  }
}

void A2dpSink::a2dpDataCb_(const uint8_t* data, uint32_t len) {
  if (!data || len == 0) return;
  if (geelen_idf::voiceFeedbackActive()) return;
  if (!s_pcm_sb) return;

  // Stereo int16 frames only. xStreamBufferSend may write 1 leftover byte
  // when space is odd; that desyncs L/R until the next codec reset.
  const size_t space = xStreamBufferSpacesAvailable(s_pcm_sb) & ~(size_t)3;
  const size_t want = (size_t)len & ~(size_t)3;
  const size_t n = want < space ? want : space;
  size_t sent = 0;
  if (n > 0) sent = xStreamBufferSend(s_pcm_sb, data, n, 0);
  if (sent < len) {
    static uint64_t s_last_drop_log = 0;
    uint64_t now = (uint64_t)xTaskGetTickCount();
    if (now - s_last_drop_log > pdMS_TO_TICKS(1000)) {
      s_last_drop_log = now;
      ESP_LOGW(TAG, "a2dp_pump StreamBuffer full, dropped %u bytes", (unsigned)(len - sent));
    }
  }
}

void A2dpSink::pumpTask_(void* /*arg*/) {
  uint8_t buf[1024];
  size_t prefix = 0;
  ESP_LOGI(TAG, "a2dp_pump up (core 1, prio 12)");
  for (;;) {
    if (s_pcm_flush.exchange(false, std::memory_order_relaxed)) {
      drain_pcm_sb_();
      prefix = 0;
      reset_resample_state_();
    }

    const size_t n = xStreamBufferReceive(s_pcm_sb, buf + prefix, sizeof(buf) - prefix,
                                          pdMS_TO_TICKS(20));
    const size_t total = prefix + n;
    const size_t aligned = total & ~(size_t)3;
    if (aligned >= 4) {
      resampleAndFeed_(buf, (uint32_t)aligned);
    }
    prefix = total - aligned;
    if (prefix > 0 && aligned > 0) {
      memmove(buf, buf + aligned, prefix);
    }

    apply_deferred_volume_(engine_);
  }
}

void A2dpSink::resampleAndFeed_(const uint8_t* data, uint32_t len) {
  if (!engine_ || !data || len == 0) return;
  if (geelen_idf::voiceFeedbackActive()) return;

  const int16_t* in = reinterpret_cast<const int16_t*>(data);
  const size_t in_samples = len / sizeof(int16_t);
  if (in_samples < 2) return;
  const size_t in_frames = in_samples / 2;

  const size_t fill = engine_->bufferedFrames();
  const size_t cap = geelen::RING_BUF_SAMPLES;
  const size_t target = geelen::SAMPLE_RATE;
  if (cap > 0) {
    int32_t err = (int32_t)fill - (int32_t)target;
    int32_t ppm = (err * 20000) / (int32_t)cap;
    if (ppm > 20000) ppm = 20000;
    if (ppm < -20000) ppm = -20000;
    s_step_adj_q16 = (uint32_t)((1 << 16) + (int32_t)((int64_t)ppm * (1 << 16) / 1000000LL));
  } else {
    s_step_adj_q16 = (1 << 16);
  }

  static std::vector<int16_t> window;
  window.resize((in_frames + (s_have_carry_frame ? 1 : 0)) * 2);
  size_t w_frames = 0;
  if (s_have_carry_frame) {
    window[0] = s_carry_frame[0];
    window[1] = s_carry_frame[1];
    w_frames = 1;
  }
  memcpy(window.data() + w_frames * 2, in, in_frames * 2 * sizeof(int16_t));
  w_frames += in_frames;

  if (w_frames < 2) return;

  const uint32_t base_step = (uint32_t)(((uint64_t)s_bt_pcm_hz << 16) / (uint64_t)geelen::SAMPLE_RATE);
  const uint32_t step = (uint32_t)(((uint64_t)base_step * (uint64_t)s_step_adj_q16) >> 16);
  const uint32_t end_q16 = (uint32_t)((w_frames - 1) << 16);

  static std::vector<int16_t> out;
  out.clear();
  out.reserve((size_t)((uint64_t)w_frames * geelen::SAMPLE_RATE / s_bt_pcm_hz + 4) * 2);

  while (s_pos_q16 < end_q16) {
    const uint32_t idx = s_pos_q16 >> 16;
    const uint32_t frac = s_pos_q16 & 0xFFFF;
    const uint32_t inv = 0x10000 - frac;

    const int16_t* f0 = &window[idx * 2];
    const int16_t* f1 = &window[(idx + 1) * 2];

    for (int ch = 0; ch < 2; ch++) {
      int32_t s0 = f0[ch];
      int32_t s1 = f1[ch];
      int32_t v = (int32_t)((s0 * (int32_t)inv + s1 * (int32_t)frac) >> 16);
      out.push_back((int16_t)v);
    }

    s_pos_q16 += step;
  }

  s_carry_frame[0] = window[(w_frames - 1) * 2];
  s_carry_frame[1] = window[(w_frames - 1) * 2 + 1];
  s_have_carry_frame = true;
  s_pos_q16 -= end_q16;

  if (!out.empty()) {
    engine_->feedPCM(reinterpret_cast<const uint8_t*>(out.data()), out.size() * sizeof(int16_t));
  }
}

}  // namespace geelen_idf

#endif  // CONFIG_BT_ENABLED

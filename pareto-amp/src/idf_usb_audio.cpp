// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// USB Audio Class playback producer. Host PCM → StreamBuffer → usb_pump
// (Core 1 prio 12) → resample to 44.1 kHz → AudioEngine::feedPCM.
//
// UAC is implemented with espressif/usb_device_uac (TinyUSB). esp_tinyusb
// does not expose the AUDIO class; mixing it with usb_device_uac is
// unsupported. Console in USB mode stays on UART; flash via ROM download
// (hold BOOT) or UART. Other S3 modes keep USB-Serial-JTAG.

#include "idf_usb_audio.h"

#if HAL_HAS_USB_AUDIO

#include "audio_engine.h"
#include "idf_config_manager.h"
#include "idf_events.h"
#include "idf_identity.h"
#include "idf_voice_feedback.h"

#include "usb_device_uac.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"
#include "soc/soc.h"
#ifdef CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
#include "soc/usb_serial_jtag_reg.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"

#include <atomic>
#include <cstring>
#include <cstdlib>

namespace geelen_idf {

namespace {

constexpr const char* TAG = "usb-audio";

#ifndef CONFIG_UAC_SAMPLE_RATE
#define CONFIG_UAC_SAMPLE_RATE 48000
#endif

constexpr size_t kPcmSbBytes = 24 * 1024;
constexpr uint32_t kHostRateHz = (uint32_t)CONFIG_UAC_SAMPLE_RATE;
constexpr uint32_t kIdleStopUs = 250000;

geelen::AudioEngine* s_engine = nullptr;
ConfigManager* s_cfg = nullptr;
StreamBufferHandle_t s_pcm_sb = nullptr;

std::atomic<bool> s_up{false};
std::atomic<bool> s_streaming{false};
std::atomic<uint64_t> s_last_pcm_us{0};
std::atomic<bool> s_pcm_flush{false};

uint32_t s_pos_q16 = 0;
int16_t s_carry_frame[2] = {0, 0};
bool s_have_carry = false;
uint32_t s_step_adj_q16 = (1u << 16);

extern "C" void on_volume_changed() __attribute__((weak));

void releaseUsbSerialJtag() {
#ifdef CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
  (void)usb_serial_jtag_driver_uninstall();
#if defined(USB_SERIAL_JTAG_CONF0_REG) && defined(USB_SERIAL_JTAG_USB_PAD_ENABLE)
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
#endif
  ESP_LOGI(TAG, "released USB-Serial-JTAG pad for TinyUSB OTG");
#endif
}

esp_err_t uacOutputCb(uint8_t* buf, size_t len, void* /*ctx*/) {
  if (!buf || len == 0 || !s_pcm_sb) return ESP_OK;
  if (voiceFeedbackActive()) return ESP_OK;

  const size_t space = xStreamBufferSpacesAvailable(s_pcm_sb) & ~(size_t)3;
  const size_t want = len & ~(size_t)3;
  const size_t n = want < space ? want : space;
  if (n > 0) {
    (void)xStreamBufferSend(s_pcm_sb, buf, n, 0);
  }
  s_last_pcm_us.store((uint64_t)esp_timer_get_time(), std::memory_order_relaxed);
  bool was = s_streaming.exchange(true, std::memory_order_relaxed);
  if (!was) eventPost(EV_USB_STREAMING, nullptr, 0);
  return ESP_OK;
}

void uacMuteCb(uint32_t mute, void* /*ctx*/) {
  if (!s_cfg) return;
  s_cfg->setMuted(mute != 0);
  if (&on_volume_changed) on_volume_changed();
}

void uacVolumeCb(uint32_t volume, void* /*ctx*/) {
  if (!s_cfg) return;
  // usb_device_uac maps Feature Unit dB to ~0–100. Values above 100 are
  // either a host overshoot or a negative-dB int truncated to uint32_t.
  uint8_t pct = 100;
  if (volume <= 100) pct = (uint8_t)volume;
  else if (volume > 200) pct = 0;
  s_cfg->setVolumeLR(pct, pct);
  if (&on_volume_changed) on_volume_changed();
}

void resetResample() {
  s_pos_q16 = 0;
  s_have_carry = false;
  s_step_adj_q16 = (1u << 16);
}

void drainSb() {
  if (!s_pcm_sb) return;
  uint8_t dump[256];
  while (xStreamBufferBytesAvailable(s_pcm_sb) > 0) {
    (void)xStreamBufferReceive(s_pcm_sb, dump, sizeof(dump), 0);
  }
}

void resampleAndFeed(const uint8_t* data, uint32_t len) {
  if (!s_engine || !data || len < 4) return;
  if (voiceFeedbackActive()) return;

  const int16_t* in = reinterpret_cast<const int16_t*>(data);
  const size_t in_frames = (len / sizeof(int16_t)) / 2;
  if (in_frames < 1) return;

  const size_t fill = s_engine->bufferedFrames();
  const size_t cap = geelen::RING_BUF_SAMPLES;
  const size_t target = geelen::SAMPLE_RATE;
  if (cap > 0) {
    int32_t err = (int32_t)fill - (int32_t)target;
    int32_t ppm = (err * 20000) / (int32_t)cap;
    if (ppm > 20000) ppm = 20000;
    if (ppm < -20000) ppm = -20000;
    s_step_adj_q16 = (uint32_t)((1 << 16) + (int32_t)((int64_t)ppm * (1 << 16) / 1000000LL));
  }

  int16_t window[(256 + 1) * 2];
  size_t w_frames = 0;
  size_t consumed = 0;
  while (consumed < in_frames) {
    w_frames = 0;
    if (s_have_carry) {
      window[0] = s_carry_frame[0];
      window[1] = s_carry_frame[1];
      w_frames = 1;
    }
    const size_t room = 256 - w_frames;
    const size_t take = (in_frames - consumed) < room ? (in_frames - consumed) : room;
    memcpy(window + w_frames * 2, in + consumed * 2, take * 2 * sizeof(int16_t));
    w_frames += take;
    consumed += take;
    if (w_frames < 2) {
      s_carry_frame[0] = window[0];
      s_carry_frame[1] = window[1];
      s_have_carry = true;
      return;
    }

    const uint32_t base_step =
        (uint32_t)(((uint64_t)kHostRateHz << 16) / (uint64_t)geelen::SAMPLE_RATE);
    const uint32_t step = (uint32_t)(((uint64_t)base_step * (uint64_t)s_step_adj_q16) >> 16);
    const uint32_t end_q16 = (uint32_t)((w_frames - 1) << 16);

    int16_t out[280 * 2];
    size_t out_frames = 0;
    while (s_pos_q16 < end_q16 && out_frames < 280) {
      const uint32_t idx = s_pos_q16 >> 16;
      const uint32_t frac = s_pos_q16 & 0xFFFF;
      const uint32_t inv = 0x10000 - frac;
      const int16_t* f0 = &window[idx * 2];
      const int16_t* f1 = &window[(idx + 1) * 2];
      for (int ch = 0; ch < 2; ch++) {
        int32_t v = (int32_t)((f0[ch] * (int32_t)inv + f1[ch] * (int32_t)frac) >> 16);
        out[out_frames * 2 + ch] = (int16_t)v;
      }
      ++out_frames;
      s_pos_q16 += step;
    }

    s_carry_frame[0] = window[(w_frames - 1) * 2];
    s_carry_frame[1] = window[(w_frames - 1) * 2 + 1];
    s_have_carry = true;
    if (s_pos_q16 >= end_q16) s_pos_q16 -= end_q16;

    if (out_frames > 0) {
      s_engine->feedPCM(reinterpret_cast<const uint8_t*>(out),
                        out_frames * 2 * sizeof(int16_t));
    }
  }
}

void pumpTask(void* /*arg*/) {
  uint8_t buf[1024];
  size_t prefix = 0;
  ESP_LOGI(TAG, "usb_pump up (core 1, prio 12, host %u Hz -> %u Hz)",
           (unsigned)kHostRateHz, (unsigned)geelen::SAMPLE_RATE);
  for (;;) {
    if (s_pcm_flush.exchange(false, std::memory_order_relaxed)) {
      drainSb();
      prefix = 0;
      resetResample();
    }

    const uint64_t now = (uint64_t)esp_timer_get_time();
    const uint64_t last = s_last_pcm_us.load(std::memory_order_relaxed);
    if (s_streaming.load(std::memory_order_relaxed) && last != 0 &&
        (now - last) > kIdleStopUs) {
      if (s_streaming.exchange(false, std::memory_order_relaxed)) {
        eventPost(EV_USB_STOPPED, nullptr, 0);
        s_pcm_flush.store(true, std::memory_order_relaxed);
      }
    }

    const size_t n = xStreamBufferReceive(s_pcm_sb, buf + prefix, sizeof(buf) - prefix,
                                          pdMS_TO_TICKS(20));
    const size_t total = prefix + n;
    const size_t aligned = total & ~(size_t)3;
    if (aligned >= 4) {
      resampleAndFeed(buf, (uint32_t)aligned);
    }
    prefix = total - aligned;
    if (prefix > 0 && aligned > 0) memmove(buf, buf + aligned, prefix);
  }
}

}  // namespace

bool usbAudioBegin(geelen::AudioEngine* engine, ConfigManager* cfg, const char* device_name) {
  if (s_up.load(std::memory_order_relaxed)) return true;
  if (!engine) return false;
  s_engine = engine;
  s_cfg = cfg;
  // usb_device_uac owns TinyUSB string descriptors (Kconfig product
  // "Pareto Amp"). Identity device_name is logged; runtime override would
  // need a forked descriptor table.

  releaseUsbSerialJtag();

  if (!s_pcm_sb) {
    s_pcm_sb = xStreamBufferCreateWithCaps(kPcmSbBytes, 4, MALLOC_CAP_SPIRAM);
    if (!s_pcm_sb) {
      ESP_LOGE(TAG, "StreamBuffer create failed");
      return false;
    }
  }

  if (xTaskCreatePinnedToCoreWithCaps(pumpTask, "usb_pump", 4096, nullptr, 12, nullptr, 1,
                                      MALLOC_CAP_SPIRAM) != pdPASS) {
    ESP_LOGE(TAG, "usb_pump task create failed");
    return false;
  }

  uac_device_config_t uac_cfg = {};
  uac_cfg.skip_tinyusb_init = false;
  uac_cfg.output_cb = uacOutputCb;
  uac_cfg.input_cb = nullptr;
  uac_cfg.set_mute_cb = uacMuteCb;
  uac_cfg.set_volume_cb = uacVolumeCb;
  uac_cfg.cb_ctx = nullptr;

  esp_err_t err = uac_device_init(&uac_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uac_device_init failed: %s", esp_err_to_name(err));
    return false;
  }

  s_up.store(true, std::memory_order_relaxed);
  eventPost(EV_USB_MOUNTED, nullptr, 0);
  ESP_LOGI(TAG, "UAC speaker up (%u Hz 16-bit stereo, identity '%s')",
           (unsigned)kHostRateHz, device_name && device_name[0] ? device_name : kDefaultDeviceName);
  return true;
}

bool usbAudioMounted() { return s_up.load(std::memory_order_relaxed); }
bool usbAudioStreaming() { return s_streaming.load(std::memory_order_relaxed); }
uint32_t usbAudioHostRateHz() { return kHostRateHz; }

}  // namespace geelen_idf

#endif  // HAL_HAS_USB_AUDIO

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors

#include "hal/led.h"

#include "esp_log.h"

#if HAL_HAS_WS2812

#include "led_strip.h"
#include "driver/gpio.h"

namespace geelen_hal {

static const char* TAG = "LED/ws2812";
static led_strip_handle_t s_strip = nullptr;

bool ledPresent() { return true; }
uint16_t ledPixelCount() { return (uint16_t)HAL_WS2812_COUNT; }

bool ledInit() {
  if (s_strip) return true;
  gpio_reset_pin(HAL_PIN_WS2812);

  led_color_component_format_t fmt{};
  fmt.format.r_pos = 1;
  fmt.format.g_pos = 0;
  fmt.format.b_pos = 2;
  fmt.format.w_pos = 3;
  fmt.format.bytes_per_color = 1;
  fmt.format.num_components = 3;
  led_strip_config_t strip_config = {};
  strip_config.strip_gpio_num = HAL_PIN_WS2812;
  strip_config.max_leds = HAL_WS2812_COUNT;
  strip_config.led_model = LED_MODEL_WS2812;
  strip_config.color_component_format = fmt;
  led_strip_rmt_config_t rmt_config = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 10 * 1000 * 1000,
      .mem_block_symbols = 64,
      .flags = {.with_dma = false},
  };
  esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
    s_strip = nullptr;
    return false;
  }
  led_strip_clear(s_strip);
  ESP_LOGI(TAG, "WS2812 GPIO %d, count %u, max_duty=%u%%",
           (int)HAL_PIN_WS2812, (unsigned)HAL_WS2812_COUNT,
           (unsigned)HAL_WS2812_MAX_DUTY);
  return true;
}

static uint8_t scale_duty(uint8_t c) {
  return (uint8_t)(((uint16_t)c * (uint16_t)HAL_WS2812_MAX_DUTY) / 100u);
}

void ledSetRgb(uint8_t r, uint8_t g, uint8_t b) {
  if (!s_strip) return;
  const uint8_t sr = scale_duty(r);
  const uint8_t sg = scale_duty(g);
  const uint8_t sb = scale_duty(b);
  for (uint32_t i = 0; i < (uint32_t)HAL_WS2812_COUNT; ++i) {
    led_strip_set_pixel(s_strip, i, sr, sg, sb);
  }
  led_strip_refresh(s_strip);
}

void ledClear() {
  if (!s_strip) return;
  led_strip_clear(s_strip);
}

}  // namespace geelen_hal

#else  // !HAL_HAS_WS2812

namespace geelen_hal {

bool ledPresent() { return false; }
uint16_t ledPixelCount() { return 0; }
bool ledInit() { return false; }
void ledSetRgb(uint8_t, uint8_t, uint8_t) {}
void ledClear() {}

}  // namespace geelen_hal

#endif

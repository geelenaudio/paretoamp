// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// USB Audio Class playback gadget (host → device → AudioEngine).
//
// S3 only (`HAL_HAS_USB_AUDIO`). Classic ESP32 WROVER USB is a UART
// bridge and cannot enumerate as UAC — this header is a no-op there.
// TinyUSB takes the USB DWC PHY, so USB mode is selected at boot
// (reboot on mode change) and USB-Serial-JTAG is released first.
//
// PCM from the UAC speaker callback is copied into a PSRAM StreamBuffer;
// `usb_pump` (Core 1, prio 12) resamples to 44.1 kHz s16 stereo and
// `feedPCM`s, gated on voice prompts. The I2S clock stays 44.1 kHz master.

#pragma once

#include <cstdint>

#include "hal/hal_board.h"

namespace geelen {
class AudioEngine;
}

namespace geelen_idf {

class ConfigManager;

#if HAL_HAS_USB_AUDIO
bool usbAudioBegin(geelen::AudioEngine* engine, ConfigManager* cfg, const char* device_name);
bool usbAudioMounted();
bool usbAudioStreaming();
uint32_t usbAudioHostRateHz();
#else
inline bool usbAudioBegin(geelen::AudioEngine*, ConfigManager*, const char*) { return false; }
inline bool usbAudioMounted() { return false; }
inline bool usbAudioStreaming() { return false; }
inline uint32_t usbAudioHostRateHz() { return 0; }
#endif

}  // namespace geelen_idf

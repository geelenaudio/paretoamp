// SPIFFS MP3 voice prompts (startup Wi‑Fi + mode; runtime BT connect / pairing).

#pragma once

#include "freertos/FreeRTOS.h"

namespace geelen {
class AudioEngine;
}
namespace geelen_idf {
class ConfigManager;
class WifiManager;

void voiceFeedbackSetContext(ConfigManager* cfg, WifiManager* wifi, geelen::AudioEngine* eng);
void voiceFeedbackScheduleBoot();
/** Block until boot prompts finish (or were skipped). Call before starting test-tone / other generators that share the ring. */
void voiceFeedbackWaitForBootDone(TickType_t timeout_ticks = portMAX_DELAY);
void voiceFeedbackPlayFileIfEnabled(const char* abs_path);
void voiceFeedbackOnA2dpConnected();
void voiceFeedbackOnBtPairing();

/** Stop boot clips + clear the boot gate so A2DP/Snapcast can feed (pairing / streaming). */
void voiceFeedbackAbortBootForUserInteraction();

/** True while the boot Wi‑Fi + mode voice sequence is running (blocks A2DP/Snap between clips). */
bool voiceFeedbackBootPromptsActive();

}  // namespace geelen_idf

// See idf_mode_manager.h for the design. The pure decision policy
// (`decideTransition` + `nameForSource`) lives in the header as `inline`
// so native unit tests can link it without pulling this translation
// unit's IDF deps. This file owns the runtime ModeManager singleton.

#include "idf_mode_manager.h"

#ifdef ESP_PLATFORM

#include "idf_config_manager.h"
#include "idf_events.h"
#include "esp_log.h"

namespace geelen_idf {

namespace {
constexpr const char* TAG = "mode-mgr";
ModeManager s_instance;
}  // namespace

void ModeManager::begin(ConfigManager* cfg) {
  cfg_ = cfg;
  ESP_LOGI(TAG, "mode manager up (current source=%s)",
           cfg ? nameForSource((uint8_t)cfg->getAudioSource()) : "?");
}

ModeTransitionResult ModeManager::requestChange(uint8_t prev_source, uint8_t next_source) {
  ModeTransitionResult result = decideTransition(prev_source, next_source);

  if (result == ModeTransitionResult::Noop) {
    ESP_LOGI(TAG, "requestChange: no-op (%s == %s)",
             nameForSource(prev_source), nameForSource(next_source));
    return result;
  }

  ESP_LOGI(TAG, "requestChange: %s -> %s (%s)",
           nameForSource(prev_source), nameForSource(next_source),
           result == ModeTransitionResult::RebootRequired ? "reboot"
           : result == ModeTransitionResult::HotSwap        ? "hot-swap"
           : "failed");

  // Notify listeners that a change has been requested. Posted before any
  // scheduleRestart so the SPA / log stream can show "transition started"
  // before the radio drops.
  ev_mode_change_t payload{prev_source, next_source};
  eventPost(EV_MODE_CHANGE_REQUESTED, &payload, sizeof(payload));

  return result;
}

ModeManager& modeManager() { return s_instance; }

}  // namespace geelen_idf

#endif  // ESP_PLATFORM

// Phase 3 — Mode Manager.
//
// Owns the firmware's "logical audio source" state machine. The REST handler
// for POST `/api/mode` posts `EV_MODE_CHANGE_REQUESTED`; the supervisor
// invokes `modeManager().requestChange()`, which decides whether a runtime
// hot-swap is possible or whether the firmware must reboot so `app_main`
// can rebuild the task graph.
//
// Today's policy (Phase 3 v1):
//   - prev == next:                 no-op, return Noop.
//   - either side is Bluetooth:     return RebootRequired; the supervisor
//                                   schedules a restart so app_main can
//                                   bring up / tear down Bluedroid cleanly.
//   - either side is USB:           return RebootRequired; TinyUSB vs
//                                   USB-Serial-JTAG cannot share the PHY.
//   - both sides are non-BT/USB:    return RebootRequired for now (snap /
//                                   test-tone / DLNA hot-swap is gated on
//                                   per-producer cleanup work that has not
//                                   landed yet).
//
// As `IProducer` implementations land for snapclient / test-tone, the
// "both non-BT" branch flips to HotSwap and the manager orchestrates
// `prev->stop()` + `next->start()` without touching `app_main`.
//
// The pure decision policy (`decideTransition`) is split out from
// `requestChange` so a native unit test can pin the behaviour without
// linking the IDF runtime.

#ifndef GEELEN_IDF_MODE_MANAGER_H
#define GEELEN_IDF_MODE_MANAGER_H

#include <cstdint>

namespace geelen_idf {

enum class ModeTransitionResult : uint8_t {
  Noop = 0,             // prev == next; nothing to do
  HotSwap,              // executed runtime swap; new producer is active
  RebootRequired,       // caller must scheduleRestart so app_main rebuilds
  Failed,               // start of the new producer failed; firmware kept the old one
};

// Mirror of `geelen_idf::AudioSource` (defined in idf_config_manager.h).
// We keep our own integer constants here so the native unit test can
// exercise the policy without pulling in IDF headers (`ConfigManager`,
// NVS, and friends).
namespace mode_source {
constexpr uint8_t kBluetooth = 0;
constexpr uint8_t kSnapclient = 1;
constexpr uint8_t kDlna = 2;
constexpr uint8_t kTestTone = 3;
constexpr uint8_t kUsb = 4;
}  // namespace mode_source

// Pure policy: given a (prev, next) pair, decide what kind of transition
// the supervisor should execute. No side effects, no logging, no event
// posts — wrapped by `ModeManager::requestChange` on the device.
//
// Defined inline so the native unit test can link the policy without
// pulling `src/idf_mode_manager.cpp` into the host build (the rest of
// that translation unit depends on `idf_config_manager.h`, NVS, etc.).
inline ModeTransitionResult decideTransition(uint8_t prev_source,
                                             uint8_t next_source) {
  if (prev_source == next_source) return ModeTransitionResult::Noop;

  // Bluetooth either side ⇒ reboot. The Bluedroid stack does not cleanly
  // de-init from a running task graph on ESP32 IDF — past attempts left
  // zombie GAP/SDP handlers and corrupted NVS BT pairings. Until that is
  // re-engineered (or we move to NimBLE), BT remains a "task graph
  // selected at boot" mode.
  if (prev_source == mode_source::kBluetooth ||
      next_source == mode_source::kBluetooth) {
    return ModeTransitionResult::RebootRequired;
  }

  // USB either side ⇒ reboot. TinyUSB takes the USB DWC PHY; other S3
  // modes keep USB-Serial-JTAG. Those stacks cannot share the PHY, so
  // USB is a boot-time task-graph choice like Bluetooth.
  if (prev_source == mode_source::kUsb || next_source == mode_source::kUsb) {
    return ModeTransitionResult::RebootRequired;
  }

  // Snapclient ↔ DLNA ↔ TestTone: structurally hot-swappable, but the
  // concrete `IProducer` implementations don't exist yet (the snap task
  // lifecycle currently lives in app_main and would need refactoring).
  // Returning RebootRequired here keeps today's behaviour while making
  // the architectural seam explicit; the next phase swaps this branch
  // for `prev->stop(); next->start();`.
  return ModeTransitionResult::RebootRequired;
}

// Map any AudioSource enum value to a stable short string (for logs).
// Inline for the same "native test linkage" reason as decideTransition.
inline const char* nameForSource(uint8_t source) {
  switch (source) {
    case mode_source::kBluetooth:  return "Bluetooth";
    case mode_source::kSnapclient: return "Snapclient";
    case mode_source::kDlna:       return "DLNA";
    case mode_source::kTestTone:   return "TestTone";
    case mode_source::kUsb:        return "USB";
    default:                       return "?";
  }
}

#ifdef ESP_PLATFORM
class ConfigManager;

class ModeManager {
 public:
  void begin(ConfigManager* cfg);
  ModeTransitionResult requestChange(uint8_t prev_source, uint8_t next_source);

 private:
  ConfigManager* cfg_{nullptr};
};

ModeManager& modeManager();
#endif  // ESP_PLATFORM

}  // namespace geelen_idf

#endif  // GEELEN_IDF_MODE_MANAGER_H

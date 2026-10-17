// Geelen control-plane event taxonomy.
//
// One dedicated `esp_event` loop carries every cross-cutting state transition
// in the firmware: Wi-Fi up/down/AP-fallback, mode lifecycle, voice-prompt
// progress, BT/Snap connection state, DLNA SOAP requests, and the periodic
// state-diff publication consumed by `/api/state/stream`.
//
// Why a dedicated loop (vs. the IDF default loop):
//   - We don't want our handlers contending with WIFI_EVENT / IP_EVENT
//     dispatch, which the Wi-Fi driver fires from a high-priority task.
//   - We want a deterministic core (Core 1, alongside audio) and priority
//     (8 — below httpd/audio, above the voice PCM feeder) for orchestration.
//   - We can size the queue to absorb the boot-time burst (~10 events in
//     <1 s) without competing with the 8-deep default queue.
//
// Layering: the IDs and payload structs are pure-C and portable so the
// native test harness can include this header. The IDF-specific helpers
// (event loop creation, post, register) compile only with ESP_PLATFORM.

#ifndef GEELEN_IDF_EVENTS_H
#define GEELEN_IDF_EVENTS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// All control-plane events flow through this single event base. Posted to
// the dedicated supervisor event loop (see `eventLoop()` below); never to
// the default loop.
typedef enum {
  EV_NONE = 0,

  // Wi-Fi state transitions (Phase 1 producer: idf_wifi_manager).
  EV_WIFI_UP,
  EV_WIFI_DOWN,
  EV_WIFI_AP_FALLBACK,

  // Mode lifecycle (Phase 3 producer: idf_mode_manager + /api/mode handler).
  EV_MODE_CHANGE_REQUESTED,
  EV_MODE_STARTED,
  EV_MODE_STOPPED,

  // Voice prompts (Phase 2 producer: idf_voice_feedback).
  EV_VOICE_SCHEDULE_BOOT,
  EV_VOICE_PROMPT_FINISHED,
  EV_VOICE_BOOT_DONE,

  // Bluetooth A2DP transitions (Phase 2 producer: idf_a2dp_sink callbacks).
  EV_A2DP_CONNECTED,
  EV_A2DP_DISCONNECTED,
  EV_A2DP_PAIRING,

  // Snapcast TCP transitions (Phase 1+ producer: idf_snapclient).
  EV_SNAP_CONNECTED,
  EV_SNAP_DISCONNECTED,

  // DLNA SOAP control (Phase 2 producer: idf_dlna).
  EV_DLNA_PLAY_REQUESTED,

  // Web push (Phase 4 producer: idf_supervisor; consumer: SSE state stream).
  EV_STATE_DIFF,

  // Wi-Fi credential probe (validate-before-commit onboarding).
  EV_WIFI_PROBE_START,
  EV_WIFI_PROBE_RESULT,

  // USB Audio Class gadget (S3 only; idf_usb_audio). Appended — do not
  // reorder earlier ids (native test_event_ids_are_stable).
  EV_USB_MOUNTED,
  EV_USB_STREAMING,
  EV_USB_STOPPED,

  // Sentinel — keep last so handlers can range-check for forward compat.
  EV_COUNT_,
} geelen_event_id_t;

// Wire-format payloads. All POD so they can be `memcpy`-ed into the event
// queue and received as-is by handlers (esp_event_post does the copy itself).
//
// Keep these small: every post incurs `len` bytes of memcpy into the event
// loop's queue ring. The `EV_DLNA_PLAY_REQUESTED` URI is the largest at
// 256 bytes and only fires once per casting-app Play.

typedef struct {
  uint32_t ip4_be;        // STA IP, big-endian (network order). 0 if AP-only.
} ev_wifi_up_t;

typedef struct {
  uint8_t reason;         // WIFI_EVENT_STA_DISCONNECTED reason field.
} ev_wifi_down_t;

typedef struct {
  uint8_t ok;             // 1 if probe got an IP.
  uint8_t reason;         // WIFI_EVENT_STA_DISCONNECTED reason (0 on success).
} ev_wifi_probe_result_t;

typedef struct {
  uint8_t prev_source;    // AudioSource enum value before transition.
  uint8_t next_source;    // AudioSource enum value after transition.
} ev_mode_change_t;

typedef struct {
  uint8_t prompt_id;      // VoiceClipId (defined in Phase 2 / idf_voice_cache.h).
  uint8_t is_boot;        // 1 if part of the boot pair (drives EV_VOICE_BOOT_DONE).
} ev_voice_prompt_t;

typedef struct {
  char uri[256];          // Absolute http:// URI from a DLNA SOAP Play action.
} ev_dlna_play_t;

typedef struct {
  // Phase 4 will fill this with a pointer/length pair into a persistent
  // JSON-Patch buffer maintained by the supervisor. Reserved bytes here
  // keep the struct size stable across the first wave of phases so the
  // queue sizing in `idf_events.cpp` doesn't need to change.
  uint8_t reserved[8];
} ev_state_diff_t;

#ifdef __cplusplus
}
#endif

// IDF-only API. Native builds (PlatformIO `[env:native]`) don't see this
// section so unit tests can include the header without pulling esp_event.h.
#ifdef ESP_PLATFORM

#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(GEELEN_EVENTS);

#ifdef __cplusplus
}  // extern "C"

namespace geelen_idf {

// Lazily creates the dedicated supervisor event loop on first call and
// returns its handle. Subsequent calls return the cached handle. Returns
// nullptr if loop creation fails (logged at ERROR level). Thread-safe.
esp_event_loop_handle_t eventLoop();

// Post a Geelen event to the supervisor loop with `portMAX_DELAY`. Safe to
// call from any task context; do NOT call from an ISR (use the FromIsr
// variant). `data` may be null when `len == 0`. Returns `ESP_OK` on success
// or a forwarded `esp_event_post_to` error.
esp_err_t eventPost(geelen_event_id_t id, const void* data, size_t len);

// ISR-safe post. `hp_task_woken` is forwarded to `esp_event_isr_post_to`.
// Returns `ESP_ERR_INVALID_STATE` if the loop has not been created yet
// (we never create from ISR — callers must ensure `eventLoop()` ran in a
// task context first; the supervisor's `begin()` does this at boot).
esp_err_t eventPostFromIsr(geelen_event_id_t id, const void* data, size_t len,
                           BaseType_t* hp_task_woken);

// Register a handler for a single event id (or `ESP_EVENT_ANY_ID` for all).
// Convenience wrapper around `esp_event_handler_register_with` that takes
// care of obtaining the loop handle. Returns the underlying esp_err_t.
esp_err_t eventRegister(int32_t id, esp_event_handler_t handler, void* arg);

}  // namespace geelen_idf

#endif  // __cplusplus

#endif  // ESP_PLATFORM

#endif  // GEELEN_IDF_EVENTS_H

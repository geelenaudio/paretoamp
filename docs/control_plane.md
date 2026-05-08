# Control plane

The firmware's "control plane" is the thin layer of state machines and
events that decides *when* the audio path runs, *which* producer is
authoritative, and *who* gets to talk over the speaker. It is deliberately
separate from the audio path itself, which stays a tight producer →
ring → I2S loop pinned to Core 1 (see
[architecture.md](architecture.md)).

This doc covers the four pieces that make up the control plane today:

1. The Geelen event bus (`idf_events.{h,cpp}`)
2. The supervisor (`idf_supervisor.{h,cpp}`)
3. The voice-prompt cache (`idf_voice_cache.{h,cpp}`)
4. The Mode Manager FSM (`idf_mode_manager.{h,cpp}`)

Plus the Wi-Fi reconnect ladder, Wi-Fi/BT coex preference, and the
push-based SPA state stream that hang off them.

## Why a dedicated event bus

Before this rewrite, every cross-module signal travelled through some
combination of:

- direct calls into another module from a host-task callback
  (`btAppAvdtpCb` invoking `voiceFeedbackOnA2dpConnected()`),
- semaphores + atomics shared across translation units
  (`s_voice_owners`, `s_boot_done_sem`),
- "schedule a reboot" as a control primitive
  (`scheduleRestart(500)` from `on_audio_source_changed`),
- and `vTaskDelay()` ladders ("settle for 3.5 s before the first prompt").

That worked, but coupling the BT host task to voice-feedback internals
meant every change touched three modules, and timing constants were
scattered across the codebase.

The Geelen bus is a dedicated `esp_event` loop running on Core 1 at
priority 8 with a 32-slot queue and a 4 KB stack. Putting it on its own
loop keeps it off the default loop (where Wi-Fi / IP / mDNS already
contend) and decouples it from any high-priority producer task.

```c
// src/idf_events.h
typedef enum {
  EV_NONE = 0,
  EV_WIFI_UP, EV_WIFI_DOWN, EV_WIFI_AP_FALLBACK,
  EV_MODE_CHANGE_REQUESTED, EV_MODE_STARTED, EV_MODE_STOPPED,
  EV_VOICE_SCHEDULE_BOOT, EV_VOICE_PROMPT_FINISHED, EV_VOICE_BOOT_DONE,
  EV_A2DP_CONNECTED, EV_A2DP_DISCONNECTED, EV_A2DP_PAIRING,
  EV_SNAP_CONNECTED, EV_SNAP_DISCONNECTED,
  EV_DLNA_PLAY_REQUESTED,
  EV_STATE_DIFF,
  EV_COUNT_,
} geelen_event_id_t;
```

Three properties this gives us for free:

1. **A single observable** for the SPA's live log. The supervisor's
   `on_any_event` handler unpacks every id and prints it through
   `ESP_LOGI`, so the log stream shows control-plane activity at the
   same fidelity the firmware sees it.
2. **No cross-module call graphs**. The BT GAP/AVDTP callbacks don't
   call into voice feedback any more — they `eventPost(EV_A2DP_*)` and
   the supervisor decides what (if anything) to enqueue.
3. **Stable IDs**. The native unit test in
   `test/test_native/test_native.cpp` pins the event-id integers and
   payload sizes so a careless reorder of the enum can't ship.

```c
// test/test_native/test_native.cpp
RUN_TEST(test_event_ids_are_stable);
RUN_TEST(test_event_payloads_are_pod);
RUN_TEST(test_event_payload_sizes);
```

## The supervisor

`supervisorBegin()` is the first non-trivial thing `app_main` calls. It:

1. Lazily creates the dedicated event loop (no-op if already up).
2. Registers a trace handler on `ESP_EVENT_ANY_ID` that timestamps and
   logs every event.
3. Registers typed handlers for the cross-module events the platform
   currently cares about (Wi-Fi up/down/fallback, A2DP
   connected/pairing).
4. Arms the **control-plane watchdog**: a 10 s periodic `esp_timer`
   that complains once if the bus has been silent for ≥ 30 s after
   going live.

The watchdog is intentionally just a logger — the most useful signal is
"engineer reads the log and sees the stall reason", not an unattended
reboot. It backs off to one warning per stall window so a long
deadlock prints a single line every 30 s rather than every poll.

```c
// src/idf_supervisor.cpp
void watchdog_cb(void* /*arg*/) {
  const int64_t last = s_last_event_us.load(std::memory_order_relaxed);
  if (last == 0) return;  // bus has never gone live yet — boot in progress
  const int64_t now = esp_timer_get_time();
  const int64_t gap = now - last;
  if (gap < kWatchdogStallUs) return;
  // … re-warn at most once per stall window …
  ESP_LOGW(TAG,
           "control-plane watchdog: %lld ms since last event — possible stall",
           (long long)(gap / 1000));
}
```

## Voice prompt cache

The voice prompts used to live as MP3 files in SPIFFS, decoded on demand
through the shared MP3 streamer. That worked but coupled the boot
sequence to:

- the Helix decoder being initialised before any callout could fire,
- the streamer's command mailbox being free,
- the engine ring being healthy enough to accept paced PCM,
- and the producer gates (`voiceFeedbackActive()`) staying held for the
  entire decode + tail-drain window.

For *seven* prompts that fit comfortably in PSRAM, that was a lot of
machinery. `idf_voice_cache.{h,cpp}` decodes every clip into raw PCM
once at boot, and the voice serializer becomes a tiny memcpy-paced
feeder.

```c
// src/idf_voice_cache.h
typedef enum {
  VOICE_CLIP_WIFI_CONNECTED = 0, VOICE_CLIP_WIFI_DISCONNECTED,
  VOICE_CLIP_BLUETOOTH_MODE,    VOICE_CLIP_SNAPCAST_MODE,
  VOICE_CLIP_DLNA_MODE,         VOICE_CLIP_BLUETOOTH_CONNECTED,
  VOICE_CLIP_PAIRING,
  VOICE_CLIP_COUNT_,
} voice_clip_id_t;
```

Cache lifecycle:

1. `voiceCacheInit()` runs once after `g_cfg.begin()` — SPIFFS is
   mounted, so each manifest entry can be opened, decoded, optionally
   resampled to 44.1 kHz stereo, and stored in PSRAM via
   `heap_caps_malloc(MALLOC_CAP_SPIRAM)`.
2. Decode failures (missing file, unparsable MP3) are logged but
   non-fatal: `voiceCacheGet()` returns `nullptr` and
   `voiceFeedbackEnqueueClip()` becomes a no-op for that id.
3. The cache is read-only after init. Callers borrow a `const VoiceClip*`
   and stream the PCM directly to `AudioEngine::feedPCM`.

The 3.5 s "boot settle" delay that lived inside
`voiceFeedbackScheduleBoot` exists only to absorb the Helix + streamer
init race, so it's gone with the cache.

## Mode Manager

`POST /api/mode` no longer flips a config bit and immediately calls
`scheduleRestart(500)`. Instead it hands prev/next to
`modeManager().requestChange(...)`, which runs a *pure* decision
function and decides:

```c
// src/idf_mode_manager.h
inline ModeTransitionResult decideTransition(uint8_t prev_source,
                                             uint8_t next_source) {
  if (prev_source == next_source) return ModeTransitionResult::Noop;
  if (prev_source == mode_source::kBluetooth ||
      next_source == mode_source::kBluetooth) {
    return ModeTransitionResult::RebootRequired;
  }
  // Snap ↔ DLNA ↔ TestTone are structurally hot-swappable, but the
  // IProducer wiring is not finished yet — return RebootRequired for
  // now to preserve today's behaviour.
  return ModeTransitionResult::RebootRequired;
}
```

Two reasons the policy is a pure function:

1. The `test_native` suite pins the policy without linking IDF — flip
   the "non-Bluetooth diagonal reboots" branch and CI catches the
   change before flashing.
2. The Mode Manager only owns *the decision*. Side effects
   (`eventPost(EV_MODE_CHANGE_REQUESTED)`,
   `scheduleRestart` in the API handler) stay where they were so this
   refactor doesn't disturb the rest of `app_main`.

The `IProducer` interface (`src/idf_producer.h`) is the architectural
seam the next iteration will plug in: `start()` / `stop()` /
`isRunning()` / `requiresReboot()`. Once the snapclient + DLNA stream +
test-tone tasks implement it, the non-Bluetooth diagonal flips to
`HotSwap` and the FSM orchestrates `prev->stop(); next->start();`
without touching `app_main`.

## Wi-Fi reconnect ladder + coex preference

The pre-rewrite `WifiManager` had a flat 8 s "STA still down? start
soft AP" timer — i.e. zero reconnect attempts. The most common Wi-Fi
blip on this device is a 2–5 s router DHCP renew or a Bluetooth
coexistence stall, so users saw the captive AP for transient issues
that would have cleared on their own.

Phase 1 replaces that with an exponential backoff ladder:

```c
// src/idf_wifi_manager.cpp
static constexpr int64_t kReconnectDelayUs[] = {
    500'000,        //  500 ms — first attempt absorbs router blips
    1'000'000,      //  1 s
    2'000'000,      //  2 s
    5'000'000,      //  5 s
    10'000'000,     // 10 s
    30'000'000,     // 30 s — cap reused for every subsequent attempt
};
static constexpr uint8_t kMaxReconnectAttempts = 6;
```

After ~50 s of cumulative backoff with no success, the soft AP comes up
so the user can reconfigure. `IP_EVENT_STA_GOT_IP` resets the ladder.

`startApFallback_()` and the ladder both post `EV_WIFI_AP_FALLBACK` /
`EV_WIFI_UP` / `EV_WIFI_DOWN`, so future modules (DLNA SSDP gating,
Mode Manager reconciliation) only need to subscribe to events instead
of polling `WifiManager::status()`.

### Coex preference (status: backed out)

An earlier iteration of Phase 5 called
`esp_coex_preference_set(ESP_COEX_PREFER_BT)` between
`esp_bt_controller_enable` and `esp_bluedroid_init`. On IDF 5.5 with
Classic-BT-only (BLE memory released up-front), that runtime call left
Bluedroid in a state where `gatt_init` enqueued onto a NULL
`fixed_queue` and the device panicked on boot.

The function is also marked deprecated on recent IDFs in favour of
sdkconfig tuning. If we want to bias the arbiter toward BT in a future
build, the right knob is:

```
CONFIG_ESP_COEX_SW_COEXIST_PREFER_BT=y
```

in `sdkconfig.amped-esp32-idf` (default is balanced). The runtime call
is gone; the comment in `src/idf_a2dp_sink.cpp` records why.

### Selectable Wi-Fi power-save

Phase 5 also makes `esp_wifi_set_ps` configurable. Persisted in NVS as
`WifiPsMode` (`None` / `MinModem` / `MaxModem`); default `None` matches
the legacy behaviour. Flipped via `POST /api/wifi/power-save`, applied
on next boot (we deliberately don't hot-apply because flipping ps
mid-stream interacts poorly with active BT coex).

## Push-based SPA state

The fourth piece is the SSE state stream (`/api/state/stream`). Every
SPA tab used to poll `/api/state` every 1–2 s, which on a single-worker
httpd was the dominant cause of "the web UI is sluggish". The stream:

- subscribes the tab to a 250 ms-debounced broadcast,
- replays the last full snapshot to new subscribers,
- caps concurrent subscribers at 6 (with 503 + Retry-After when full),
- uses `buildStateJson()` from `idf_api.cpp` so the payload is
  byte-for-byte identical to a `GET /api/state` response.

`web/api.js` exposes `subscribeState(cb)`; the affected pages
(overview, mode, audio, dsp, test) flipped from `getState()` polls to
`subscribeState()` subscriptions.

# Voice prompts

The device speaks short clips at boot and on a few runtime events
(Bluetooth connect, BT pairing). The MP3s ship in `data/audio/` on the
SPIFFS partition, but they are decoded into raw PCM in PSRAM at boot
and never re-decoded at runtime — see
[control_plane.md § Voice prompt cache](control_plane.md#voice-prompt-cache).

The prompt manifest lives in `idf_voice_cache.cpp`:

```c
// src/idf_voice_cache.cpp (manifest excerpt)
{ VOICE_CLIP_WIFI_CONNECTED,    "/spiffs/audio/WifiConnected.mp3"    },
{ VOICE_CLIP_WIFI_DISCONNECTED, "/spiffs/audio/WifiDisconnected.mp3" },
{ VOICE_CLIP_BLUETOOTH_MODE,    "/spiffs/audio/BluetoothMode.mp3"    },
{ VOICE_CLIP_SNAPCAST_MODE,     "/spiffs/audio/SnapcastMode.mp3"     },
{ VOICE_CLIP_DLNA_MODE,         "/spiffs/audio/DLNAMode.mp3"         },
{ VOICE_CLIP_BLUETOOTH_CONNECTED, "/spiffs/audio/BluetoothConnected.mp3" },
{ VOICE_CLIP_PAIRING,           "/spiffs/audio/Pairing.mp3"          },
{ VOICE_CLIP_USB_MODE,          "/spiffs/audio/UsbMode.mp3"          },
```

## Catalogue

| File | Spoken text (approx) | Triggered when |
|---|---|---|
| `WifiConnected.mp3` | "Wi-Fi connected" | Boot, if STA has an IP |
| `WifiDisconnected.mp3` | "Wi-Fi not connected" | Boot, if no STA IP (AP fallback) |
| `BluetoothMode.mp3` | "Bluetooth mode" | Boot, in BT mode, after the Wi-Fi clip |
| `SnapcastMode.mp3` | "Snapcast mode" | Boot, in Network mode, after the Wi-Fi clip |
| `DLNAMode.mp3` | "DLNA mode" | Boot, in DLNA mode, after the Wi-Fi clip |
| `BluetoothConnected.mp3` | "Bluetooth connected" | A2DP `CONNECTION_STATE_CONNECTED` from the phone |
| `Pairing.mp3` | "Pairing" | BT GAP `PIN_REQ_EVT` (debounced to once / 5 s) |
| `UsbMode.mp3` | ident (placeholder two-tone; replace with spoken "USB mode") | Boot, in USB mode, after the Wi-Fi clip |

In test-tone mode there is no spoken "test-tone mode" clip; only the
Wi-Fi clip plays at boot.

## When each fires

### Boot pair

`voiceFeedbackScheduleBoot` is called from `app_main` BEFORE the audio
producers (A2DP, Snap) are wired up:

```402:408:pareto-amp/src/idf_main.cpp
  geelen_idf::mp3::streamer()->setEngine(&g_engine);
  geelen_idf::voiceFeedbackSetContext(&g_cfg, &g_wifi, &g_engine);
  // Schedules the boot prompts and spawns the voice task. PCM comes
  // straight from `voiceCacheInit()` — the prior 3.5 s "boot settle" wait
  // existed only to let the Helix decoder + MP3 streamer race finish
  // before the first prompt; with the cache there is nothing to race.
  geelen_idf::voiceFeedbackScheduleBoot();
```

It enqueues:

1. `VOICE_CLIP_WIFI_CONNECTED` if `wifi.status().sta_ip != 0`, else
   `VOICE_CLIP_WIFI_DISCONNECTED`.
2. The mode clip for the current `AudioSource`
   (`VOICE_CLIP_BLUETOOTH_MODE`, `VOICE_CLIP_SNAPCAST_MODE`, or
   `VOICE_CLIP_DLNA_MODE`). In `TestTone` mode no mode clip is queued.

Since the PCM is already resident in PSRAM, the serializer feeds the
first clip immediately — there is no "boot settle" ladder. After each
clip it drains the engine ring to ~340 ms, then plays the next. After
the last queued clip it first feeds ~400 ms of silence (`AUDIO_LOW_WATER_MS
+ 100`) so the engine's low-water rebuffer abandons zeros rather than
the last syllable, then drains to ~340 ms and drops the producer-side
gate. When the boot pair has finished it posts `EV_VOICE_BOOT_DONE` so
DLNA's SOAP `Play` handler can stop waiting.

### Runtime BT prompts

The Bluetooth host-task callbacks no longer call into voice feedback
directly — they post events:

```c
// src/idf_a2dp_sink.cpp
} else if (event == ESP_BT_GAP_PIN_REQ_EVT) {
  eventPost(EV_A2DP_PAIRING, nullptr, 0);
}
// …
case ESP_A2D_CONNECTION_STATE_EVT:
  if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    eventPost(EV_A2DP_CONNECTED, nullptr, 0);
  } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    eventPost(EV_A2DP_DISCONNECTED, nullptr, 0);
  }
```

The supervisor (`src/idf_supervisor.cpp`) subscribes to those events
and decides whether to enqueue the matching cached clip — including
the 5 s `EV_A2DP_PAIRING` debounce. The BT host task never blocks on
the voice queue, and adding a new prompt for "BT disconnected" is a
one-line edit on the supervisor side.

## FIFO ordering guarantee

There is exactly one queue (`s_prompt_queue`) and exactly one consumer
(`voice_task`). The queue is FreeRTOS `xQueueCreate(8, ...)`, so up to
eight prompts can be in flight without dropping. Submit order ≡ play
order. This means:

- **Pair during boot mode clip.** If you press "connect" on your phone
  exactly while `BluetoothMode.mp3` is playing, the supervisor
  receives `EV_A2DP_CONNECTED` and enqueues
  `VOICE_CLIP_BLUETOOTH_CONNECTED`. Result: you hear "Wi-Fi
  connected → Bluetooth mode → Bluetooth connected" in that order.
  Music PCM stays gated behind all three.
- **PIN request during boot.** `EV_A2DP_PAIRING` is debounced to once
  per 5 s in the supervisor, then enqueued. It plays after whatever
  boot clip is in flight.
- **A2DP CONNECTED then DISCONNECTED then CONNECTED in quick succession.**
  Each CONNECTED enqueues the connected clip; the queue serialises
  them and you hear the prompt for each connection.

## What about disabled voice feedback?

`ConfigManager::getVoiceFeedbackEnabled` (toggled from the Settings
page or `POST /api/voice-feedback`) gates ALL prompt enqueues. When
voice is off:

- `voiceFeedbackScheduleBoot` immediately posts `EV_VOICE_BOOT_DONE`
  and returns, so DLNA `Play` waiters do not stall.
- `voiceFeedbackEnqueueClip` is a no-op.
- `voiceFeedbackActive()` stays `false`, so producers (A2DP / Snap /
  DLNA) feed the engine without any gate.

The result is the same audio behaviour as before voice feedback was
added — silent boot, immediate music.

## Replacing or recording new clips

The clips in `data/audio/` are conventional 44.1 kHz / 32 kHz / 48 kHz
MP3 files. Helix MP3 decodes them once at boot; the cache resamples to
44.1 kHz stereo if needed. Keep clips short (≤ 2 s) so the cache stays
small.

To swap a clip:

1. Drop the new MP3 in `pareto-amp/data/audio/<Name>.mp3` with
   the same basename.
2. `pio run -t uploadfs` to flash the SPIFFS partition.
3. Reboot the device — the cache decodes once at startup.

To add a brand new clip, append a row to `kClipManifest` in
`idf_voice_cache.cpp`, add the matching `VOICE_CLIP_*` id in
`idf_voice_cache.h`, call `voiceFeedbackEnqueueClip(VOICE_CLIP_FOO)`
from wherever the event lives, and ship the MP3 alongside the others.

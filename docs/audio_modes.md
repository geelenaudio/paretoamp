# Audio modes

The device runs in exactly one **audio source** at a time. The choice is
persisted in NVS (`audio_source` key) and surfaced via the API as a string
(`bluetooth`, `snapclient`, `dlna`, `test_tone`). Mode-specific tasks are
spawned at boot from `app_main`; switching modes from the Web UI persists
the new choice and **reboots** the device so the task graph is rebuilt
cleanly (see "Why mode changes reboot" below).

## The four modes

| Mode | Enum | API string | Producer that feeds the engine | DLNA renderer always on? |
|---|---|---|---|---|
| Bluetooth | `AudioSource::Bluetooth` | `bluetooth` | `a2dp_pump` prio 12 (Bluedroid SBC → StreamBuffer → resample → `feedPCM`) | Yes (if `dlna_enabled`) |
| Snapcast | `AudioSource::Network` | `snapclient` | `Snapclient::tick` (TCP WireChunk -> `feedPCM`) | Yes (if `dlna_enabled`) |
| DLNA only | `AudioSource::Dlna` | `dlna` | `mp3::Streamer` (HTTP MP3 -> Helix -> resample -> `feedPCM`) | Yes |
| Test tone | `AudioSource::TestTone` | `test_tone` | `idf_test_tone` (sine / square / pink / log sweep) | No |

`AudioSource` is defined in `idf_config_manager.h`; mapping to API strings
lives in `idf_api.cpp`:

```151:167:ESP32_AudioPlatform/src/idf_api.cpp
    case AudioSource::Bluetooth: return "bluetooth";
    case AudioSource::Network:   return "snapclient";
    case AudioSource::Dlna:      return "dlna";
    case AudioSource::TestTone:  return "test_tone";
  }
  return "";
}

bool parseAudioSource(const char* s, AudioSource* out) {
  if (!s || !out) return false;
  if (!strcmp(s, "bluetooth") || !strcmp(s, "bt")) { *out = AudioSource::Bluetooth; return true; }
  if (!strcmp(s, "snapclient") || !strcmp(s, "snap") || !strcmp(s, "network")) {
    *out = AudioSource::Network; return true;
  }
  if (!strcmp(s, "dlna") || !strcmp(s, "upnp")) { *out = AudioSource::Dlna; return true; }
```

## Mode-specific behaviour

### Bluetooth

```309:319:ESP32_AudioPlatform/src/idf_main.cpp
  if (src == geelen_idf::AudioSource::Bluetooth) {
    if (!g_wifi.isApOnly()) {
      g_a2dp.begin("Geelen Audio");
    } else {
      ESP_LOGW("geelen",
               "AP-only Wi-Fi onboarding: deferring Bluetooth ~%u s (or until STA "
               "gets an IP) to avoid Bluedroid init OOM; then A2DP starts anyway",
               (unsigned)((kBtDeferMaxPolls * kBtDeferPollMs) / 1000));
      xTaskCreate(deferred_a2dp_start_task, "bt_defer", 4096, nullptr, 5, nullptr);
    }
  }
```

- Bluedroid + A2DP sink come up immediately on boot if Wi-Fi is in STA or
  is disabled. If Wi-Fi is in AP-only onboarding mode, BT is deferred (up
  to ~3 s, or until Wi-Fi gets an IP) so the captive APSTA stack does not
  blow out the Bluedroid OSI timer pool. After the deferral, A2DP starts
  anyway so the user can pair without ever touching Wi-Fi. Discoverable
  only during the 180 s pairing window (Overview Pair reopens it).
- On `ESP_A2D_CONNECTION_STATE_CONNECTED`, the A2DP callback enqueues the
  `BluetoothConnected.mp3` voice prompt. Music PCM is gated behind the
  voice owner refcount, so the prompt finishes (including the 340 ms tail
  drain) before the first SBC sample is admitted to the engine ring.

### Snapcast (Network)

```332:395:ESP32_AudioPlatform/src/idf_main.cpp
  } else if (src == geelen_idf::AudioSource::Network) {
    ...
    g_snap.setEngine(&g_engine);
    ...
    xTaskCreatePinnedToCore(
        [](void*) {
          char h[geelen_idf::SNAPCAST_HOST_MAX];
          while (true) {
            if (!g_snap.isConnected()) {
              g_cfg.getSnapcastHost(h, sizeof(h));
              if (h[0] == '\0') {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
              }
              const uint16_t p = g_cfg.getSnapcastPort();
              if (!g_snap.connect(h, p)) {
                ...
                continue;
              }
            }
            g_snap.tick();
          }
        },
        "snap", 4096, nullptr, 5, nullptr, 1);
  }
```

- The snap task is created inside `app_main` and never exits. It re-reads
  host/port from NVS on every reconnect, so editing the host in the Web UI
  takes effect within ~1 s without a reboot.
- The data callback inside `tick()` gates on `voiceFeedbackActive()`,
  which means WireChunks during the boot mode clip get dropped on the
  floor (rather than overwriting the prompt's PCM).

### DLNA

```296:298:ESP32_AudioPlatform/src/idf_main.cpp
  if (g_cfg.getDlnaEnabled() && src != geelen_idf::AudioSource::TestTone) {
    geelen_idf::dlna::start((httpd_handle_t)g_http.nativeHandle(), &g_cfg);
  }
```

DLNA is two things:

1. **The renderer (SSDP + SOAP)** — always on if `dlna_enabled` and we are
   not in test-tone mode. So a casting app on your phone can wake the
   device by sending `Play` even while the user-selected source is
   Bluetooth or Snapcast (in those modes, when DLNA `Play` arrives, the
   MP3 streamer takes over the engine ring; the A2DP / Snap producer
   gates kick in via `voiceFeedbackActive` only during prompts, not
   continuously).
2. **The dedicated DLNA-only mode** — pure renderer, no Snap or BT producer.

The SOAP `Play` handler waits for the boot pair to drain (15 s hard cap)
before it kicks off the streamer, so a casting app cannot preempt the
mode clip:

```280:291:ESP32_AudioPlatform/src/idf_dlna.cpp
  if (strcmp(action, "Play") == 0) {
    // A casting app issuing Play during the boot mode clip would have the
    // streamer abort the prompt and start music. Gate on the boot pair (15 s
    // hard cap so the SOAP request never hangs forever).
    voiceFeedbackWaitForBootDone(pdMS_TO_TICKS(15000));
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_current_uri[0]) {
      s_state = TransportState::Playing;
      mp3::streamer()->play(s_current_uri);
    }
    xSemaphoreGive(s_mutex);
    return send_soap_response(req, AVT_URN, "Play", "");
  }
```

### Test tone

The test-tone task is spawned only after the voice boot pair drains, so
the user actually hears the boot Wi-Fi clip even when the device boots
into the test-tone page:

```188:193:ESP32_AudioPlatform/src/idf_main.cpp
/** Test tone shares the engine ring with voice prompts; start it only after boot clips finish. */
static void test_tone_after_voice_task(void*) {
  geelen_idf::voiceFeedbackWaitForBootDone(portMAX_DELAY);
  geelen_idf::testToneStart(&g_engine, &g_cfg);
  vTaskDelete(nullptr);
}
```

In test-tone mode there is no spoken "TestToneMode" clip — the boot
sequence plays only the Wi-Fi clip and then idle until the user starts a
waveform from the Web UI.

## Why mode changes reboot

```47:62:ESP32_AudioPlatform/src/idf_main.cpp
// Hook invoked by the HTTP server after POST /api/mode persists a new
// source selection. Until the runtime source manager lands (Phase 9 wires
// DLNA on top of it), there is no clean way to stop the Snap task or
// start DLNA in-place — tasks pinned at boot stick around. A brief reboot
// is the simplest correct answer: the on-disk NVS already reflects the
// new source, so app_main picks the right task graph after restart.
```

Tasks created in `app_main` are pinned for the lifetime of the firmware:
the snap task is in an infinite loop, the BT controller / Bluedroid stack
allocates substantial RAM that is hard to release cleanly, and the test
tone has its own producer thread. A teardown path that handles every
combination is much riskier than a 500 ms restart, especially since the
NVS write happens BEFORE the restart so the device boots straight into
the new mode.

The boot sequence (Wi-Fi clip + new mode clip) gives the user audible
confirmation that the mode change took effect.

## Mode hooks

- `extern "C" void on_audio_source_changed(unsigned prev, unsigned next)` —
  fired by the API after the new mode is persisted, schedules the reboot.
- `extern "C" void on_snapcast_config_changed()` — fired after a host/port
  edit; calls `g_snap.disconnect()` so the snap task picks up the new
  config on its next iteration. No reboot needed.
- `extern "C" void on_volume_changed()` / `on_channels_changed()` /
  `on_eq_changed()` — apply state to the live engine without reboot.

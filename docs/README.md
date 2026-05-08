# ESP32 Audio Platform — Documentation

This directory documents the firmware that runs on the Amped-ESP32 board: how
the audio pipeline is wired together, how the device onboards onto Wi-Fi,
which spoken prompts you can expect to hear, and how to build / flash / extend
the project.

If you are operating the device, start with the **Operating guide**. If you
are diving into the code, start with **Architecture**.

## Contents

### Operations

- [Operating guide](operating_guide.md) — first-time setup, what each prompt
  means, switching modes from the Web UI, troubleshooting.
- [User guide (legacy)](user-guide.md) — older operations notes (kept for
  reference; the operating guide supersedes most of it).
- [Manual test matrix](manual-test-matrix.md) — checklist for verifying a
  build before flashing.

### How it works

- [Architecture](architecture.md) — task graph, ring buffer, MP3 streamer
  command queue, voice cache + prompt FSM. Diagrams included.
- [Control plane](control_plane.md) — Geelen event bus, supervisor +
  watchdog, Mode Manager FSM, Wi-Fi reconnect ladder, BT coex
  preference, push-based SPA state. Diagrams included.
- [Audio modes](audio_modes.md) — the four `AudioSource` modes
  (`Bluetooth`, `Network`/Snapcast, `Dlna`, `TestTone`), who feeds the engine
  in each, why switching modes still reboots today.
- [Voice prompts](voice_prompts.md) — every clip in the PSRAM cache, when
  each fires, FIFO ordering guarantees.
- [Wi-Fi onboarding](wifi_onboarding.md) — STA timeout, AP fallback, the
  exponential-backoff reconnect ladder, the captive HTML, the
  `/api/wifi` form, and the `/wifi` SPA page. Sequence diagrams included.

### Reference

- [REST API](api.md) — endpoint reference (legacy notes; the canonical schema
  lives in [`web/api/openapi.yaml`](../ESP32_AudioPlatform/web/api/openapi.yaml)
  and is rendered at `/docs` on the device).
- [Hardware](hardware.md) — MCU, DAC, I2S pinout, amp enable, PSRAM.

### Development

- [Development](development.md) — PlatformIO env, build / flash / upload-fs
  commands, native test, adding SPIFFS audio, OpenAPI workflow.

// Phase 3 — minimal producer abstraction for runtime-swappable audio sources.
//
// Today the firmware launches A2DP / Snapclient / DLNA / TestTone / USB in `app_main`
// based on `cfg->getAudioSource()`. Switching modes therefore requires a reboot
// because the task graph is set up once and never torn down. Phase 3 takes
// the first step toward runtime swap by introducing this interface; future
// concrete implementations under `src/producers/` will let the Mode Manager
// coordinate (stop old → start new) without touching `app_main`.
//
// Today's status:
//   - Bluetooth (`A2dpSink`)            — REBOOT-REQUIRED on this MCU; the
//     Bluedroid stack does not cleanly de-init from a running task graph
//     and tearing it down has historically corrupted nvs / left zombie
//     handlers. Bluetooth → other and other → Bluetooth keep using the
//     reboot path indefinitely.
//   - Snapclient (`Snapclient`)         — task lifecycle owned in app_main
//     today; will move into a `SnapProducer` in a follow-up.
//   - DLNA (`dlna::start`)              — passive: SSDP advertise + SOAP
//     handlers; no active task to stop. DLNA is "available if the user
//     enabled it" regardless of mode; the Mode Manager treats it as a
//     no-op on transitions but tracks it for state reporting.
//   - TestTone (`testToneStart/Stop`)   — already has a clean stop path;
//     trivial wrap.
//   - USB (`usbAudioBegin`)             — REBOOT-REQUIRED; TinyUSB takes the
//     USB DWC PHY and cannot share it with USB-Serial-JTAG. S3 only.
//
// The interface itself is deliberately tiny: start, stop, name, requiresReboot.
// Anything richer (state-version reporting, telemetry, mute requests) lands
// via events on `GEELEN_EVENTS` rather than in additional virtuals — keeps
// each producer self-contained and means adding a new source (AirPlay,
// Spotify Connect, …) doesn't require interface changes.

#ifndef GEELEN_IDF_PRODUCER_H
#define GEELEN_IDF_PRODUCER_H

#ifdef ESP_PLATFORM

namespace geelen_idf {

class IProducer {
 public:
  virtual ~IProducer() = default;

  /** Stable short name for logs and `/api/state`. */
  virtual const char* name() const = 0;

  /** True if a transition into / out of this producer requires a reboot.
   *  Today this only ever returns true for the Bluetooth A2DP producer. */
  virtual bool requiresReboot() const = 0;

  /** Idempotent. Returns true if the producer is now running (or already was). */
  virtual bool start() = 0;

  /** Idempotent. After return the producer must not feed any more PCM into
   *  the engine ring (the Mode Manager calls `start()` on the next producer
   *  immediately afterwards and races would otherwise interleave). */
  virtual void stop() = 0;

  virtual bool isRunning() const = 0;
};

}  // namespace geelen_idf

#endif  // ESP_PLATFORM

#endif  // GEELEN_IDF_PRODUCER_H

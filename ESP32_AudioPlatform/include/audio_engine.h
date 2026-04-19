/**
 * Audio Engine – 10-band EQ, PSRAM ring buffer, I2S output.
 * Feed PCM via feedPCM(); call process() to drain buffer → EQ → I2S.
 */

#ifndef GEELEN_AUDIO_ENGINE_H
#define GEELEN_AUDIO_ENGINE_H

#include <cstdint>
#include <cstddef>

namespace geelen {

constexpr size_t AUDIO_EQ_BANDS = 10;
// Two cascades (calibration + preference), each 10 bands, fed one after
// the other before the per-channel volume stage.
constexpr size_t AUDIO_EQ_LAYERS = 2;
constexpr int EQ_LAYER_CALIBRATION = 0;
constexpr int EQ_LAYER_PREFERENCE  = 1;

// Parametric band, same wire shape as firmware config. Gain in dB, Q and
// frequency in natural units.
struct EqBand {
  float freq_hz;
  float gain_db;
  float q;
};

constexpr int SAMPLE_RATE = 44100;
// 2 seconds of stereo 16-bit PCM at 44.1kHz (frames, not int16 samples)
constexpr size_t RING_BUF_SAMPLES = (size_t)SAMPLE_RATE * 2;
constexpr size_t RING_BUF_BYTES = RING_BUF_SAMPLES * sizeof(int16_t) * 2;

// Buffering strategy:
// - Prefill before starting playback to absorb BT/WiFi scheduling jitter.
// - If we drop too low, pause and rebuffer.
//
// The Snapclient is deliberately "dumb" (no TimeProvider, no drift correction),
// so once playback starts the ring level stays at whatever prefill we picked —
// server and I2S both run at 44.1 kHz. That means the prefill IS the runtime
// margin against WiFi/BT-coex jitter, not just a startup setting. 400 ms was
// too tight: a single 200 ms WiFi burst would drop us under low-water and
// cause an audible rebuffer, and shorter ones would empty the 185 ms DMA
// pipeline and click. ~1 s is empirically stable and matches the snapserver's
// default `buffer` setting, so the TCP socket stops accumulating once we're
// at steady state. Low-water bumped to match: rebuffer a little earlier, so
// gaps are a soft reset instead of a cliff-edge stutter.
constexpr uint32_t AUDIO_PREFILL_MS = 1000;
constexpr uint32_t AUDIO_LOW_WATER_MS = 300;

class AudioEngine {
 public:
  AudioEngine();
  ~AudioEngine();

  bool begin();
  /** Set I2S TX channel handle (new driver). Pass from main after i2s_channel_init_std_mode. */
  void setI2SChannel(void* tx_handle);
  // Legacy graphic-EQ entry point: interprets the int8 array as per-band
  // dB gain at fixed centre frequencies and writes it into the calibration
  // cascade with a default Q of 1.2. Kept as a compatibility shim for
  // code paths that haven't been rewritten to setEqCurve yet.
  void setEQGains(const int8_t gains[AUDIO_EQ_BANDS]);
  // Phase 5 parametric entry point. Replaces the given cascade's bands
  // and on/off state and rebuilds its biquad coefficients. Safe to call
  // from any task: the audio task reads coefficients atomically per block.
  //
  // `layer` is one of EQ_LAYER_CALIBRATION / EQ_LAYER_PREFERENCE.
  void setEqCurve(int layer, const EqBand bands[AUDIO_EQ_BANDS], bool enabled);
  // Linear gain applied after both EQ cascades and mono/channel processing,
  // expressed in dB. Must be ≤ 0 (otherwise clipping becomes likely). Used
  // to implement automatic headroom: the API layer sweeps computeEqPeakGainDb()
  // and passes the negative of that to keep peak output at ≤ 0 dB.
  void setHeadroomDb(float db);
  // Compute the maximum combined |H(f)| of the enabled cascades across
  // 20 Hz..20 kHz (dense log-spaced sweep). Returns dB. If both cascades
  // are disabled the result is 0 dB. Deterministic — pure function of the
  // stored coefficients, safe to call from any task.
  float computeEqPeakGainDb() const;
  // Master volume (applies to both channels equally). Kept for legacy callers;
  // new code should use setVolumeLR for independent channels.
  void setVolume(uint8_t vol_percent);
  // Per-channel volume (0..100 each). Each channel is independently
  // attenuated before I2S.
  void setVolumeLR(uint8_t l_pct, uint8_t r_pct);
  // Hard mute: zero both outputs until cleared. Mute overrides volume but
  // preserves the saved levels so un-muting restores what the user set.
  void setMuted(bool muted);
  // Channel routing:
  //   l_on / r_on : if false, the corresponding output is silenced.
  //   mono        : if true, L and R are averaged ((L+R)/2) and the result
  //                 is fed to both enabled outputs. Downstream the usual
  //                 per-channel gain / on-off still applies, so "mono ->
  //                 L only" correctly sums to L and silences R.
  void setChannels(bool l_on, bool r_on, bool mono);

  void feedPCM(const uint8_t* data, size_t len_bytes);
  // Returns true if bytes were written to I2S (i.e. the call blocked briefly
  // inside i2s_channel_write and yielded CPU naturally). Returns false if the
  // call early-returned — prefill, rebuffer, underrun, or I2S backpressure
  // with written=0. Callers driving process() from a tight task loop should
  // vTaskDelay(1) when it returns false, so the task doesn't busy-spin the
  // core during those quiet windows.
  bool process();

  bool isRunning() const { return running_; }
  /** Current buffered audio (frames at 44.1kHz stereo). Useful for drift control. */
  size_t bufferedFrames() const;

 private:
  size_t ringFillSamples() const;
  void initBiquads();
  void rebuildCascade(int layer);
  void processSamples(int16_t* buf, size_t num_frames);

  // Per-cascade coefficients + per-channel biquad state.
  struct Cascade {
    EqBand bands[AUDIO_EQ_BANDS];
    float  b0[AUDIO_EQ_BANDS], b1[AUDIO_EQ_BANDS], b2[AUDIO_EQ_BANDS];
    float  a1[AUDIO_EQ_BANDS], a2[AUDIO_EQ_BANDS];
    float  x1[AUDIO_EQ_BANDS][2], x2[AUDIO_EQ_BANDS][2];
    float  y1[AUDIO_EQ_BANDS][2], y2[AUDIO_EQ_BANDS][2];
    bool   enabled;
  };
  Cascade cascades_[AUDIO_EQ_LAYERS];

  float headroom_linear_;  // 10^(db/20); <= 1.0
  float volume_;           // legacy master (ganged) volume, 0..1
  float volume_l_;         // per-channel linear gain, 0..1
  float volume_r_;
  bool muted_;
  bool l_on_;
  bool r_on_;
  bool mono_;              // true -> sum L+R before per-channel gain

  int16_t* ring_buf_;
  volatile size_t ring_head_;
  volatile size_t ring_tail_;
  size_t ring_cap_;
  bool running_;
  void* i2s_tx_handle_;  /* i2s_chan_handle_t from driver/i2s_std.h */

  bool playing_;
  uint32_t underruns_;
  uint32_t overflows_;
  unsigned long last_stats_ms_;
};

}  // namespace geelen

#endif  // GEELEN_AUDIO_ENGINE_H

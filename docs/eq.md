# Single-layer parametric EQ

The firmware exposes one PEQ curve (no calibration/preference layers).
Band count is board-specific:

| Board  | Backend   | User bands | DSP implementation                                      |
| ------ | --------- | ---------- | ------------------------------------------------------- |
| Amped  | software  | 10         | `AudioEngine` cascade of biquads on the audio task      |
| Louder | hardware  | 15         | TAS5825M on-chip BQ slots, uploaded as signed Q1.31     |

`EQ_MAX_BANDS` (storage) is always 15 so a Louder preset round-trips through
an Amped build without losing metadata. Amped truncates to
`HAL_EQ_MAX_BANDS_ACTIVE` (10) at apply time and the SPA shows a warning
when a loaded preset is longer than `state.eq.max_bands`.

`GET /api/state` reports:

```json
"eq": {
  "enabled": true,
  "band_count": 8,
  "max_bands": 10,
  "backend": "software",
  "bands": [ ... ],
  "headroom": { "mode": "auto", "manual_db": 0, "auto_db": -2.1 },
  "active_slot": 255
}
```

`backend` is `"software"`, `"hardware"`, or `"software (fallback)"` when a
Louder I2C upload fails for the session.

## Hardware upload (Louder)

1. `eqBuildStageList` expands each user band into 0–2 `EqStage`s (24 dB/oct
   shelves produce two biquads).
2. Each stage's `b0/b1/b2/a1/a2` is converted with `eqFloatToQ1_31`
   (`include/eq_q131.h`). The identity biquad is `{b0=1.0 → INT32_MAX}`.
3. `tas5805m_set_bq_coeffs` writes Book 0xAA slots for left and right
   (ganged). Unused slots get the identity biquad.
4. `tas5805m_commit_eq_page` toggles DSP_MISC so the DAC swaps the page bank
   without a pop.
5. On any I2C error the firmware logs ERROR, sets `backend` to
   `"software (fallback)"`, and runs the cascade in `AudioEngine`.

If stage expansion exceeds 15 slots the extra slope-only stages are dropped
and a WARN is logged.

See [hardware/louder-wrover.md](hardware/louder-wrover.md) and
[perf/bq_reference.py](perf/bq_reference.py).

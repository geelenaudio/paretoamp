# Single-layer parametric EQ

The firmware exposes one PEQ layer (no calibration/preference split).
Band count and backend are board-specific:

| Board                  | Backend  | User bands | Volume                         | DSP implementation                                      |
| ---------------------- | -------- | ---------- | ------------------------------ | ------------------------------------------------------- |
| Amped / Amped Plus     | software | 10         | Software (`AudioEngine` gain)  | One cascade of biquads on the audio task (always ganged)|
| Louder-ESP32           | hardware | 15         | TAS5805M register `0x4C`       | On-chip BQ RAM, Book 0xAA, Q5.27, 15 slots per channel  |
| Louder-ESP32-Plus / Pro| hardware | 15         | TAS5825M register `0x4C`       | Same Q5.27 BQs; different page map (probed via DIE_ID)  |

Amped Plus PCM5122 6-band DSP is **not** in this firmware; flashing an Amped
env always uses software PEQ and software volume.

`EQ_MAX_BANDS` (storage) is always 15 so a Louder preset round-trips through
an Amped build without losing metadata. Amped truncates to
`HAL_EQ_MAX_BANDS_ACTIVE` (10) at apply time and the SPA shows a warning
when a loaded preset is longer than `state.eq.max_bands`.

## Channel mode (Louder only)

Default is **ganged**: one editor, left copied onto right (`eq_bands` is
canonical; `eq_bands_r` is kept identical on every ganged write).

Louder TAS silicon has independent 15+15 Book 0xAA slots. `HAL_HAS_HW_PEQ`
advertises `state.eq.supports_split`. The SPA **Unlink** toggle sets
`channel_mode` to `"split"` and edits `bands` (left) vs `bands_r` (right)
separately, with **Copy L→R / R→L**. Relink confirms, then gangs from
**left**.

Amped never splits: independent software curves would double EQ math in
`processSamples`. `supports_split` is false, `channel_mode` is always
`"ganged"`, and a Louder split preset loaded on Amped applies the **left**
curve only (no 400).

If Louder I2C upload fails, software fallback is **ganged on the left
curve** — not two software cascades. `backend` stays `"hardware"` or
`"software (fallback)"`.

Auto headroom uses `max(peak(L), peak(R))` from the band lists (plus
`EQ_INTERSAMPLE_HEADROOM_DB`), even when the software cascade is off so
HW PEQ boosts still pad PCM before I2S.

NVS: `eq_bands` (left/ganged), `eq_bands_r` (missing on old devices → copy
from L), `eq_ch_mode` (0 ganged / 1 split). One `eq_on` and one
`eq_active_bands` for both sides.

Presets are **V4** (`EGP4`) with `channel_mode` and a second 15-band array.
V3 files load as ganged (R=L).

`GET /api/state` reports:

```json
"eq": {
  "enabled": true,
  "band_count": 8,
  "max_bands": 10,
  "backend": "software",
  "supports_split": false,
  "channel_mode": "ganged",
  "bands": [ ... ],
  "headroom": { "mode": "auto", "manual_db": 0, "auto_db": -2.1 },
  "active_slot": 255
}
```

On Louder, `supports_split` is true and `bands_r` is always present.

`backend` is `"software"`, `"hardware"`, or `"software (fallback)"` when a
Louder I2C upload fails for the session.

Bluetooth AVRCP, the encoder, SPA, and NVS all share the same **0–100%**
control value. Hardware register mapping is private to the Louder HAL.

## Hardware upload (Louder)

TAS5825M datasheet ([tas5825m.pdf](hardware/tas5825m.pdf), SLASEH7H) §9.5.3:
I2S stable → Hi-Z + enable DSP → ≥5 ms → **initialize DSP coefficients** →
PLAY. After PLAY, volume (`0x4C`) and EQ RAM change on the fly — no PDN /
full chip re-init.

Do **not** use [slaa847.pdf](hardware/slaa847.pdf) (EEPROM-via-SPI boot) or
[sbaa414.pdf](hardware/sbaa414.pdf) (PCM6xx0 ADC Q1.31). TAS58xx biquads are
**Q5.27** (`1.0 = 0x08000000`), not Q1.31.

1. At PLAY bring-up, still in Hi-Z, `tas5805m_enter_play` writes 15+15
   identity slots (Book 0xAA, sequential 20-byte records) and clears
   `USE_DEFAULT_COEFFS` (`DSP_CTRL` `0x46` bit0 = 0, 48 kHz processing =
   `0x08`).
2. `eqBuildStageList` expands each side independently at 44.1 kHz. Unused
   slots get identity `b0=0x08000000`.
3. Each stage is packed with `eqStageToTas5825mQ5_27` (`include/eq_q131.h`).
   Software IIR subtracts `a1`/`a2`; the TAS DSP **adds** them, so textbook
   feedback coefficients are negated.
4. Live EQ stays in PLAY: bypass → write left table from L stages and right
   table from R stages → un-bypass. TAS5825M Book 0x8C page `0x0B`:
   **gang=1 when ganged, gang=0 when split**. TAS5805M `DSP_MISC` is
   unchanged (independent RAM). DIE_ID `0x95` selects TAS5825M.
5. On any I2C error the firmware bypasses EQ, sets `backend` to
   `"software (fallback)"`, and runs a **ganged** cascade on the left curve.

If stage expansion exceeds 15 slots the extra slope-only stages are dropped
and a WARN is logged.

See [hardware/louder-wrover.md](hardware/louder-wrover.md) and
[perf/bq_reference.py](perf/bq_reference.py).

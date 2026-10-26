<!--
SPDX-License-Identifier: GPL-3.0-or-later
-->
# tas5805m — TAS5825M DAC driver (vendored, patched)

Derived from https://github.com/sonocotta/esp32-tas5805m-dac (commit
`c5d70d6ad765a58d798e887c62c3e9cdf49982a0`) and licensed
**GPL-3.0-or-later**. See [../../../COPYING](../../../COPYING) and
[../../../docs/licensing.md](../../../docs/licensing.md) for the
whole-firmware licensing implications.

## What lives here

- `include/tas5805m.h` — public API used by the HAL wrapper
  (`src/hal/dac_tas5825m.cpp`). Adds two functions on top of the
  upstream shape:
  - `tas5805m_set_bq_coeffs(band, ch, b0, b1, b2, a1, a2)` — raw Q5.27
    biquad upload for a single slot of the on-chip 15-band PEQ (TI-form
    a1/a2, Book 0xAA).
  - `tas5805m_commit_eq_page()` — enable 15-band EQ (TAS5825M Book 0x8C
    bypass + gang bit, or TAS5805M `DSP_MISC`) so the DSP picks up host RAM.
  - `tas5805m_set_eq_ganged(bool)` — TAS5825M gang_eq (1 = L/R share left
    RAM, 0 = independent banks). Applied on the next enable.
- `src/tas5805m.c` — the actual driver, rewritten against the modern
  ESP-IDF `driver/i2c_master.h` bus API. Register addresses, book/page
  layout, and startup semantics come from the upstream and the TAS5825M
  datasheet (chapters 6.5 and 8).
- `patches/` — historical patches captured from earlier upstream
  snapshots for §5a compliance. Currently empty because the driver is
  a clean-room re-implementation against the upstream reference; the
  next upstream sync will land its diff here.

## Modifications from upstream (GPL §5a)

1. **I/O layer rewrite** — the upstream uses the legacy `driver/i2c.h`
   API which owns the I2C peripheral outright. Our HAL owns a shared
   bus handle (see `src/hal/dac_tas5825m.cpp`) so we re-implemented the
   register-write helpers against `driver/i2c_master.h` /
   `i2c_master_bus_handle_t`.
2. **Raw BQ upload** — `tas5805m_set_bq_coeffs` / `tas5805m_commit_eq_page`
   are new. Upstream only exposes preset upload from a compiled
   coefficient table (`eq/tas5805m_eq.h`); we drive the DSP at runtime
   from a user-editable PEQ curve.
3. **Trimmed surface** — upstream ships mute / mixer / clipper / fault
   helpers we don't currently use. We may re-import those piecemeal as
   the Louder feature set grows.

## Build gating

The component is registered unconditionally so `pio run -e <any-env>`
succeeds, but the wrapper source (`src/hal/dac_tas5825m.cpp`) is
`HAL_HAS_I2C_DAC`-gated. On Amped builds `HAL_HAS_I2C_DAC=0`, the
wrapper compiles to nothing, and the driver's symbols are unreferenced —
so `libtas5805m.a` does not end up in the final Amped image. The
_source tree_ still contains the GPL-3.0 driver, which is what forces
the whole firmware to ship under GPL-3.0-or-later (see
`docs/licensing.md`).

## Where does this run?

On Louder builds (`louder-esp32-idf`, `louder-esp32s3-idf`) the driver
is initialised at boot from `src/hal/dac_tas5825m.cpp::dacInit()`. It
verifies the DAC responds on I2C, drops it into HI-Z, and returns.
`dacAmpEnable(true)` later transitions HI-Z → PLAY once I2S clocks are
stable. `dacApplyPeqBands` streams the user's live PEQ into all 15 BQ
slots and commits the page bank.

On Amped builds the driver source is unreachable and
`src/hal/dac_none.cpp` provides the DAC HAL (`dacInit()` = no-op,
`dacAmpEnable` drives GPIO 13).

## Bumping the upstream

1. Fetch upstream at the new commit.
2. Diff against `src/tas5805m.c` and update the register map / startup
   sequence if the datasheet numbers have moved.
3. Refresh the modification notes above and the pinned commit hash in
   `docs/licensing.md`.
4. Verify the change against `pio test -e native --filter test_tas5825m_bq`
   and on hardware if available.

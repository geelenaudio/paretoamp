# Performance capture procedure

This folder collects "before/after" snapshots for the multi-board / 1-layer PEQ /
Wi-Fi / multithreading refactor. Each file is a plain text dump so it is easy to
diff.

## Files

- `baseline.txt` — 60 s snapshot on `/dev/ttyUSB0` (Amped-WROVER) captured before
  the refactor started. Contains the `[Audio]` stats line during A2DP + `/api/state`
  polling from a laptop, plus one `/api/system/metrics` blob idle, BT-playing, and
  DLNA-playing.
- `streaming.txt` — Phase 7 task stats capture: `uxTaskGetSystemState` dump during
  idle, BT playback, DLNA playback, and rapid `/api/state` polling. Both "before"
  (single mutex ring, blocking `feed_mutex_`) and "after" (SPSC ring + `a2dp_pump`)
  snapshots live in this file separated by clear headers.
- `final-validation.txt` — end-of-refactor Phase 9 log capture: the full manual
  test matrix in [../manual-test-matrix.md](../manual-test-matrix.md) plus the new
  Wi-Fi onboarding + multithreading assertions.
- `bq_reference.py` — Python reference implementation of the TAS5825M Q1.31
  biquad coefficient encoding. `test/test_native/test_tas5825m_bq.cpp` cross-checks
  the firmware conversion against this file.

## Capture recipe

The recipe is scripted where practical. `task fw:monitor` writes the UART log to
`stdout`, so a 60 s capture is:

```bash
task fw:upload && task fw:web && timeout 60 task fw:monitor \
  > docs/perf/baseline.txt 2>&1
```

The `/api/state` polling side is done from the developer laptop while BT / DLNA
is playing. `curl -sS http://<device>/api/system/metrics | jq .` is captured
three times per playback state and pasted at the bottom of the file.

## Assertion targets (Phase 7)

- `/api/state` P95 during BT + DLNA playback < 500 ms (was ~1–3 s pre-refactor).
- 0 `BT_APPL: Pkt dropped` warnings during a 5-minute BT playback with `/dsp`
  page open.
- No audio underruns in the `[Audio]` stats line while streaming.
- `[Audio] dma_wait=%` stays roughly in the 60–80 % range (blocked on DMA
  pacing is the desired state; a lower value means the ring is draining faster
  than the DMA can consume, i.e. the task is spin-yielding).

## Notes

Runs marked `(build-only)` did not touch hardware — they are host-side compile
gates for the Amped-S3, Louder-WROVER, and Louder-S3 PIO envs. On-device
validation for those three boards is a Phase 10 follow-up.

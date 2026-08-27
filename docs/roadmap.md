# Post-refactor roadmap

Deferred items after the 1-layer PEQ / 4-board / Wi-Fi / threading
refactor. None of these block the current firmware.

## EQ / DSP

- [ ] Per-channel (L/R) EQ: expose `eq.channel_mode` and store two 15-band curves.
- [ ] TAS5825M DRC: `GET/POST /api/drc` wrapping sonocotta DRC registers.
- [ ] ISO 226 loudness contour toggle on top of the user PEQ.
- [ ] Hardware EQ magnitude readback so the SPA shows the as-applied curve.
- [ ] Documented procedure to bump the pinned sonocotta commit.

## Board / HAL

- [ ] On-device validation of Amped-S3, Louder-WROVER, Louder-S3 (only Amped-WROVER was validated on `/dev/ttyUSB0`).
- [ ] 16 MB S3 partition/OTA layout.
- [ ] Periodic `TAS5825M_ADDR_FAULT` poll surfaced as `state.dac.fault`.
- [ ] Shared `i2c_master_bus_handle_t` lock if a second I2C device is added.
- [ ] Amped Speakers / Headphones / Both routing — stock boards share one
  analog path and one mute GPIO; no independent mux.

## Wi-Fi

- [ ] STA-mode `/api/wifi/validate` drops the HTTP client (STA must leave the AP to probe). Return 202 + poll `/api/wifi/state`. After a probe, large `/api/state` bodies can stall until reboot; small endpoints keep working.
- [ ] Finer WPA3-SAE reason codes (today they collapse to `AUTH_FAIL`).
- [ ] Concurrent captive DNS + mDNS on the AP.

## Multithreading / perf

- [x] Nav-induced SSE teardown starved `a2dp_pump` (httpd prio 10 > pump
  prio 6 → StreamBuffer full / `BT_APPL: Pkt dropped`). Fixed: SPA defers
  EventSource close ~2 s across route changes; pump is prio 12.
- [ ] Central `src/task_map.h` with boot-time core/priority assertions.
- [ ] Revisit httpd stack in internal RAM vs PSRAM under contention.
- [ ] Snapclient buffer slider in the SPA.
- [ ] `WIFI_PS_MIN_MODEM` only while the onboarding AP is up.
- [ ] Host-side sanitizer stress test for the SPSC ring.

## Testing / CI

- [ ] Native unit tests for the Wi-Fi validate/commit FSM (mock `esp_wifi_*`).
- [ ] `pio check` / clang-tidy in Taskfile.
- [ ] Hardware-in-the-loop harness for [manual-test-matrix.md](manual-test-matrix.md).
- [ ] CI job that builds all four PIO envs on every PR.

## SPA / UX

- [ ] Undo/redo for EQ edits.
- [ ] Copy-to-other-channel once per-channel EQ exists.
- [ ] Global on-device preset library (not only NVS slot + localStorage).
- [ ] Keyboard / screen-reader pass on the SVG curve.

## Housekeeping

- [ ] Keep [licensing.md](licensing.md) third-party inventory current.
- [ ] Semver policy for the firmware vs SPA now that `/api/eq` is a breaking shape.
- [ ] Release-notes template that diffs `docs/perf/*.txt`.

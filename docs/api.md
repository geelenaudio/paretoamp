# REST API reference

Base URL: `http://<device-ip>/` or `http://192.168.4.1/` when connected to the Geelen-Audio AP.

## Endpoints

### GET /api/status

Returns current status.

**Response (JSON):**
- `volume` (0–100)
- `source` (0 = Bluetooth, 1 = Network)
- `preset` (preset id)

### GET /api/volume?v=\<0-100\>

Get or set volume. If `v` is present, sets volume and returns new value.

**Response:** `{ "volume": number }`

### POST /api/source

Set audio source.

**Body (form or query):** `s=0` (Bluetooth) or `s=1` (Network)

**Response:** `{ "source": 0|1 }`

### GET /api/eq

Get the active PEQ curve. Band count is clamped to `max_bands` (10 on
Amped software PEQ, 15 on Louder TAS5825M hardware PEQ).

**Response:** `{ enabled, band_count, max_bands, backend, bands[], headroom, active_slot }`

### GET /api/snapcast

Get Snapcast server config.

**Response:** `{ "host": string, "port": number }`

### POST /api/wifi/validate

JSON `{ssid, pass}`. Does not persist. Returns `{ok, reason, detail, ip?}`.
Reasons: `AUTH_FAIL`, `NO_AP_FOUND`, `ASSOC_FAIL`, `TIMEOUT`, `FAIL`.

### POST /api/wifi/commit

Persists the last successful validate and reboots. 400 if none.

### POST /api/wifi

Form-encoded `ssid=&pass=`. Now probes first; only reboots on success.

### GET /api/system/summary

Small metrics blob for the overview card.

### GET /api/system/tasks

`uxTaskGetSystemState` dump (name, prio, stack HWM, runtime).

### POST /api/led

WS2812 colour and brightness. Takes over from the boot/pairing animation.

**Body:** `{ "color": "#rrggbb", "brightness": 0-100 }` (omitted fields keep the current value).

**Response:** `{ present, count, phase, color, brightness }`. `count` is compile-time `HAL_WS2812_COUNT` (not writable). 404 if this image has no WS2812.

Also exposed on `GET /api/state` as `led`.

### POST /api/factory_reset

Reset all config to defaults and restart the device.

**Response:** `{ "ok": true }` (then device restarts)

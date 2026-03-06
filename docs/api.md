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

Get 10-band EQ gains (dB, -12 to +12).

**Response:** `{ "bands": [ number, ... ] }` (10 elements)

### GET /api/snapcast

Get Snapcast server config.

**Response:** `{ "host": string, "port": number }`

### POST /api/wifi

Save WiFi credentials (used when in AP mode or from Web UI).

**Body (application/x-www-form-urlencoded):** `ssid=...&pass=...`

**Response:** `{ "ok": true }`

### POST /api/factory_reset

Reset all config to defaults and restart the device.

**Response:** `{ "ok": true }` (then device restarts)

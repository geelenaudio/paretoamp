# Device identity

Friendly name, DHCP/mDNS hostname, and optional Wi-Fi / Bluetooth MAC
overrides. All three are stored in NVS (`geelen` namespace) and exposed on
the SPA About page, `GET/POST /api/identity`, and `POST /api/device-name`.

## Fields

**Device name** (`dev_name`, max 31 UTF-8 bytes plus NUL). Default
`Geelen Audio`. Shown in the SPA, Bluetooth GAP advertising, DLNA friendly
name, and the mDNS **instance**. Changing it applies live: GAP name,
mDNS instance, and the SSE state snapshot update without a reboot. Phones
may keep the old Bluetooth name until they re-pair.

**Hostname** (`hostname`). DNS LDH label: `[a-z0-9-]{1,31}`, no leading or
trailing hyphen. Used as the STA DHCP hostname and the mDNS A record, so
the SPA is at `http://<hostname>.local/` on clients that speak mDNS.
If the NVS value is empty, firmware slugs the device name (lowercase,
spaces to `-`, other punctuation stripped, fallback `geelen-audio`).
`POST /api/identity` with `hostname: null` or `""` clears the override
so later renames of the friendly name re-derive the slug.

**STA / BT MAC**. Optional 6-byte blobs. Absent means factory (eFuse).
Custom values must be unicast (not multicast, not all-zero / all-ones);
firmware forces the locally-administered bit (`mac[0] |= 0x02`). STA and
BT must differ. SoftAP MAC stays IDF-derived from STA. The captive AP
SSID stays `Geelen-Audio` (not renamed).

MAC changes are applied with `esp_iface_mac_addr_set` **before** Wi-Fi
and Bluetooth init, so they require a reboot. `POST /api/identity`
returns `reboot: true` and restarts. Reverting is `sta_mac` / `bt_mac`
set to JSON `null`. Factory reset clears overrides with the rest of NVS.

Changing the BT MAC (or the GAP name, for some phones) requires
re-pairing. Changing the STA MAC usually gets a new DHCP lease.

## mDNS

The vendored ESP-IDF `mdns` component is a **responder only**: one
`_http._tcp` service on port 80, started on `EV_WIFI_UP` (STA has an IP)
and stopped on Wi-Fi down or AP fallback. It does not run on the captive
onboarding AP (that would fight captive DNS). Live hostname / instance
renames call `mdns_hostname_set` / `mdns_instance_name_set`.

Task and allocations are in PSRAM (`CONFIG_MDNS_TASK_CREATE_FROM_SPIRAM`,
`CONFIG_MDNS_MEMORY_ALLOC_SPIRAM`), priority 1, Core 0, max 2 services.
That keeps internal DRAM for A2DP + httpd. There is no querier and no
console CLI.

## API

- `POST /api/device-name` `{ "name": "Kitchen" }` — friendly name only, live.
- `GET /api/identity` — name, hostname, `hostname_custom`, mDNS URL, MAC
  current/factory/overridden for STA, AP, BT.
- `POST /api/identity` — any of `name`, `hostname`, `sta_mac`, `bt_mac`.
- `GET /api/state` includes `identity` without MAC fields (SSE cadence).
- `GET /api/system/identity` includes the full `identity` object plus the
  legacy `net.sta_mac` / `net.bt_mac` strings.

Invalid hostname or MAC returns 400 and does not reboot.

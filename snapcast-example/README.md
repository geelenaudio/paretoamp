# Snapcast example (Spotify → Snapserver → ESP32)

A minimal Docker setup that turns any Linux box into a Snapcast server that the
Pareto Amp (Network / Snapcast source) can stream from. Uses the
self-contained [`sweisgerber/snapcast`](https://hub.docker.com/r/sweisgerber/snapcast)
image, which already bundles `snapserver`, `snapweb`, `librespot`, and
`shairport-sync` in one container — no extra sidecar needed.

## Prereqs

- Docker Engine + Docker Compose v2 (`docker compose version` prints ≥ 2.x).
- Your Linux box and the ESP32 on the same LAN subnet.
- Spotify account (Premium is not required anymore but gives better stability).

## Files

- [`docker-compose.yml`](docker-compose.yml) — one `snapcast` service on host
  networking.
- [`snapserver.conf`](snapserver.conf) — overrides the image default so the
  stream is pinned to **44100:16:2** (matches the ESP32 I2S rate) and enables
  Spotify Connect via the bundled librespot.

Ports bound on the host (via `network_mode: host`):

| Port   | Proto | Purpose                                                    |
|--------|-------|------------------------------------------------------------|
| `1704` | TCP   | **Audio stream — ESP32 stores this as `snap_port`**        |
| `1705` | TCP   | JSON-RPC control (used by Snapweb / CLI controllers)       |
| `1780` | TCP   | Snapweb UI — open in a browser                             |
|`22382` | TCP   | librespot zeroconf advertisement (internal)                |

## Bring it up

```bash
cd snapcast-example
docker compose up -d
docker compose logs -f snapcast
```

A healthy run prints (abbreviated):

```
(Snapserver) Snapserver v0.35.0
(Snapserver) Stream: LibreSpot
(librespot) Using libmdns for discovery
(librespot) Zeroconf server: listening on port 22382
```

Open `http://<host-ip>:1780/` to see the snapweb UI and confirm the `LibreSpot`
stream is in the "Streams" list.

## Point the ESP32 at it

1. Open the device Web UI (`http://<device-ip>/`).
2. In the **Snapcast** section, set `host = <host LAN IP>` and `port = 1704`
   (the Snapcast **stream** port — do not use 1705, that's the JSON-RPC
   control channel and a Snapclient on it will connect but never receive
   audio), then click **Save**. Do **not** use `127.0.0.1` or a docker
   bridge IP — the ESP32 needs to be able to route to it.
3. Click **Network** under **Source**. The device reboots once (expected: the
   BT stack is torn down). After the reboot, the serial monitor should print:

   ```
   I (XXXX) geelen: Snapclient loop starting -> <host>:1704
   I (XXXX) Snap: Connected to snapserver <host>:1704 (Hello sent)
   ```

4. In the Spotify app on your phone, pick **LibreSpot** as the Connect target
   and start playing. Audio comes out of the ESP32 amp through Snapcast.

## Known Spotify gotcha: `error audio key 0 1`

If the container log looks healthy up to and including:

```
(librespot_connect::spirc)   active device is <...>
(librespot_playback::player) Loading <...> with Spotify URI <spotify:track:...>
```

and then you see:

```
(librespot_core::audio_key)  error audio key 0 1
(librespot_playback::player) Unable to load key, continuing without decryption: Service unavailable { audio key error }
(librespot_playback::player) Unable to read audio file: Symphonia Decoder Error: end of stream
```

...that is **Spotify's backend refusing to issue the AES track key** to your
account over the legacy Mercury channel. It is the same server-side policy
change that made us drop the on-device `cspot` integration. There is no
client-side workaround in librespot 0.8.0 — snapserver's `LibreSpot` stream
will stay idle and the ESP32 will play silence.

Three options when you hit this:

1. **Use an older Spotify account** (family member, secondary login). Accounts
   created before Spotify rolled out the new key policy usually still decrypt
   through librespot. This is a lottery but fast to try.
2. **Switch to AirPlay.** The `sweisgerber/snapcast` image also bundles
   `shairport-sync`. Flip `START_AIRPLAY=true` in
   [`docker-compose.yml`](docker-compose.yml), `docker compose up -d`, and
   pick `snapcast` as an AirPlay target from any Apple device. Spotify on
   iOS/macOS can cast over AirPlay, bypassing librespot entirely.
3. **Feed a local pipe instead of Spotify.** See *Verifying without Spotify*
   below — any raw PCM source (ffmpeg of a local file, `parec` from
   PulseAudio, ALSA loopback of desktop audio) plays out of the ESP32
   unaltered.

## Verifying without Spotify

If you just want to prove the pipeline works before dragging Spotify into it,
add a second stream fed from a FIFO and point your ESP32 at that stream:

1. Add to [`snapserver.conf`](snapserver.conf) under `[stream]`:

   ```ini
   source = pipe:////data/testfifo?name=Test&sampleformat=44100:16:2&codec=pcm&mode=create
   ```

2. `docker compose restart snapcast` and pipe a raw S16LE 44.1 kHz stereo
   stream into `./data/testfifo` on the host:

   ```bash
   ffmpeg -re \
     -i https://www2.iis.fraunhofer.de/AAC/ChID-BLITS-EBU-Narration441-16b.wav \
     -f s16le -ar 44100 -ac 2 data/testfifo
   ```

3. In snapweb, select the `Test` stream for the connected client.

Audio out of the amp confirms everything downstream of Spotify (snapserver →
WiFi → ESP32 Snapclient → I2S → amp) is healthy.

## Troubleshooting, in likely order

- **Device reboots into Network source but no `Starting Snapclient` log** →
  `snap_host` isn't persisted. `GET http://<device-ip>/api/status` should echo
  the host you saved; if it doesn't, the Snapcast form didn't land in NVS.
- **`Starting Snapclient` logs but no audio** → from the host,
  `ss -lntu | grep 170[45]` must show both ports bound on `0.0.0.0`, not just
  `127.0.0.1`. If only loopback is bound, `network_mode: host` got dropped.
  Also verify the container log shows `Stream: LibreSpot` (not an error).
- **Host firewall drops the ESP32** → Ubuntu's `ufw` default-denies inbound
  from the LAN:

  ```bash
  sudo ufw allow from <esp32-ip> to any port 1704:1705 proto tcp
  ```

- **Spotify app can't see `LibreSpot`** → the container needs outbound Internet
  and mDNS reachable on your network (most home routers are fine; guest VLANs
  or `avahi-daemon` running on the host can conflict with the container's
  mDNS). Check `docker compose logs snapcast | grep -i zeroconf`.
- **`(Avahi) Failed to create client: Daemon not running` in the log** →
  harmless; the container doesn't run avahi-daemon and librespot falls back
  to its bundled `libmdns` responder on port 22382. If your phone does see
  `LibreSpot`, zeroconf is working.
- **`Loading <track>` then `error audio key 0 1`** → see *Known Spotify
  gotcha* above. Not a container / network / config problem; it is a
  server-side block on your Spotify account.
- **Choppy playback** → raise `buffer` in [`snapserver.conf`](snapserver.conf);
  2000 ms is a safe upper bound. The firmware keeps an ~2 s PSRAM ring
  (`buf=.../88200 frames`), so server-side buffer is usually the right knob.
- **Audible echo / "knistering" that decays away** → you have a **Snapweb tab
  open** (`http://localhost:1780`) while also capturing desktop audio into the
  FIFO. The browser tab is a full Snapcast client: it decodes the stream and
  plays it back out of your **default audio sink**, which is the same sink
  `pw-record` is monitoring, creating a feedback loop. Either close the
  Snapweb tab, mute the `Snapweb client` in the snapweb UI, or mute it from
  the host:

  ```bash
  CID=$(echo '{"id":1,"jsonrpc":"2.0","method":"Server.GetStatus"}' \
        | nc -w1 127.0.0.1 1705 \
        | python3 -c 'import json,sys; d=json.load(sys.stdin); \
print([c["id"] for g in d["result"]["server"]["groups"] \
for c in g["clients"] if c["host"]["name"]=="Snapweb client"][0])')
  printf '{"id":1,"jsonrpc":"2.0","method":"Client.SetVolume","params":{"id":"%s","volume":{"muted":true,"percent":0}}}\r\n' "$CID" \
    | nc -w1 127.0.0.1 1705
  ```

- **Image pull says `denied` / `not found`** → make sure the image path has
  `docker.io/` (Docker Hub) in front, not `ghcr.io/` (GitHub Container
  Registry). The docker-compose here already does.

## Tear down

```bash
docker compose down          # stop and remove the container
rm -rf data                  # also nuke librespot + snapserver state
```

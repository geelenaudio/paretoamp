# User guide

## First-time setup

1. Power the device. If no WiFi is configured, it will start an AP **Geelen-Audio** (password: **geelen123**).
2. Connect your phone/PC to **Geelen-Audio**.
3. Open a browser; you may be redirected to the setup page, or go to **http://192.168.4.1**.
4. Enter your home WiFi SSID and password and click **Save**. Power cycle the device so it connects to your network.

## WiFi

- **Station mode:** If credentials are saved, the device connects to your WiFi on boot. Find its IP (serial message or router DHCP list) and open **http://\<device-ip\>/**.
- **AP fallback:** If the saved WiFi is unavailable, the device starts the **Geelen-Audio** AP. Connect to it and open **http://192.168.4.1** to reconfigure WiFi (WiFi form on the main page).

## Bluetooth

- Default source is **Bluetooth**. The device advertises as **Geelen Audio**. Pair and connect from your phone; audio streams to the amp through the 10-band EQ.
- Last paired device can be re-paired on next power-up (stored in config).

## Spotify via WiFi (Network source)

1. On a Raspberry Pi or PC, install **snapserver** and a Spotify source (e.g. **librespot**). Configure snapserver to stream from librespot.
2. In the Web UI, set **Source** to **Network** and set **Snapcast host** to your snapserver IP (and port, default 1705).
3. Reboot or ensure the device has WiFi and the correct source; it will connect to snapserver and play the stream. From the Spotify app, choose the librespot/snapserver output to play to this speaker.

## Web UI

- **Dashboard:** Volume (0–100), source (Bluetooth / Network), quick buttons.
- **EQ:** Use `/api/eq` to read/write the 10 bands (see [API](api.md)).
- **WiFi:** SSID/password form when connected to the device (STA or AP).
- **Factory reset:** POST to `/api/factory_reset` resets config and restarts the device.

## EQ presets

- Fixed presets: Flat, Bass Boost, Vocals, Treble Clarity.
- Custom slots can be saved and named (via config/API). All settings are stored in flash and restored on boot.

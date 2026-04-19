// Thin JSON wrappers around the firmware REST API. Every call returns a
// resolved JSON body or throws; component code handles the throw via the toast
// system. Phase 1 fleshes these endpoints out — for now, the legacy Phase-0
// endpoints (`/api/status`, `/api/volume?v=`, `/api/source?s=`, `/api/wifi`,
// `/api/snapcast`) are what's wired, and the compatibility wrappers below map
// the new SPA shape onto them until Phase 1 ships.

async function j(method, url, body) {
  const init = { method, headers: {} };
  if (body !== undefined) {
    init.headers["Content-Type"] = "application/json";
    init.body = JSON.stringify(body);
  }
  const res = await fetch(url, init);
  const text = await res.text();
  let json = null;
  if (text) {
    try { json = JSON.parse(text); } catch { /* non-JSON fallback */ }
  }
  if (!res.ok) {
    const msg = (json && (json.error || json.message)) || text || res.statusText;
    throw new Error(`${res.status} ${res.statusText}: ${msg}`);
  }
  return json ?? {};
}

export const api = {
  get: (url) => j("GET", url),
  post: (url, body) => j("POST", url, body ?? {}),
  delete: (url) => j("DELETE", url),

  // Convenience wrappers (stable SPA surface — implementations may swap
  // Phase 0 compat endpoints for Phase 1 JSON endpoints without page churn).

  async getState() {
    // Phase 1 replaces this with /api/state. For now, synthesize a view over
    // /api/status so the SPA can render something real during Phase 0.
    try {
      const s = await j("GET", "/api/state");
      return s;
    } catch (_) {
      const legacy = await j("GET", "/api/status");
      return {
        mode: legacy.source === 0 ? "bluetooth" : "snapclient",
        volume: { l: legacy.volume ?? 80, r: legacy.volume ?? 80, muted: false },
        channels: { stereo_mono: "stereo", l_on: true, r_on: true },
        snapcast: { host: legacy.snap_host || "", port: legacy.snap_port || 1704 },
        device_name: "Geelen Audio",
        firmware: "dev",
        _legacy: true,
      };
    }
  },

  async getSystem() {
    try { return await j("GET", "/api/system"); }
    catch (_) { return { _unimplemented: true }; }
  },

  async setVolume(percentL, percentR, muted) {
    try {
      return await j("POST", "/api/volume", { l: percentL, r: percentR, muted: !!muted });
    } catch (_) {
      // Phase 0 fallback: single scalar `v=`.
      const v = Math.round((percentL + percentR) / 2);
      return await j("GET", `/api/volume?v=${v}`);
    }
  },

  async setMode(mode) {
    try {
      return await j("POST", "/api/mode", { mode });
    } catch (_) {
      const s = mode === "bluetooth" ? 0 : 1;
      return await j("POST", `/api/source?s=${s}`);
    }
  },

  async setSnapcast(host, port) {
    return await j("POST", "/api/snapcast", { host, port });
  },

  async setChannels(patch) {
    return await j("POST", "/api/channels", patch);
  },

  async setDlna(patch) {
    return await j("POST", "/api/dlna", patch);
  },

  async setDeviceName(name) {
    return await j("POST", "/api/device-name", { name });
  },

  async setEqLayer(patch) {
    return await j("POST", "/api/eq", patch);
  },

  async setHeadroom(patch) {
    return await j("POST", "/api/eq/headroom", patch);
  },

  async getPresets() { return await j("GET", "/api/eq/presets"); },
  async savePreset(idx, name) { return await j("POST", `/api/eq/presets/${idx}/save`, { name }); },
  async loadPreset(idx)       { return await j("POST", `/api/eq/presets/${idx}/load`); },
  async renamePreset(idx, name) { return await j("POST", `/api/eq/presets/${idx}/rename`, { name }); },
  async deletePreset(idx)     { return await j("DELETE", `/api/eq/presets/${idx}`); },

  async reset(scope) { return await j("POST", "/api/reset", { scope }); },

  async setWifi(ssid, pass) {
    const body = `ssid=${encodeURIComponent(ssid)}&pass=${encodeURIComponent(pass)}`;
    const res = await fetch("/api/wifi", {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body,
    });
    if (!res.ok) throw new Error(res.statusText);
    return res.json();
  },
};

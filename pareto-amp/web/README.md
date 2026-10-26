# Web UI

Source tree for the SPA served from SPIFFS. No build step; the assets here are
the assets that run, only gzipped on the way into the SPIFFS image.

## Layout

```
web/
├── index.html       — shell page, loaded once
├── app.css          — design tokens + base styles
├── app.js           — SPA bootstrap (mounts router + page switcher)
├── router.js        — 40-line hash router
├── api.js           — thin fetch wrappers with legacy fallbacks
├── vendor/
│   └── htm-preact.mjs   — Preact + HTM standalone bundle (~13 kB / ~6 kB gz)
├── pages/
│   ├── overview.js
│   ├── mode.js
│   ├── audio.js
│   ├── dsp.js
│   ├── reset.js
│   ├── logs.js
│   └── about.js
├── components/      — shared UI bits (populated in later phases)
├── api/             — OpenAPI spec (added in Phase 1b)
└── docs/            — Swagger UI bundle (added in Phase 1b)
```

Each page is a Preact component default-exported from its file. The shell
dynamic-`import()`s the page on navigation, so the first paint only pulls
`index.html`, `app.css`, `app.js`, `router.js`, `api.js`, and
`vendor/htm-preact.mjs`. Clicking a nav item streams in that page's module.

## Building & flashing

The firmware looks for assets at `/spiffs/web/<path>.gz` (and falls back to the
plain file if a `.gz` isn't present). `scripts/build_web.py` is wired into
`platformio.ini` as a pre-build hook and:

1. Walks `web/**`
2. Gzip-level-9 compresses each text asset into `data/web/<same-path>.gz`
3. Copies binary assets verbatim
4. Prunes orphaned files in `data/web/` whose source has been removed

After the hook runs, build and flash the filesystem:

```
pio run -t buildfs -e amped-esp32-idf     # produce the SPIFFS image
pio run -t uploadfs -e amped-esp32-idf    # flash SPIFFS only (keeps app)
```

If you change SPA code, run `uploadfs` again — the SPIFFS partition and the
app partition are independent, so you don't need to reflash firmware just to
iterate on the UI.

## Local preview (optional)

The SPA talks to `/api/*` on the device. For rapid local iteration you can run
a plain HTTP server in `web/` and point it at the device's IP via a proxy:

```
cd web
python3 -m http.server 8000
# browse http://localhost:8000 — everything renders but API calls 404 until
# you route them to the device.
```

A fuller local-dev setup (with an API proxy) is intentionally out of scope;
for now, `pio run -t uploadfs` is fast enough for the tight loop.

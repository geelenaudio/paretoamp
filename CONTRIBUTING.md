# Contributing

Thanks for helping with Pareto Amp. The firmware is
[GPL-3.0-or-later](COPYING). Issues and pull requests on
[geelenaudio/paretoamp](https://github.com/geelenaudio/paretoamp) are
welcome.

You can also just **use** a release binary — no clone required:
[Using the firmware](docs/using-firmware.md).

## Issues

Open an issue when something is wrong, unclear, or missing. Search
[existing issues](https://github.com/geelenaudio/paretoamp/issues) first.

Use the **bug** template for defects and the **feature** template for
new behaviour. A useful report includes:

- **Board** — Amped-ESP32, Amped-ESP32-S3, Louder-ESP32 / Plus, or
  Louder-ESP32-S3.
- **Firmware version** — About on the Web UI, or the GitHub Release tag
  you flashed (`v0.0.1`, …).
- **What you did**, **what you expected**, **what happened**.
- **Logs** — Web UI → Logs, or a serial capture at 115200. Cut the
  relevant lines; a full boot is fine for bring-up failures.

Hardware questions that need a pinout: link the board page under
[docs/hardware](docs/hardware/README.md).

Please do not attach secrets (Wi-Fi passwords, tokens). Factory images
wipe NVS; do not paste live credentials from a backup.

## Pull requests

1. Fork the repo (or push a branch if you have write access).
2. Branch from current `main`.
3. Keep the change focused. Note the board(s) you flashed or why a
   native-only change is enough.
4. Use [Conventional Commits](https://www.conventionalcommits.org/) —
   they drive the next SemVer tag:

   | Prefix | Effect |
   | ------ | ------ |
   | `fix:` | patch release |
   | `feat:` | minor release |
   | `BREAKING CHANGE:` in the footer | major release |
   | `docs:`, `chore:`, `style:`, `test:` | no release |

5. Open a PR against `main`. CI (`.github/workflows/ci.yml`) must pass:
   native tests plus factory images for all four boards.

Do not push `vMAJOR.MINOR.PATCH` tags and do not bump
`pareto-amp/version.txt` by hand. semantic-release does both on `main`
after a `feat:` / `fix:` / breaking commit. Details:
[Development — versioning](docs/development.md#versioning-and-releases).

Please do not rename the captive SSID (`Geelen-Audio`), the factory
mDNS hostname (`geelen-audio`), or the C++ `geelen::` namespaces — those
are the product identity, not leftovers.

## Building a change

From the repo root (directory with `Taskfile.yml`):

```sh
task fw:amped        # Amped-WROVER
task fw:louder       # Louder-WROVER + TAS58xx
task fw:amped-s3
task fw:louder-s3
```

Host tests: `cd pareto-amp && pio test -e native`. Toolchain pins and
module map: [Development](docs/development.md).

## Licence of contributions

By opening a PR you offer the change under the same GPL-3.0-or-later
terms as the rest of the tree ([docs/licensing.md](docs/licensing.md)).

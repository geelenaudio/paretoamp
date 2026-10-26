#!/usr/bin/env python3
"""Merge bootloader + partition table + app + SPIFFS into one 0x0 factory image.

PlatformIO/IDF already built:
  .pio/build/<env>/bootloader.bin
  .pio/build/<env>/partitions.bin
  .pio/build/<env>/firmware.bin
  .pio/build/<env>/spiffs.bin

Offsets come from flasher_args.json (chip-specific bootloader) plus the
spiffs row in partitions_idf.csv. Gaps (including NVS at 0x9000) are 0xFF,
so flashing the result at 0x0 is a factory write.

Usage (from pareto-amp/ after pio run + pio run -t buildfs):
  python3 scripts/merge_factory.py --env amped-esp32-idf
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

ENV_SLUG = {
    "amped-esp32-idf": "amped-esp32",
    "louder-esp32-idf": "louder-esp32",
    "amped-esp32s3-idf": "amped-esp32s3",
    "louder-esp32s3-idf": "louder-esp32s3",
}

PIO_FILES = {
    "bootloader": "bootloader.bin",
    "app": "firmware.bin",
    "partition-table": "partitions.bin",
}


def project_dir() -> Path:
    return Path(__file__).resolve().parent.parent


def read_version(root: Path) -> str:
    p = root / "version.txt"
    if not p.is_file():
        raise SystemExit("version.txt missing — stamp PROJECT_VER before merging")
    ver = p.read_text(encoding="utf-8").strip().splitlines()[0].strip()
    if ver.startswith("v"):
        ver = ver[1:]
    if not ver:
        raise SystemExit("version.txt is empty")
    return ver


def parse_spiffs_offset(csv_path: Path) -> int:
    for raw in csv_path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) < 4:
            continue
        if cols[0] == "spiffs":
            return int(cols[3], 0)
    raise SystemExit(f"no spiffs row in {csv_path}")


def resolve_bin(build: Path, key: str, idf_rel: str) -> Path:
    pio_name = PIO_FILES[key]
    for candidate in (build / pio_name, build / idf_rel):
        if candidate.is_file():
            return candidate
    raise SystemExit(f"missing {key} binary (looked for {pio_name} and {idf_rel} under {build})")


def find_esptool() -> tuple[list[str], dict[str, str]]:
    """Prefer PlatformIO's esptool v5 (`merge-bin`). Distro packages are often v4."""
    env = os.environ.copy()
    pio_py = Path.home() / ".platformio" / "penv" / "bin" / "python"
    python = str(pio_py) if pio_py.is_file() else sys.executable
    pkg = Path.home() / ".platformio" / "packages" / "tool-esptoolpy"
    if pkg.is_dir():
        env["PYTHONPATH"] = str(pkg) + os.pathsep + env.get("PYTHONPATH", "")
        return [python, "-m", "esptool"], env
    if pio_py.is_file():
        return [str(pio_py), "-m", "esptool"], env
    raise SystemExit(
        "esptool not found — install PlatformIO (tool-esptoolpy) and rerun"
    )


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--env", required=True, help="PlatformIO env (e.g. amped-esp32-idf)")
    ap.add_argument("--slug", default="", help="Artifact slug (default: derived from --env)")
    ap.add_argument("--project-dir", default="", help="pareto-amp/ (default: next to this script)")
    args = ap.parse_args()

    root = Path(args.project_dir).resolve() if args.project_dir else project_dir()
    env = args.env
    slug = args.slug or ENV_SLUG.get(env)
    if not slug:
        raise SystemExit(f"unknown env {env!r}; pass --slug")

    build = root / ".pio" / "build" / env
    args_json = build / "flasher_args.json"
    if not args_json.is_file():
        raise SystemExit(f"{args_json} missing — run: pio run -e {env}")

    spiffs = build / "spiffs.bin"
    if not spiffs.is_file():
        raise SystemExit(f"{spiffs} missing — run: pio run -e {env} -t buildfs")

    meta = json.loads(args_json.read_text(encoding="utf-8"))
    flash = meta.get("flash_settings") or {}
    chip = (meta.get("extra_esptool_args") or {}).get("chip") or "esp32"
    flash_mode = flash.get("flash_mode") or "dio"
    flash_size = flash.get("flash_size") or "8MB"
    flash_freq = flash.get("flash_freq") or "40m"

    segments: list[tuple[int, Path]] = []
    for key in ("bootloader", "partition-table", "app"):
        entry = meta.get(key) or {}
        offset_s = entry.get("offset")
        rel = entry.get("file")
        if not offset_s or not rel:
            raise SystemExit(f"flasher_args.json missing {key}")
        segments.append((int(offset_s, 0), resolve_bin(build, key, rel)))

    csv_path = root / "partitions_idf.csv"
    segments.append((parse_spiffs_offset(csv_path), spiffs))
    segments.sort(key=lambda s: s[0])

    version = read_version(root)
    dist = root / "dist"
    dist.mkdir(parents=True, exist_ok=True)
    out = dist / f"pareto-amp-{slug}-v{version}.bin"

    esptool, env = find_esptool()
    cmd = esptool + [
        "--chip",
        chip,
        "merge-bin",
        "--output",
        str(out),
        "--format",
        "raw",
        "--target-offset",
        "0",
        "--flash-mode",
        flash_mode,
        "--flash-size",
        flash_size,
        "--flash-freq",
        flash_freq,
        "--pad-to-size",
        flash_size,
    ]
    for offset, path in segments:
        cmd.extend([f"0x{offset:x}", str(path)])

    print(" ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, env=env)

    digest = sha256_file(out)
    sha_path = out.with_suffix(out.suffix + ".sha256")
    sha_path.write_text(f"{digest}  {out.name}\n", encoding="utf-8")
    print(f"factory: {out} ({out.stat().st_size} bytes)")
    print(f"sha256:  {digest}")
    print(f"flash:   esptool --chip {chip} write-flash 0x0 {out.name}")
    print("note:    factory image includes 0xFF over NVS — Wi-Fi/EQ are wiped")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as e:
        sys.exit(e.returncode)
    except KeyboardInterrupt:
        sys.exit(130)

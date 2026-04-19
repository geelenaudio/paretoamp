"""
PlatformIO pre-build hook: bundle the SPA from `web/` into `data/web/`, gzipped.

Runs on every build so the SPIFFS image always reflects the current `web/`
tree. `pio run -t buildfs` / `-t uploadfs` reads `data/`, so this script only
needs to drop files there.

Strategy:
  - Source of truth: `web/**` (text + vendored JS + images).
  - Every text asset is gzipped at compression level 9 into `data/web/<path>.gz`.
  - Binary-ish assets (png, ico, ttf, woff2) are copied as-is — they're already
    compressed and re-gzipping them wastes flash.
  - The HTTP handler on the device serves `/<path>` by looking up
    `/spiffs/web/<path>.gz` first (add `Content-Encoding: gzip`) and falls back
    to the plain file if no `.gz` exists.

The script is idempotent: if the source mtime is older than the dest, it skips
the file. That keeps incremental builds fast.
"""

from __future__ import annotations

import gzip
import os
import shutil
from pathlib import Path

try:
    Import("env")  # noqa: F821 — provided by PlatformIO at SCons parse time
    PROJECT_DIR = Path(env["PROJECT_DIR"])  # noqa: F821
except Exception:
    # Allow running standalone (`python scripts/build_web.py`) for local testing.
    PROJECT_DIR = Path(__file__).resolve().parent.parent


WEB_SRC = PROJECT_DIR / "web"
WEB_DST = PROJECT_DIR / "data" / "web"

# Extensions we gzip. Everything else is copied verbatim.
GZIP_EXTS = {
    ".html", ".css", ".js", ".mjs", ".json", ".svg", ".txt", ".yaml", ".yml",
    ".xml", ".ico",  # ICO is uncompressed bitmap, gz saves ~50%
    ".map",
}

SKIP_NAMES = {".DS_Store", "Thumbs.db", "README.md"}
SKIP_SUFFIXES = {".md"}  # developer docs never ship to SPIFFS


def _should_gzip(name: str) -> bool:
    return Path(name).suffix.lower() in GZIP_EXTS


def _needs_rebuild(src: Path, dst: Path) -> bool:
    if not dst.exists():
        return True
    return src.stat().st_mtime > dst.stat().st_mtime


def _gzip_to(src: Path, dst_gz: Path) -> None:
    dst_gz.parent.mkdir(parents=True, exist_ok=True)
    # mtime=0 so the gzipped blob is reproducible across builds.
    with src.open("rb") as fin, gzip.GzipFile(
        filename=str(dst_gz), mode="wb", compresslevel=9, mtime=0,
    ) as fout:
        shutil.copyfileobj(fin, fout)


def _copy_to(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def build_web(*_args, **_kwargs) -> None:
    if not WEB_SRC.is_dir():
        print(f"[build_web] no {WEB_SRC} directory — nothing to do")
        return

    WEB_DST.mkdir(parents=True, exist_ok=True)

    total = 0
    gzipped = 0
    copied = 0
    bytes_in = 0
    bytes_out = 0

    # Build a set of expected dest files so we can prune stale ones.
    expected: set[Path] = set()

    for src in WEB_SRC.rglob("*"):
        if src.is_dir():
            continue
        if src.name in SKIP_NAMES:
            continue
        if src.suffix.lower() in SKIP_SUFFIXES:
            continue
        rel = src.relative_to(WEB_SRC)
        total += 1

        if _should_gzip(src.name):
            dst = WEB_DST / rel.parent / (rel.name + ".gz")
            expected.add(dst)
            if _needs_rebuild(src, dst):
                _gzip_to(src, dst)
                gzipped += 1
            bytes_in += src.stat().st_size
            bytes_out += dst.stat().st_size
        else:
            dst = WEB_DST / rel
            expected.add(dst)
            if _needs_rebuild(src, dst):
                _copy_to(src, dst)
                copied += 1
            bytes_in += src.stat().st_size
            bytes_out += dst.stat().st_size

    # Prune orphaned files in data/web/ that no longer have a source.
    removed = 0
    for old in WEB_DST.rglob("*"):
        if old.is_dir():
            continue
        if old not in expected:
            old.unlink()
            removed += 1

    # Clean up empty directories left behind by pruning.
    for d in sorted((p for p in WEB_DST.rglob("*") if p.is_dir()), reverse=True):
        try:
            d.rmdir()
        except OSError:
            pass

    ratio = (bytes_out / bytes_in * 100) if bytes_in else 0
    print(
        f"[build_web] {total} files ({gzipped} gz, {copied} copied, {removed} pruned) "
        f"{bytes_in:,} B -> {bytes_out:,} B ({ratio:.1f}%)"
    )


# When PlatformIO imports this script, run it immediately so data/ is ready by
# the time the buildfs target kicks in. (buildfs itself is a separate target
# invoked via `pio run -t buildfs` or automatically by `uploadfs`.)
build_web()

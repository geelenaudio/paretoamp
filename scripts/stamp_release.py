#!/usr/bin/env python3
"""Write pareto-amp/version.txt and the README release badge.

The GitHub Releases API badge cannot see a private repo, so README uses a
static shields.io badge that we stamp on each semantic-release.
"""
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
README = ROOT / "README.md"
VERSION_TXT = ROOT / "pareto-amp" / "version.txt"
BADGE = r"(https://img\.shields\.io/badge/release-)v\d+\.\d+\.\d+(-blue)"


def main() -> int:
    if len(sys.argv) != 2 or not sys.argv[1].strip():
        print("usage: stamp_release.py <semver>", file=sys.stderr)
        return 2
    ver = sys.argv[1].strip().lstrip("v")
    VERSION_TXT.write_text(ver + "\n", encoding="utf-8")
    text = README.read_text(encoding="utf-8")
    text2, n = re.subn(BADGE, rf"\1v{ver}\2", text, count=1)
    if n != 1:
        print(f"stamp_release.py: expected 1 README badge match, got {n}", file=sys.stderr)
        return 1
    README.write_text(text2, encoding="utf-8")
    print(f"stamped {ver}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

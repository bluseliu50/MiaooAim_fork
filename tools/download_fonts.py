#!/usr/bin/env python3
"""Download font sources for bitmap font generation.

Downloads:
- GNU Unifont .hex → tools/fonts/unifont-16.0.02.hex
- Fusion Pixel 12px BDF → tools/fonts/fusion-pixel-12px-monospaced-zh_hans.bdf
"""

from __future__ import annotations

import sys
import urllib.request
from pathlib import Path

UNIFONT_URL = "https://unifoundry.com/pub/unifont/unifont-16.0.02/font-builds/unifont-16.0.02.hex.gz"
UNIFONT_HEX = "tools/fonts/unifont-16.0.02.hex"

FUSION_URL = "https://github.com/TakWolf/fusion-pixel-font/releases/download/2026.07.01/fusion-pixel-font-12px-monospaced-bdf-v2026.07.01.zip"
FUSION_BDF = "tools/fonts/fusion-pixel-12px-monospaced-zh_hans.bdf"


def download(url: str, dst: Path) -> None:
    if dst.exists():
        print(f"  already exists: {dst}")
        return
    print(f"  downloading: {url}")
    data = urllib.request.urlopen(url, timeout=60).read()
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(data)
    print(f"  saved: {dst} ({len(data)} bytes)")


def download_unifont() -> None:
    dst = Path(UNIFONT_HEX)
    if dst.exists():
        print(f"  already exists: {dst}")
        return
    print("Downloading GNU Unifont...")
    import gzip
    raw = urllib.request.urlopen(UNIFONT_URL, timeout=120).read()
    decompressed = gzip.decompress(raw)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(decompressed)
    print(f"  saved: {dst} ({len(decompressed)} bytes)")


def download_fusion() -> None:
    dst = Path(FUSION_BDF)
    if dst.exists():
        print(f"  already exists: {dst}")
        return
    print("Downloading Fusion Pixel Font 12px BDF...")
    import io, zipfile
    raw = urllib.request.urlopen(FUSION_URL, timeout=120).read()
    with zipfile.ZipFile(io.BytesIO(raw)) as zf:
        for name in zf.namelist():
            if "zh_hans" in name and name.endswith(".bdf"):
                data = zf.read(name)
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_bytes(data)
                print(f"  saved: {dst} ({len(data)} bytes)")
                return
    print("ERROR: zh_hans BDF not found in archive", file=sys.stderr)
    sys.exit(1)


def main() -> int:
    download_unifont()
    download_fusion()
    print("Font sources ready.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

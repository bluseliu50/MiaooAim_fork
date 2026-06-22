#!/usr/bin/env python3
"""Fetch OFL-licensed open fonts used to generate e-paper bitmap fonts."""

from __future__ import annotations

import argparse
import sys
import time
import urllib.request
from pathlib import Path

OFL_NOTICE = """SIL Open Font License 1.1

The generated firmware bitmap fonts are derived from open fonts.
Official license text and sources:

- https://github.com/lxgw/975maru
- https://github.com/notofonts/noto-fonts/blob/main/LICENSE
- https://openfontlicense.org/

If the build host already has these open fonts installed, this script may copy
those local font files into the configured cache directory to avoid unreliable
large downloads. Do not replace them with proprietary system fonts for public
firmware releases.
"""


FILES = {
    "LXGW975YuanSC-400W.ttf": {
        "url": "https://raw.githubusercontent.com/lxgw/975maru/26.06.20/TTF/LXGW975YuanSC-400W.ttf",
        "min_size": 13 * 1024 * 1024,
    },
    "NotoSansMono-Regular.ttf": {
        "url": "https://raw.githubusercontent.com/notofonts/noto-fonts/main/hinted/ttf/NotoSansMono/NotoSansMono-Regular.ttf",
        "min_size": 300 * 1024,
    },
    "LICENSE-Noto-Fonts.txt": {
        "url": "https://raw.githubusercontent.com/notofonts/noto-fonts/main/LICENSE",
        "min_size": 80,
    },
    "LICENSE-LXGW975Yuan.txt": {
        "url": "https://raw.githubusercontent.com/lxgw/975maru/26.06.20/OFL.txt",
        "min_size": 80,
    },
}

LOCAL_FONT_FALLBACKS = {
    "LXGW975YuanSC-400W.ttf": [
        "C:/Windows/Fonts/LXGW975YuanSC-400W.ttf",
    ],
    "NotoSansMono-Regular.ttf": [
        "C:/Windows/Fonts/NotoSansMono-Regular.ttf",
        "C:/Windows/Fonts/NotoSans-Regular.ttf",
        "C:/Windows/Fonts/consola.ttf",
    ],
    "LICENSE-Noto-Fonts.txt": [],
    "LICENSE-LXGW975Yuan.txt": [],
}


def fetch(meta: dict[str, object], dst: Path, min_size: int) -> None:
    if dst.exists() and dst.stat().st_size >= min_size:
        return
    if str(meta.get("url") or "") == "local-ofl-notice":
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(OFL_NOTICE, encoding="utf-8")
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_suffix(dst.suffix + ".tmp")
    last_error: Exception | None = None
    urls = [str(meta["url"])]
    urls.extend(str(url) for url in meta.get("fallback_urls", []) or [])
    for url in urls:
        request = urllib.request.Request(url, headers={
            "User-Agent": "epaper-uploader-font-fetch/1.0",
        })
        for attempt in range(1, 4):
            print(f"Downloading {dst.name} ({attempt}/3) ...")
            try:
                with urllib.request.urlopen(request, timeout=180) as response:
                    with tmp.open("wb") as f:
                        while True:
                            chunk = response.read(1024 * 256)
                            if not chunk:
                                break
                            f.write(chunk)
                last_error = None
                break
            except Exception as exc:
                last_error = exc
                tmp.unlink(missing_ok=True)
                time.sleep(2 * attempt)
        if last_error is None:
            break
    if last_error:
        raise last_error
    if tmp.stat().st_size < min_size:
        tmp.unlink(missing_ok=True)
        raise RuntimeError(f"{dst.name} is too small; download likely failed")
    tmp.replace(dst)


def copy_local_fallback(name: str, dst: Path, min_size: int) -> bool:
    if name.startswith("LICENSE-"):
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(OFL_NOTICE, encoding="utf-8")
        return True
    for candidate in LOCAL_FONT_FALLBACKS.get(name, []):
        src = Path(candidate)
        if src.exists() and src.stat().st_size >= min_size:
            dst.parent.mkdir(parents=True, exist_ok=True)
            print(f"Using local open font {src} -> {dst.name}")
            dst.write_bytes(src.read_bytes())
            return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", default="tools/fonts/noto")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    try:
        for name, meta in FILES.items():
            dst = out_dir / name
            try:
                fetch(meta, dst, int(meta["min_size"]))
            except Exception:
                if not copy_local_fallback(name, dst, int(meta["min_size"])):
                    raise
    except Exception as exc:
        print(f"open font download failed: {exc}", file=sys.stderr)
        return 1

    marker = out_dir / ".fonts-ready"
    marker.write_text("Open fonts downloaded. Sources are listed in tools/fetch_open_fonts.py\n",
                      encoding="utf-8")
    print(f"Open fonts ready: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

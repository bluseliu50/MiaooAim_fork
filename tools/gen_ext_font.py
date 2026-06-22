#!/usr/bin/env python3
"""Generate external MEF bitmap fonts for large e-paper UI text.

MEF2 format:
  4 bytes  magic "MEF2"
  u16le    square glyph pixel size, e.g. 48
  u16le    bytes per glyph: ((size + 7) // 8) * size
  u32le    glyph count
  repeated sorted records:
    u32le  Unicode codepoint
    u8     glyph advance in source pixels
    bytes  horizontal MSB-first 1bpp bitmap, 1 = ink
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import struct
import sys
import time
from pathlib import Path


FONT_CANDIDATES = [
    "tools/fonts/noto/LXGW975YuanSC-400W.ttf",
    "tools/fonts/noto/LXGW975YuanSC-500W.ttf",
    "tools/fonts/noto/ResourceHanRoundedCN-Medium.ttf",
    "C:/Windows/Fonts/msyhbd.ttc",
    "C:/Windows/Fonts/simhei.ttf",
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/Deng.ttf",
    "C:/Windows/Fonts/Dengl.ttf",
    "C:/Windows/Fonts/msyhl.ttc",
    "C:/Windows/Fonts/simsun.ttc",
]

ASCII_FONT_CANDIDATES = [
    "tools/fonts/noto/LXGW975YuanSC-400W.ttf",
    "tools/fonts/noto/LXGW975YuanSC-500W.ttf",
    "tools/fonts/noto/NotoSansMono-Regular.ttf",
    "C:/Windows/Fonts/comicbd.ttf",
    "C:/Windows/Fonts/segoeprb.ttf",
    "C:/Windows/Fonts/mvboli.ttf",
    "C:/Windows/Fonts/Inkfree.ttf",
    "C:/Windows/Fonts/arialbd.ttf",
]

ASCII_PRINTABLE = "".join(chr(code) for code in range(32, 127))


def load_gen_font_module():
    path = Path(__file__).with_name("gen_font.py")
    spec = importlib.util.spec_from_file_location("gen_font_source", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def ensure_pillow():
    try:
        from PIL import Image, ImageDraw, ImageFont  # noqa: F401
    except ImportError as exc:
        raise SystemExit("Pillow is required: python -m pip install Pillow") from exc


def load_extra_chars(path: Path) -> str:
    if not path.exists():
        return ""
    chars: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "#" in line:
            line = line.split("#", 1)[0].strip()
        chars.append("".join(ch for ch in line if not ch.isspace()))
    return "".join(chars)


def resolve_font_path(path: str | None) -> str | None:
    if not path:
        return None
    p = Path(path)
    if p.is_absolute():
        return str(p)
    project_dir = Path(__file__).resolve().parent.parent
    return str(project_dir / p)


def choose_font(explicit: str | None, pixel_size: int):
    from PIL import ImageFont

    candidates = [explicit] if explicit else []
    candidates.extend(FONT_CANDIDATES)
    for fp in candidates:
        fp = resolve_font_path(fp)
        if not fp:
            continue
        if os.path.exists(fp):
            try:
                return ImageFont.truetype(fp, pixel_size), fp
            except Exception:
                continue
    raise SystemExit("No usable CJK font found. Use --font C:/path/font.ttf")


def choose_ascii_font(explicit: str | None, pixel_size: int):
    from PIL import ImageFont

    candidates = [explicit] if explicit else []
    candidates.extend(ASCII_FONT_CANDIDATES)
    for fp in candidates:
        fp = resolve_font_path(fp)
        if not fp:
            continue
        if os.path.exists(fp):
            try:
                return ImageFont.truetype(fp, pixel_size), fp
            except Exception:
                continue
    return choose_font(None, pixel_size)


def build_charset(which: str) -> list[str]:
    src = load_gen_font_module()
    tools_dir = Path(__file__).resolve().parent
    extra = src.EXTRA_CHARS + load_extra_chars(tools_dir / "font_extra_chars.txt")
    if which == "level1":
        chars = ASCII_PRINTABLE + src.get_gb2312_level1() + extra
    elif which == "gb2312":
        chars = ASCII_PRINTABLE + src.get_gb2312_level1() + src.get_gb2312_level2() + extra
    else:
        chars = ASCII_PRINTABLE + extra
    return sorted(dict.fromkeys(chars), key=ord)


def glyph_advance(font, ch: str, size: int, glyph_w: int) -> int:
    cp = ord(ch)
    if cp >= 0x80:
        return size
    try:
        adv = int(round(font.getlength(ch)))
    except Exception:
        adv = glyph_w
    if glyph_w > 0:
        adv = max(adv, glyph_w + 2)
    if adv < 1:
        adv = max(1, size // 4)
    if adv > size:
        adv = size
    return adv


def pack_glyph(font, ch: str, size: int, threshold: int,
               cjk_stroke: int, cjk_expand: int) -> tuple[int, bytes]:
    from PIL import Image, ImageDraw

    canvas = Image.new("L", (size, size), 0)
    probe = ImageDraw.Draw(Image.new("L", (size * 3, size * 3), 0))
    is_cjk = ord(ch) >= 0x80
    stroke = cjk_stroke if is_cjk else 0
    bb = probe.textbbox((0, 0), ch, font=font, stroke_width=stroke)
    gw = max(0, bb[2] - bb[0])
    gh = max(0, bb[3] - bb[1])
    advance = glyph_advance(font, ch, size, gw)
    if gw == 0 or gh == 0:
        return advance, bytes(((size + 7) // 8) * size)

    glyph = Image.new("L", (gw, gh), 0)
    draw = ImageDraw.Draw(glyph)
    draw.text((-bb[0], -bb[1]), ch, font=font, fill=255,
              stroke_width=stroke, stroke_fill=255)

    max_w = max(1, size - 2)
    max_h = max(1, size - 2)
    if ord(ch) < 0x80:
        max_w = max(1, advance - 1)
    if gw > max_w or gh > max_h:
        ratio = min(max_w / gw, max_h / gh)
        nw = max(1, round(gw * ratio))
        nh = max(1, round(gh * ratio))
        glyph = glyph.resize((nw, nh), Image.Resampling.LANCZOS)
        gw, gh = glyph.size

    if ord(ch) < 0x80:
        x = 1
    else:
        x = (size - gw) // 2
    y = (size - gh) // 2
    canvas.paste(glyph, (x, y))

    if is_cjk and cjk_expand > 0:
        from PIL import ImageFilter
        for _ in range(cjk_expand):
            canvas = canvas.filter(ImageFilter.MaxFilter(3))

    stride = (size + 7) // 8
    out = bytearray(stride * size)
    pix = canvas.load()
    for yy in range(size):
        row_off = yy * stride
        for xx in range(size):
            if pix[xx, yy] >= threshold:
                out[row_off + xx // 8] |= 0x80 >> (xx % 8)
    return advance, bytes(out)


def replace_output(tmp_path: Path, out_path: Path) -> None:
    for attempt in range(6):
        try:
            if out_path.exists():
                out_path.unlink()
            tmp_path.replace(out_path)
            return
        except PermissionError:
            if attempt == 5:
                raise
            time.sleep(0.2 * (attempt + 1))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--size", type=int, choices=(16, 24, 32, 48, 64), required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--font")
    parser.add_argument("--ascii-font")
    parser.add_argument("--set", choices=("level1", "gb2312", "extra"), default="gb2312")
    parser.add_argument("--threshold", type=int, default=130)
    parser.add_argument("--cjk-stroke", type=int, default=0,
                        help="extra CJK stroke width before 1bpp packing")
    parser.add_argument("--cjk-expand", type=int, default=0,
                        help="extra 1px CJK grayscale expansion passes")
    parser.add_argument("--require-open-fonts", action="store_true")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    ensure_pillow()
    if args.require_open_fonts:
        project_dir = Path(__file__).resolve().parent.parent
        required = [
            project_dir / "tools/fonts/noto/LXGW975YuanSC-400W.ttf",
            project_dir / "tools/fonts/noto/NotoSansMono-Regular.ttf",
        ]
        missing = [str(path) for path in required if not path.exists()]
        if missing:
            raise SystemExit("Required open fonts are missing: " + ", ".join(missing))
    chars = build_charset(args.set)
    # Use a slightly smaller font size than the square cell to keep dense CJK
    # glyphs away from UI grid lines on e-paper. The 24px test size needs a
    # fuller source glyph; otherwise round corners collapse after 1bpp packing.
    if args.size <= 16:
        cjk_font_px = args.size
        ascii_font_px = args.size
    elif args.size <= 24:
        cjk_font_px = max(8, args.size - 2)
        ascii_font_px = max(8, args.size - 2)
    else:
        cjk_font_px = max(8, args.size - (6 if args.size >= 48 else 4))
        ascii_font_px = max(8, args.size - (5 if args.size >= 48 else 3))
    cjk_font, font_path = choose_font(args.font, cjk_font_px)
    ascii_font, ascii_font_path = choose_ascii_font(args.ascii_font,
                                                    ascii_font_px)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    glyph_bytes = ((args.size + 7) // 8) * args.size
    tmp_path = out_path.with_suffix(out_path.suffix + ".tmp")
    with tmp_path.open("wb") as f:
        f.write(b"MEF2")
        f.write(struct.pack("<HHI", args.size, glyph_bytes, len(chars)))
        for ch in chars:
            f.write(struct.pack("<I", ord(ch)))
            font = ascii_font if ord(ch) < 0x80 else cjk_font
            advance, bitmap = pack_glyph(font, ch, args.size, args.threshold,
                                         args.cjk_stroke, args.cjk_expand)
            f.write(struct.pack("<B", advance))
            f.write(bitmap)
    replace_output(tmp_path, out_path)

    if not args.quiet:
        print(f"Generated {out_path}")
        print(f"  CJK source: {font_path}")
        print(f"  ASCII source: {ascii_font_path}")
        print(f"  size: {args.size}px, glyphs: {len(chars)}, bytes: {out_path.stat().st_size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

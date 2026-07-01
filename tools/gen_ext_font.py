#!/usr/bin/env python3
"""Generate external MEF bitmap fonts for e-paper UI text.

Uses two native bitmap font sources (no Pillow rasterization needed):
- Unifont .hex (16px native, ×2 for 32px) for cjk16.mef and cjk32.mef
- Fusion Pixel 12px .bdf (×2 for 24px) for cjk24.mef

MEF2 format:
  4 bytes  magic "MEF2"
  u16le    square glyph pixel size
  u16le    bytes per glyph: ((size + 7) // 8) * size
  u32le    glyph count
  repeated sorted records:
    u32le  Unicode codepoint
    u8     glyph advance in source pixels (CJK=size, ASCII=size//2)
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

ASCII_PRINTABLE = "".join(chr(code) for code in range(32, 127))

UNIFONT_HEX = str(Path(__file__).resolve().parent.parent / "tools/fonts/unifont-16.0.02.hex")
FUSION_BDF = str(Path(__file__).resolve().parent.parent / "tools/fonts/fusion-pixel-12px-monospaced-zh_hans.bdf")


def load_gen_font_module():
    path = Path(__file__).with_name("gen_font.py")
    spec = importlib.util.spec_from_file_location("gen_font_source", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


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


# ── Unifont .hex parser ──────────────────────────────────────────────────

def load_unifont_hex(path: str, needed_cps: set[int]) -> dict[int, list[int]]:
    """Parse Unifont .hex file. Returns {codepoint: 32 bytes (16x16 1bpp)}.
    Only loads codepoints in needed_cps."""
    glyphs: dict[int, list[int]] = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or ":" not in line:
                continue
            cp_str, hex_str = line.split(":", 1)
            cp = int(cp_str, 16)
            if cp not in needed_cps:
                continue
            raw = bytes.fromhex(hex_str)
            if len(raw) == 32:
                glyphs[cp] = list(raw)
            elif len(raw) == 16:
                # Narrow glyph (8px wide) — expand to 16px wide
                expanded = []
                for i in range(16):
                    b = raw[i] if i < len(raw) else 0
                    expanded.append(b)
                    expanded.append(0)
                glyphs[cp] = expanded
    return glyphs


def pack_unifont_glyph(raw: list[int], target_size: int, is_cjk: bool) -> tuple[int, bytes]:
    """Pack a 16px Unifont glyph into target_size cell.
    16px = native 1:1. 32px = 2× NEAREST upscale (fills cell completely)."""
    advance = target_size if is_cjk else (target_size // 2)
    scale = target_size // 16

    stride = (target_size + 7) // 8
    out = bytearray(stride * target_size)

    for ty in range(target_size):
        sy = ty // scale
        if sy >= 16:
            continue
        src_byte_lo = raw[sy * 2]
        src_byte_hi = raw[sy * 2 + 1]
        for tx in range(target_size):
            sx = tx // scale
            if sx < 8:
                bit = src_byte_lo & (0x80 >> sx)
            else:
                bit = src_byte_hi & (0x80 >> (sx - 8))
            if bit:
                out[ty * stride + tx // 8] |= 0x80 >> (tx % 8)

    return advance, bytes(out)


# ── Fusion Pixel BDF parser ──────────────────────────────────────────────

def load_fusion_bdf(path: str, needed_cps: set[int]) -> dict[int, dict]:
    """Parse Fusion Pixel 12px monospaced BDF. Returns {codepoint: glyph_dict}.
    glyph_dict = {'dwidth': int, 'bbx': (w,h,xoff,yoff), 'bitmap': [hex_rows]}"""
    glyphs: dict[int, dict] = {}
    with open(path, "r", encoding="utf-8") as f:
        in_glyph = False
        cur_cp = -1
        cur_dwidth = 12
        cur_bbx = None
        cur_bitmap: list[str] = []

        for line in f:
            line = line.strip()
            if line.startswith("STARTCHAR"):
                in_glyph = True
                cur_cp = -1
                cur_dwidth = 12
                cur_bbx = None
                cur_bitmap = []
            elif line.startswith("ENCODING"):
                cur_cp = int(line.split()[1])
            elif line.startswith("DWIDTH"):
                parts = line.split()
                cur_dwidth = int(parts[1])
            elif line.startswith("BBX"):
                parts = line.split()
                cur_bbx = (int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]))
            elif line == "BITMAP":
                pass
            elif line == "ENDCHAR":
                if cur_cp >= 0 and cur_cp in needed_cps and cur_bbx:
                    glyphs[cur_cp] = {
                        "dwidth": cur_dwidth,
                        "bbx": cur_bbx,
                        "bitmap": cur_bitmap[:],
                    }
                in_glyph = False
            elif in_glyph and cur_bbx:
                cur_bitmap.append(line)
    return glyphs


def pack_fusion_glyph(glyph: dict, target_size: int = 24) -> tuple[int, bytes]:
    """Pack a 12px Fusion Pixel BDF glyph into 24px cell via 2× NEAREST upscale.
    Fills the cell completely. Stroke doubling is handled by the source design."""
    bbx = glyph["bbx"]
    bw, bh, xoff, yoff = bbx
    hex_rows = glyph["bitmap"]
    dwidth = glyph["dwidth"]

    bytes_per_row = (bw + 7) // 8
    pixels: list[list[int]] = []
    for row_hex in hex_rows:
        row_hex = row_hex.strip()
        if not row_hex:
            raw = bytes(bytes_per_row)
        else:
            try:
                raw = bytes.fromhex(row_hex)
            except ValueError:
                raw = bytes(bytes_per_row)
        row = []
        for x in range(bw):
            byte_idx = x // 8
            bit_idx = 7 - (x % 8)
            if byte_idx < len(raw):
                row.append(1 if raw[byte_idx] & (1 << bit_idx) else 0)
            else:
                row.append(0)
        pixels.append(row)

    is_cjk = dwidth >= 12
    advance = target_size if is_cjk else (target_size // 2)
    scale = target_size // 12  # 2 for 24px

    # Compute glyph placement in SOURCE (12px) coordinates.
    baseline_src = 10  # FONT_ASCENT from BDF
    glyph_top_src = baseline_src - (yoff + bh)
    glyph_left_src = xoff

    if not is_cjk:
        glyph_w_scaled = bw * scale
        target_left = max(0, (advance - glyph_w_scaled) // 2)
        glyph_left_src = target_left // scale

    stride = (target_size + 7) // 8
    out = bytearray(stride * target_size)

    for sy in range(bh):
        ty = (glyph_top_src + sy) * scale
        if ty < 0 or ty >= target_size:
            continue
        for sx in range(bw):
            if not pixels[sy][sx]:
                continue
            tx = (glyph_left_src + sx) * scale
            for dy in range(scale):
                yy = ty + dy
                if yy < 0 or yy >= target_size:
                    continue
                for dx in range(scale):
                    xx = tx + dx
                    if xx < 0 or xx >= target_size:
                        continue
                    out[yy * stride + xx // 8] |= 0x80 >> (xx % 8)

    return advance, bytes(out)


# ── Main ─────────────────────────────────────────────────────────────────

def replace_output(tmp_path: Path, out_path: Path) -> None:
    for attempt in range(6):
        try:
            os.replace(str(tmp_path), str(out_path))
            return
        except OSError:
            if attempt == 5:
                raise
            time.sleep(0.2 * (attempt + 1))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--size", type=int, choices=(16, 24, 32), required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--set", choices=("level1", "gb2312", "extra"), default="gb2312")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()



    chars = build_charset(args.set)
    needed_cps = {ord(ch) for ch in chars}

    if args.size in (16, 32):
        # Unifont source
        unifont = load_unifont_hex(UNIFONT_HEX, needed_cps)
        missing = [ch for ch in chars if ord(ch) not in unifont]
        if missing:
            print(f"WARNING: {len(missing)} chars missing from Unifont (first: {''.join(missing[:10])})",
                  file=sys.stderr)
    elif args.size == 24:
        # Fusion Pixel BDF source
        fusion = load_fusion_bdf(FUSION_BDF, needed_cps)
        missing = [ch for ch in chars if ord(ch) not in fusion]
        if missing:
            print(f"WARNING: {len(missing)} chars missing from Fusion Pixel (first: {''.join(missing[:10])})",
                  file=sys.stderr)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    glyph_bytes = ((args.size + 7) // 8) * args.size
    tmp_path = out_path.with_suffix(out_path.suffix + ".tmp")

    with tmp_path.open("wb") as f:
        f.write(b"MEF2")
        f.write(struct.pack("<HHI", args.size, glyph_bytes, len(chars)))
        for ch in chars:
            cp = ord(ch)
            f.write(struct.pack("<I", cp))
            if args.size in (16, 32):
                is_cjk = cp >= 0x80
                if cp in unifont:
                    advance, bitmap = pack_unifont_glyph(unifont[cp], args.size, is_cjk)
                else:
                    advance = args.size if is_cjk else (args.size // 2)
                    bitmap = bytes(glyph_bytes)
            else:  # 24
                if cp in fusion:
                    advance, bitmap = pack_fusion_glyph(fusion[cp], args.size)
                else:
                    is_cjk = cp >= 0x80
                    advance = args.size if is_cjk else (args.size // 2)
                    bitmap = bytes(glyph_bytes)
            f.write(struct.pack("<B", advance))
            f.write(bitmap)

    replace_output(tmp_path, out_path)

    if not args.quiet:
        print(f"Generated {out_path}")
        print(f"  size: {args.size}px, glyphs: {len(chars)}, bytes: {out_path.stat().st_size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

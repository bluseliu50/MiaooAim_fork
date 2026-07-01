#!/usr/bin/env python3
"""Generate compact bitmap font data for the EPD firmware."""
import sys, os, gzip, io

UNIFONT_URL = "https://unifoundry.com/pub/unifont/unifont-16.0.02/font-builds/unifont-16.0.02.hex.gz"

UI_FONT_CANDIDATES = [
    "tools/fonts/unifont-16.0.02.hex",
]

SYMBOL_FONT_CANDIDATES = [
    "C:/Windows/Fonts/seguisym.ttf",
    "C:/Windows/Fonts/arial.ttf",
    "C:/Windows/Fonts/NotoSans-Regular.ttf",
    "C:/Windows/Fonts/Noto Sans SC (TrueType).otf",
]

ASCII_FONT_CANDIDATES = [
    "tools/fonts/unifont-16.0.02.hex",
] + UI_FONT_CANDIDATES

def get_gb2312_level1():
    """Generate all 3755 GB2312 Level-1 Chinese characters."""
    chars = []
    for row in range(0xB0, 0xD8):
        col_end = 0xFA if row == 0xD7 else 0xFF
        for col in range(0xA1, col_end):
            try:
                ch = bytes([row, col]).decode('gb2312')
                chars.append(ch)
            except (UnicodeDecodeError, ValueError):
                pass
    return ''.join(chars)

EXTRA_CHARS = (
    "\u6d4f\u89c8\u5668\u9875\u6001\u5740\u949f\u76d8\u8f6e\u64ad"
    "\u58a8\u5347\u7ea7\u7ebf\u7801"
    "\u00b0\u2103\u00b7\u2014\uff1a\uff0c\u3002\uff01\uff1f\uff08\uff09"
    "\u3010\u3011\u300c\u300d"
    "\u7535\u5b50\u58a8\u6c34\u5c4f\u7f51\u7edc\u4fe1\u606f\u4f7f\u7528"
    "\u6b65\u9aa4\u5df2\u8fde\u5730\u5740\u72b6\u6001\u79bb\u7ebf\u70ed"
    "\u70b9\u5bc6\u7801\u7f51\u9875\u6253\u5f00\u914d\u7f6e\u5bb6\u5ead"
    "\u8fd4\u56de\u6309\u952e\u6a21\u5f0f\u4e0a\u4e00\u4e0b"
    "\u5012\u8ba1\u65f6\u5929\u540e\u5c31\u662f\u4eca\u524d\u6682\u65e0"
    "\u8bf7\u901a\u8fc7\u6dfb\u52a0"
)

def get_gb2312_level2():
    """Generate all GB2312 Level-2 Chinese characters (rows D8-F7)."""
    chars = []
    for row in range(0xD8, 0xF8):
        for col in range(0xA1, 0xFF):
            try:
                ch = bytes([row, col]).decode('gb2312')
                chars.append(ch)
            except (UnicodeDecodeError, ValueError):
                pass
    return ''.join(chars)

def download_unifont(cache_path):
    """Download GNU Unifont .hex.gz and cache locally."""
    import urllib.request
    print(f"Downloading GNU Unifont from {UNIFONT_URL} ...")
    try:
        urllib.request.urlretrieve(UNIFONT_URL, cache_path)
        print(f"Saved to {cache_path}")
        return True
    except Exception as e:
        print(f"Download failed: {e}")
        return False


def load_unifont_hex(hex_gz_path, needed_cps):
    """Parse unifont .hex.gz, return dict { codepoint: [32 bytes] } for 16-wide glyphs."""
    needed = set(needed_cps)
    glyphs = {}
    opener = gzip.open if hex_gz_path.endswith('.gz') else open
    with opener(hex_gz_path, 'rt', encoding='ascii') as f:
        for line in f:
            line = line.strip()
            if not line or ':' not in line:
                continue
            cp_str, hex_data = line.split(':', 1)
            cp = int(cp_str, 16)
            if cp not in needed:
                continue
            raw = bytes.fromhex(hex_data)
            if len(raw) == 32:
                glyphs[cp] = list(raw)
            elif len(raw) == 16:
                wide = []
                for b in raw:
                    wide.append(b)
                    wide.append(0)
                glyphs[cp] = wide
            if len(glyphs) == len(needed):
                break
    return glyphs


def ensure_pillow():
    try:
        from PIL import Image, ImageDraw, ImageFont
        return True
    except ImportError:
        print("Pillow not found, installing...")
        import subprocess
        subprocess.check_call([sys.executable, "-m", "pip", "install", "Pillow", "-q"])
        return True


def find_font(candidates):
    from PIL import ImageFont
    for fp in candidates:
        if os.path.exists(fp):
            try:
                ImageFont.truetype(fp, 14)
                return fp
            except Exception:
                continue
    return None

def is_bitmap_font(path):
    """Check if a font is a pixel/bitmap-style font that benefits from
    fontmode='1' binary rendering and NEAREST scaling."""
    name = os.path.basename(path).lower()
    return "chillbitmap" in name or "pixel" in name


def _pack_1bpp(img, w, h, threshold):
    data = []
    for row in range(h):
        byte = 0
        bit_count = 0
        for col in range(w):
            if img.getpixel((col, row)) > threshold:
                byte |= 0x80 >> (bit_count % 8)
            bit_count += 1
            if bit_count % 8 == 0:
                data.append(byte)
                byte = 0
        if bit_count % 8:
            data.append(byte)
    return data


def bitmap_pixel_count(bmp):
    return sum(int(b).bit_count() for b in bmp)


def bitmap_nonempty_columns(bmp, w, h):
    stride = (w + 7) // 8
    cols = 0
    for x in range(w):
        used = False
        for y in range(h):
            if bmp[y * stride + x // 8] & (0x80 >> (x % 8)):
                used = True
                break
        if used:
            cols += 1
    return cols


def validate_ascii_data(ascii_data):
    glyphs = {ch: bmp for _code, ch, bmp in ascii_data}
    required = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
    bad = []
    for ch in required:
        bmp = glyphs.get(ch)
        pixels = bitmap_pixel_count(bmp or [])
        cols = bitmap_nonempty_columns(bmp or [], 8, 16)
        min_cols = 5 if ch in "WwMm" else 2
        if pixels < 3 or cols < min_cols:
            bad.append(f"{ch}(pixels={pixels}, cols={cols})")
    if bad:
        raise RuntimeError("ASCII glyph sanity check failed: " + ", ".join(bad))


def load_supplemental_chars(path):
    if not os.path.exists(path):
        return ""
    chars = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "#" in line:
                line = line.split("#", 1)[0].strip()
            chars.append("".join(ch for ch in line if not ch.isspace()))
    return "".join(chars)


def render_cell(font, ch, cell_w, cell_h, threshold=100, pixel_font=False,
               baseline=12):
    from PIL import Image, ImageDraw

    canvas = Image.new('L', (cell_w, cell_h), 0)
    probe = ImageDraw.Draw(Image.new('L', (max(64, cell_w * 4), max(64, cell_h * 4)), 0))
    if pixel_font:
        probe.fontmode = "1"
    bb = probe.textbbox((0, 0), ch, font=font, anchor="ls")
    glyph_w = max(0, bb[2] - bb[0])
    glyph_h = max(0, bb[3] - bb[1])
    if glyph_w == 0 or glyph_h == 0:
        return _pack_1bpp(canvas, cell_w, cell_h, threshold)

    glyph = Image.new('L', (glyph_w, glyph_h), 0)
    gdraw = ImageDraw.Draw(glyph)
    if pixel_font:
        gdraw.fontmode = "1"
    gdraw.text((-bb[0], -bb[1]), ch, font=font, fill=255, anchor="ls")

    ink_top_to_baseline = -bb[1]

    if glyph_w > cell_w or glyph_h > cell_h:
        new_w = min(glyph_w, cell_w)
        new_h = min(glyph_h, cell_h)
        resample = Image.Resampling.NEAREST if pixel_font else Image.Resampling.LANCZOS
        glyph = glyph.resize((new_w, new_h), resample)
        ink_top_to_baseline = round(ink_top_to_baseline * new_h / glyph_h)
        glyph_w, glyph_h = glyph.size

    x = (cell_w - glyph_w) // 2
    y = baseline - ink_top_to_baseline
    canvas.paste(glyph, (x, y))
    return _pack_1bpp(canvas, cell_w, cell_h, threshold)


def render_8x16(font, ch, pixel_font=False, baseline=12):
    return render_cell(font, ch, 8, 16,
                       threshold=1 if pixel_font else 100,
                       pixel_font=pixel_font, baseline=baseline)


def render_16x16_ttf(font, ch, threshold=90, baseline=12, pixel_font=False):
    """Render a CJK character from TrueType into the existing 16x16 cell."""
    return render_cell(font, ch, 16, 16, threshold=threshold,
                       pixel_font=pixel_font, baseline=baseline)


def render_zh_with_fallback(ch, primary_font, fallback_fonts, unifont_glyphs,
                            baseline=12, pixel_font=False):
    cp = ord(ch)
    if primary_font:
        bmp = render_16x16_ttf(primary_font, ch, baseline=baseline,
                               pixel_font=pixel_font)
    elif cp in unifont_glyphs:
        bmp = unifont_glyphs[cp]
    else:
        bmp = [0] * 32

    if bitmap_pixel_count(bmp) > 0:
        return bmp

    for font in fallback_fonts:
        bmp = render_16x16_ttf(font, ch, baseline=baseline,
                               pixel_font=pixel_font)
        if bitmap_pixel_count(bmp) > 0:
            return bmp

    return unifont_glyphs.get(cp, bmp)


def main():
    if not ensure_pillow():
        sys.exit(1)
    from PIL import ImageFont

    tools_dir = os.path.dirname(os.path.abspath(__file__))

    # --- ASCII via a narrow monospace font ---
    # The firmware stores ASCII in an 8x16 fixed cell. Proportional UI fonts can
    # make wide glyphs such as W/w collapse or crop, so prefer a monospace TTF.
    # --- ASCII via Unifont hex (8x16 cell) ---
    hex_path = os.path.join(tools_dir, "fonts", "unifont-16.0.02.hex")
    ascii_unifont = {}
    if os.path.exists(hex_path):
        with open(hex_path, "r", encoding="utf-8") as hf:
            for line in hf:
                line = line.strip()
                if not line or ":" not in line:
                    continue
                cp_str, hex_str = line.split(":", 1)
                cp = int(cp_str, 16)
                if 32 <= cp <= 126:
                    raw = bytes.fromhex(hex_str)
                    if len(raw) == 32:
                        ascii_unifont[cp] = [raw[i*2] for i in range(16)]
                    elif len(raw) == 16:
                        ascii_unifont[cp] = list(raw)

    baseline = 12
    ascii_data = []
    for code in range(32, 127):
        if code in ascii_unifont:
            ascii_data.append((code, chr(code), ascii_unifont[code]))
        else:
            ascii_data.append((code, chr(code), [0] * 16))
    validate_ascii_data(ascii_data)

    # --- Chinese via UI TrueType font, with Unifont only as last fallback ---
    # fb_render.c uses binary search over font_zh, so glyphs must be sorted by
    # Unicode codepoint. GB2312 table order is not codepoint order.
    supplemental_path = os.path.join(tools_dir, "font_extra_chars.txt")
    supplemental_chars = load_supplemental_chars(supplemental_path)
    if supplemental_chars:
        print(f"Supplemental chars: {len(dict.fromkeys(supplemental_chars))} unique from {supplemental_path}")
    chinese_chars = get_gb2312_level1() + get_gb2312_level2() + EXTRA_CHARS + supplemental_chars
    unique = sorted(dict.fromkeys(chinese_chars), key=ord)
    needed_cps = [ord(ch) for ch in unique]

    zh_path = find_font(UI_FONT_CANDIDATES)
    zh_px = 16 if (zh_path and is_bitmap_font(zh_path)) else 15
    zh_font = ImageFont.truetype(zh_path, zh_px) if zh_path else None
    if zh_path:
        print(f"Chinese UI font: {zh_path}")

    if not zh_path:
        print("WARNING: No Chinese UI font found, falling back to Unifont")

    fallback_fonts = []
    fallback_paths = []
    for fp in SYMBOL_FONT_CANDIDATES + UI_FONT_CANDIDATES:
        if not fp or fp == zh_path or fp in fallback_paths or not os.path.exists(fp):
            continue
        try:
            fallback_fonts.append(ImageFont.truetype(fp, 15))
            fallback_paths.append(fp)
        except Exception:
            continue
    if fallback_paths:
        print("Fallback glyph fonts: " + ", ".join(os.path.basename(p) for p in fallback_paths[:3]))

    hex_path = os.path.join(tools_dir, "fonts", "unifont-16.0.02.hex")
    unifont_glyphs = {}
    if not zh_font:
        if not os.path.exists(hex_path):
            # Try old gz format as fallback
            hex_gz = os.path.join(tools_dir, "unifont.hex.gz")
            if not os.path.exists(hex_gz):
                download_unifont(hex_gz)
            if os.path.exists(hex_gz):
                print(f"Loading Unifont from {hex_gz} ...")
                unifont_glyphs = load_unifont_hex(hex_gz, needed_cps)
        else:
            print(f"Loading Unifont from {hex_path} ...")
            # Parse raw hex format (same as load_unifont_hex but uncompressed)
            needed_set = set(needed_cps)
            with open(hex_path, "r", encoding="utf-8") as hf:
                for line in hf:
                    line = line.strip()
                    if not line or ":" not in line:
                        continue
                    cp_str, hex_str = line.split(":", 1)
                    cp = int(cp_str, 16)
                    if cp not in needed_set:
                        continue
                    raw = bytes.fromhex(hex_str)
                    if len(raw) == 32:
                        unifont_glyphs[cp] = list(raw)
                    elif len(raw) == 16:
                        unifont_glyphs[cp] = list(raw) + [0] * 16
        if unifont_glyphs:
            print(f"  Found {len(unifont_glyphs)}/{len(needed_cps)} glyphs in Unifont")

    zh_data = []
    for ch in unique:
        cp = ord(ch)
        zh_data.append((cp, ch,
                        render_zh_with_fallback(ch, zh_font,
                                                fallback_fonts, unifont_glyphs,
                                                baseline=baseline)))

    # --- Output C header ---
    out = os.path.join(tools_dir, "..", "main", "font_data.h")
    project_dir = os.path.abspath(os.path.join(tools_dir, ".."))

    def source_label(path, size=0):
        if not path:
            return ""
        label = path
        try:
            rel = os.path.relpath(os.path.abspath(path), project_dir)
            if not rel.startswith(".." + os.sep) and rel != "..":
                label = rel.replace(os.sep, "/")
            elif os.path.isabs(path):
                label = "system font: " + os.path.basename(path)
        except ValueError:
            if os.path.isabs(path):
                label = "system font: " + os.path.basename(path)
        if size:
            label = f"{label} @ {size}px"
        return label

    with open(out, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated bitmap font data. Do not edit. */\n")
        f.write(f"/* ASCII: {len(ascii_data)} glyphs (8x16)  Chinese: {len(zh_data)} glyphs (16x16) */\n")
        ascii_source = "GNU Unifont"
        zh_source = source_label(zh_path) if zh_path else "GNU Unifont fallback"
        f.write(f"/* ASCII source: {ascii_source} */\n")
        f.write(f"/* Chinese source: {zh_source} */\n\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")

        f.write(f"static const uint8_t font_ascii[{len(ascii_data)}][16] = {{\n")
        for code, ch, bmp in ascii_data:
            h = ", ".join(f"0x{b:02X}" for b in bmp)
            safe = ch if ch not in ("\\", "'", '"') else f"0x{code:02X}"
            f.write(f"    {{ {h} }}, /* '{safe}' */\n")
        f.write("};\n\n")

        f.write("typedef struct { uint32_t cp; uint8_t bmp[32]; } zh_glyph_t;\n\n")
        f.write(f"static const zh_glyph_t font_zh[{len(zh_data)}] = {{\n")
        for cp, ch, bmp in zh_data:
            h = ", ".join(f"0x{b:02X}" for b in bmp)
            f.write(f"    {{ 0x{cp:04X}, {{ {h} }} }}, /* {ch} */\n")
        f.write("};\n\n")
        f.write(f"#define FONT_ZH_COUNT {len(zh_data)}\n")

    print(f"\nGenerated {out}")
    print(f"  ASCII:   {len(ascii_data)} chars (TrueType/default)")
    print(f"  Chinese: {len(zh_data)} chars ({'TrueType' if zh_font else 'Unifont fallback'})")


if __name__ == "__main__":
    main()

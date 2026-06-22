# Fonts

This firmware uses two font paths:

- `main/font_data.h`: compact built-in 8x16 ASCII and 16x16 GB2312 CJK bitmap font for small text.
- `fontfs` SPIFFS partition: generated 16/24/32 px bitmap fonts for common 4.2 inch UI text.

The generated `fontfs` CJK fonts are built from LXGW 975 Yuan downloaded by `tools/fetch_open_fonts.py`:

- LXGW 975 Yuan SC 400W
- LXGW 975 Yuan SC 400W for Latin, numbers, symbols, and CJK glyphs

These fonts are licensed under the SIL Open Font License 1.1. The downloaded license files are kept under `tools/fonts/noto/` in local builds. The source font files are not committed to keep the repository small; they are fetched during build and converted into `.mef` bitmap files.

To keep the 16 MB flash layout practical, only the 4.2 inch common sizes are stored:

- `cjk16.mef`: GB2312 coverage, used for small labels and status text.
- `cjk24.mef`: GB2312 coverage, used for body text and compact page content.
- `cjk32.mef`: GB2312 coverage, used for titles, larger numbers, and broad fallback.

For 48 px or larger requests, the renderer falls back to `cjk32.mef` and scales it to the requested size. This avoids keeping a separate large 48 px font file in flash.

`tools/gen_ext_font.py` packs the CJK glyphs with a conservative 1bpp threshold.
This keeps the round style visible without making small text too thin or large
text merge into dark blocks on 4.2 inch panels.

Do not generate public firmware assets from Windows system fonts such as Microsoft YaHei, SimHei, SimSun, DengXian, etc. Those fonts may exist on a developer machine, but they are not appropriate as redistributable open-source firmware font assets.

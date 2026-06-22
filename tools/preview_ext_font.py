#!/usr/bin/env python3
"""Render a quick preview sheet for e-paper font candidates."""

from __future__ import annotations

import argparse
from pathlib import Path


SAMPLES = [
    "喵喵墨水屏",
    "日历 天气 待办 倒计时",
    "2026年6月21日 星期日",
    "体感 湿度 风力 气压",
    "课程表 留言板 额度",
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", required=True)
    parser.add_argument("--out", default="outputs/font-preview/font-preview.png")
    args = parser.parse_args()

    from PIL import Image, ImageDraw, ImageFont

    font_path = Path(args.font)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    rows: list[tuple[str, ImageFont.FreeTypeFont]] = []
    for px in (24, 32, 48):
        rows.append((f"{px}px", ImageFont.truetype(str(font_path), px)))

    width = 760
    line_h = 62
    top = 24
    height = top + len(rows) * len(SAMPLES) * line_h + 28
    im = Image.new("L", (width, height), 255)
    draw = ImageDraw.Draw(im)
    label_font = ImageFont.truetype(str(font_path), 18)

    y = top
    for label, font in rows:
        for text in SAMPLES:
            draw.text((24, y + 8), label, font=label_font, fill=0)
            draw.text((98, y), text, font=font, fill=0)
            draw.line((20, y + line_h - 8, width - 20, y + line_h - 8), fill=220)
            y += line_h

    im.save(out_path)
    print(out_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

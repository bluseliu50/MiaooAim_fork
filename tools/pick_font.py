#!/usr/bin/env python3
"""交互式选择 tools/fonts/ 下的字体，输出相对项目根的路径供 gen_ext_font.py --font 使用。

用法：
  pick_font.py                 # 交互式列出全部字体，等待用户输入序号
  pick_font.py --query fz      # 模糊匹配文件名；唯一命中直接返回，多个则交互选
  pick_font.py --list          # 仅列出可用字体，不选择

非 TTY（如 CI / 管道）且无 --query 时：列出字体并以非零码退出，
提示用户通过 FONT= 显式指定。
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent
FONT_DIR = PROJECT_DIR / "tools" / "fonts"
EXTS = (".ttf", ".ttc", ".otf")


def eprint(*a, **kw):
    """输出到 stderr——保证 stdout 仅含最终选中的字体路径，
    便于 Makefile 用命令替换 $(...) 捕获结果而不吞掉交互提示。"""
    print(*a, file=sys.stderr, **kw)


def scan_fonts() -> list[Path]:
    if not FONT_DIR.exists():
        return []
    return sorted(
        (p for p in FONT_DIR.rglob("*")
         if p.suffix.lower() in EXTS and p.is_file()),
        key=lambda p: str(p).lower(),
    )


def rel(p: Path) -> str:
    """相对项目根的路径（forward slash），可直接喂给 --font。"""
    return str(p.relative_to(PROJECT_DIR)).replace("\\", "/")


def match(fonts: list[Path], query: str) -> list[Path]:
    q = query.lower()
    return [f for f in fonts if q in rel(f).lower() or q in f.stem.lower()]


def choose_interactive(fonts: list[Path]) -> Path | None:
    eprint("可用字体：")
    for i, f in enumerate(fonts, 1):
        eprint(f"  [{i}] {f.name}   ({rel(f)})")
    while True:
        try:
            choice = input(f"选择 [1-{len(fonts)}]，q 退出: ").strip()
        except EOFError:
            return None
        if choice.lower() in ("q", "quit", "exit"):
            return None
        if choice.isdigit() and 1 <= int(choice) <= len(fonts):
            return fonts[int(choice) - 1]
        eprint("无效输入，请重试。")


def emit(chosen: Path, out_file: str | None) -> None:
    """输出选中字体路径：写到 --out 指定文件（供 Makefile），同时打印到 stdout。"""
    p = rel(chosen)
    if out_file:
        Path(out_file).write_text(p)
    print(p)


def main() -> int:
    ap = argparse.ArgumentParser(description="为字库生成选择字体源")
    ap.add_argument("--query", help="模糊匹配字体文件名（大小写不敏感）")
    ap.add_argument("--list", action="store_true", help="仅列出，不选择")
    ap.add_argument("--out", metavar="FILE",
                    help="将选中字体的相对路径写入该文件（供 Makefile 读取）")
    args = ap.parse_args()

    fonts = scan_fonts()
    if not fonts:
        eprint(f"未在 {rel(FONT_DIR)} 下找到任何字体文件。")
        eprint("请将 .ttf/.otf/.ttc 放入该目录后重试。")
        return 1

    if args.list:
        for f in fonts:
            print(rel(f))
        return 0

    candidates = match(fonts, args.query) if args.query else fonts
    if args.query and not candidates:
        eprint(f"没有字体匹配 '{args.query}'。可用字体：")
        for f in fonts:
            eprint(f"  {rel(f)}")
        return 1
    if args.query and len(candidates) == 1:
        emit(candidates[0], args.out)
        return 0

    # 多个候选 或 无 query：交互式（Makefile 以 </dev/tty 提供 TTY）
    if not sys.stdin.isatty():
        eprint("可用字体（非交互环境，请用 FONT=<名字或路径> 指定）：")
        for f in candidates:
            eprint(f"  {rel(f)}")
        return 2

    chosen = choose_interactive(candidates)
    if chosen is None:
        eprint("已取消。")
        return 1
    emit(chosen, args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

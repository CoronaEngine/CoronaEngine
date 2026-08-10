#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
将多个文件复制到目标目录，若内容相同则跳过（减少无意义写入）。
用法：
    python copy_files.py --dest <DEST_DIR> [--list deps.txt] <FILE1> <FILE2> ...
- 目标目录会自动创建
- 已存在且相同的文件将跳过复制
- 日志为英文
"""

from __future__ import annotations

import argparse
import filecmp
import os
import shutil
import sys
from pathlib import Path


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def copy_if_changed(src: Path, dest_dir: Path) -> None:
    if not src.exists():
        print(f"[copy] Skip (missing): {src}")
        return
    ensure_dir(dest_dir)
    dest = dest_dir / src.name
    if dest.exists():
        try:
            same = filecmp.cmp(str(src), str(dest), shallow=False)
        except Exception:
            same = False
        if same:
            print(f"[copy] Same, skip: {src} -> {dest}")
            return
    print(f"[copy] Copy: {src} -> {dest}")
    shutil.copy2(src, dest)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Copy files to destination if different")
    ap.add_argument("--dest", required=True, help="Destination directory")
    ap.add_argument(
        "--list",
        action="append",
        default=[],
        help="Path to newline-separated dependency list",
    )
    ap.add_argument("files", nargs="*", help="Files to copy")
    args = ap.parse_args(argv)

    dest_dir = Path(args.dest).resolve()

    raw_entries: list[str] = []

    for list_path in args.list:
        list_file = Path(list_path)
        if not list_file.exists():
            print(f"[copy] List missing, skip: {list_file}")
            continue
        try:
            content = list_file.read_text(encoding="utf-8")
        except Exception as exc:
            print(f"[copy] Failed to read list {list_file}: {exc}")
            continue
        for line in content.splitlines():
            line = line.strip()
            if line:
                raw_entries.append(line)

    raw_entries.extend(args.files)
    files = [Path(entry).resolve() for entry in raw_entries if entry.strip()]

    if not files:
        print("[copy] No files specified; nothing to do")
        return 0

    for f in files:
        copy_if_changed(f, dest_dir)

    return 0


if __name__ == "__main__":
    sys.exit(main())

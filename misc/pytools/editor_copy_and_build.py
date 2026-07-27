#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
复制 Editor 资源目录到目标可执行目录下的 CabbageEditor/ 子目录，并使用本地 Node 环境执行前端构建。
- 通过多次传入 --src-dir 指定多个源目录（如 Backend、Frontend 等）
- 目标根目录由 --dest-root 指定（通常是 $<TARGET_FILE_DIR:...>/CabbageEditor ）
- 然后用 --node-dir 和 --frontend-dir 触发 npm install / npm run build
注意：日志输出为英文，便于在构建日志中检索。
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import uuid
from pathlib import Path
import subprocess
from typing import Optional


def echo(msg: str) -> None:
    print(msg)


_STALE_EXTENSIONS = {".py", ".vue", ".js", ".ts", ".jsx", ".tsx", ".css", ".scss"}


def _cleanup_stale_files(src_root: Path, dst_root: Path, ignore_fn) -> None:
    """删除目标目录中存在但源目录中已不存在的源码文件（一对一映射）。
    手动递归遍历以跳过 ignore 目录（如 node_modules），避免进入超长路径。"""
    if not dst_root.exists() or not src_root.exists():
        return

    dirs_to_check: list[Path] = []

    def _walk(directory: Path) -> None:
        try:
            entries = list(directory.iterdir())
        except OSError:
            return
        for entry in entries:
            if ignore_fn("", [entry.name]):
                continue
            if entry.is_dir():
                _walk(entry)
                dirs_to_check.append(entry)
            elif entry.is_file() and entry.suffix in _STALE_EXTENSIONS:
                rel = entry.relative_to(dst_root)
                if not (src_root / rel).exists():
                    echo(f"[editor-copy] Remove stale: {entry}")
                    try:
                        entry.unlink()
                    except Exception as e:
                        echo(f"[editor-copy] ERROR removing {entry}: {e}")

    _walk(dst_root)

    # 清理删除后残留的空目录（自底向上，dirs_to_check 已按深度优先顺序）
    for dirpath in reversed(dirs_to_check):
        try:
            if dirpath.is_dir() and not any(dirpath.iterdir()):
                echo(f"[editor-copy] Remove empty dir: {dirpath}")
                dirpath.rmdir()
        except OSError:
            pass


def _normalize_destination_case(path: Path) -> Path:
    """Make a Windows destination entry use the source tree's exact casing."""
    if os.name != "nt" or not path.parent.exists():
        return path

    for existing in path.parent.iterdir():
        if existing.name == path.name:
            return path
        if existing.name.casefold() != path.name.casefold():
            continue

        temporary = path.parent / f".{path.name}.case-normalize-{uuid.uuid4().hex}"
        existing.rename(temporary)
        temporary.rename(path)
        return path

    return path


def copy_tree(src: Path, dst: Path, merge_content: bool = False) -> None:
    if not src.exists():
        echo(f"[editor-copy] Skip (not exists): {src}")
        return
    # Ensure destination directory exists
    dst.mkdir(parents=True, exist_ok=True)

    # Avoid copying heavy transient folders and development-only files
    def _should_ignore(name: str) -> bool:
        """Check if a file/directory should be ignored."""
        # Direct match list
        ignored_exact = {
            # Node/Frontend build artifacts
            "node_modules", "dist", ".vite", ".turbo",
            # Python cache and test
            "__pycache__", ".pytest_cache", ".tox",
            # IDE and editor
            ".idea", ".vscode", ".vs", "autosave",
            # Version control
            ".git", ".gitignore", ".gitattributes",
            # Build and deployment
            "Dockerfile", "docker-compose.yml",
            # Documentation
            "README.md", "docs",
            # Dependencies manifests (dev-only lock files)
            "requirements.txt", "package-lock.json", "yarn.lock", "pnpm-lock.yaml",
            # Test files
            "tests", "test",
            # Temporary and log files
            ".cache", ".temp", "tmp",
            # OS files
            ".DS_Store", "Thumbs.db", "desktop.ini"
        }

        # Direct match
        if name in ignored_exact:
            return True

        # Pattern matching
        if name.endswith(('.pyc', '.pyo', '.log', '.bak', '.backup', '.md')) or name.endswith('~'):
            return True

        return False

    def _ignore(dirpath: str, names: list[str]) -> set[str]:
        """shutil.copytree compatible ignore function."""
        return {n for n in names if _should_ignore(n)}

    if merge_content:
        # Copy the content of src directory directly into dst (no subdirectory created)
        echo(f"[editor-copy] Merging content: {src}/* -> {dst}")
        for item in src.iterdir():
            # Check if should be ignored
            if _should_ignore(item.name):
                echo(f"[editor-copy] Ignoring: {item.name}")
                continue

            target = _normalize_destination_case(dst / item.name)
            try:
                if item.is_dir():
                    shutil.copytree(item, target, dirs_exist_ok=True, ignore=_ignore)
                    # 删除目标中源已不存在的源码文件
                    _cleanup_stale_files(item, target, _ignore)
                else:
                    shutil.copy2(item, target)
            except Exception as e:
                print(f"[editor-copy] ERROR copying {item} -> {target}: {e}")
    else:
        # Original behavior: copy src as a subdirectory of dst
        target = _normalize_destination_case(dst / src.name)
        echo(f"[editor-copy] Copying: {src} -> {target}")
        try:
            shutil.copytree(src, target, dirs_exist_ok=True, ignore=_ignore)
            # 删除目标中源已不存在的源码文件
            _cleanup_stale_files(src, target, _ignore)
        except Exception as e:
            print(f"[editor-copy] ERROR copying {src} -> {target}: {e}")


def run(cmd: list[str], cwd: Path | None = None, env: dict | None = None) -> int:
    print(f"$ {' '.join(str(c) for c in cmd)}")
    if cwd:
        print(f"  (cwd: {cwd})")
    try:
        p = subprocess.run([str(c) for c in cmd], cwd=str(cwd) if cwd else None, env=env, check=False)
        return p.returncode
    except FileNotFoundError as e:
        print(f"ERROR: Command not found: {cmd[0]} -> {e}")
        return 127


def stream_run(cmd: list[str], cwd: Optional[Path] = None, env: Optional[dict] = None) -> int:
    # Simplified: just run and let the console handle buffering/encoding
    print(f"$ {' '.join(str(c) for c in cmd)}")
    if cwd:
        print(f"  (cwd: {cwd})")
    try:
        p = subprocess.run([str(c) for c in cmd], cwd=str(cwd) if cwd else None, env=env, check=False)
        return p.returncode
    except FileNotFoundError as e:
        print(f"ERROR: Command not found: {cmd[0]} -> {e}")
        return 127


def maybe_run_npm(frontend_dir: Path, node_dir: Path) -> int:
    if not frontend_dir.exists():
        echo(f"[npm-build] Frontend directory not found: {frontend_dir}; skip build")
        return 0
    npm_cmd = node_dir / ("npm.cmd" if os.name == "nt" else "npm")
    if not npm_cmd.exists():
        echo(f"[npm-build] npm not found at: {npm_cmd}; skip build")
        return 0
    env = os.environ.copy()
    env["PATH"] = str(node_dir) + os.pathsep + env.get("PATH", "")
    # Keep environment unchanged
    echo("[npm-build] Installing dependencies...")
    rc = stream_run([str(npm_cmd), "install"], cwd=frontend_dir, env=env)
    if rc != 0:
        print(f"CMake Warning: [npm-build] 'npm install' failed (exit code {rc}); continuing without frontend build.", flush=True)
        return 0
    echo("[npm-build] Building frontend...")
    rc = stream_run([str(npm_cmd), "run", "build"], cwd=frontend_dir, env=env)
    if rc != 0:
        print(f"CMake Warning: [npm-build] 'npm run build' failed (exit code {rc}); continuing without frontend build.", flush=True)
        return 0
    echo("[npm-build] Frontend build completed successfully.")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Copy editor resources and run npm build")
    ap.add_argument("--dest-root", required=True, help="Destination root directory (CabbageEditor under target dir)")
    ap.add_argument("--src-dir", action="append", default=[], help="Source directory to copy (can be specified multiple times)")
    ap.add_argument("--merge-content", action="store_true",
                    help="Merge source directory content directly into dest (don't create subdirectory)")
    ap.add_argument("--frontend-dir", required=True, help="Frontend directory path for npm build")
    ap.add_argument("--node-dir", required=True, help="Bundled Node directory path")
    args = ap.parse_args(argv)

    dest_root = Path(args.dest_root).resolve()
    src_dirs = [Path(p).resolve() for p in args.src_dir]
    frontend_dir = Path(args.frontend_dir).resolve()
    node_dir = Path(args.node_dir).resolve()

    echo(f"[editor-copy] Dest root: {dest_root}")
    dest_root.mkdir(parents=True, exist_ok=True)

    for s in src_dirs:
        copy_tree(s, dest_root, merge_content=args.merge_content)

    # Run npm build last
    return maybe_run_npm(frontend_dir, node_dir)


if __name__ == "__main__":
    sys.exit(main())

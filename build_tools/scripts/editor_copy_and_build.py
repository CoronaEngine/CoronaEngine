#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
复制 Editor 资源目录到目标可执行目录下的 CabbageEditor/ 子目录，并使用本地 Node 环境执行前端构建。
- 通过多次传入 --src-dir 指定多个源目录（如 Backend、Frontend 等）
- --python-dir 指定 Engine 自有的 Python 源目录（合并到目标根）
- 目标根目录由 --dest-root 指定（通常是 $<TARGET_FILE_DIR:...>/CabbageEditor ）
- 然后用 --node-dir 和 --frontend-dir 触发 npm install / npm run build
注意：日志输出为英文，便于在构建日志中检索。
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path
import subprocess
import uuid
from typing import Optional


if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from build_tools.paths import REPO_ROOT


def echo(msg: str) -> None:
    print(msg)


_STALE_EXTENSIONS = {".py", ".vue", ".js", ".ts", ".jsx", ".tsx", ".css", ".scss", ".mjs"}
_PROTECTED_DEPLOYED_ROOTS = (Path("data"), Path("runtime") / "generated", Path(".retired-plugins"))


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
        "requirements.txt", "yarn.lock",
        # Test files and engine-only python development assets
        "tests", "test", "frontend_assets",
        # Temporary and log files
        ".cache", ".temp", ".tmp", "tmp", ".retired-plugins",
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


def _retire_removed_ai_packages(src_root: Path, dst_root: Path) -> None:
    """Move obsolete deployed packages out of the import path, preserving local data."""
    if not (src_root / "plugins" / "AITool" / "main.py").is_file():
        return
    destination = dst_root.resolve()
    for name in ("Quasar", "cai_extensions"):
        relative = Path("plugins") / "AITool" / name
        retired = dst_root / relative
        if (src_root / relative).exists() or not retired.exists():
            continue
        # Do not follow an old deployment junction outside the managed tree.
        if not retired.resolve().is_relative_to(destination):
            raise ValueError(f"Retired AI package is outside the deployment: {retired}")
        archive = dst_root / ".retired-plugins"
        if not archive.resolve().is_relative_to(destination):
            raise ValueError(f"Retired plugin archive is outside the deployment: {archive}")
        archive.mkdir(exist_ok=True)
        backup = archive / f"{name}-{uuid.uuid4().hex}"
        retired.rename(backup)
        echo(f"[editor-copy] Retired {retired}; preserved previous files in {backup}")


def _cleanup_stale_files(sources: list[Path], dst_root: Path, ignore_fn) -> None:
    """删除目标目录中存在但所有源目录中均不存在的源码文件（多源合并映射）。
    手动递归遍历以跳过 ignore 目录（如 node_modules），避免进入超长路径。"""
    source_roots = [source for source in sources if source.exists()]
    if not dst_root.exists() or not source_roots:
        return

    _retire_removed_ai_packages(source_roots[0], dst_root)

    dirs_to_check: list[Path] = []

    def _walk(directory: Path) -> None:
        try:
            entries = list(directory.iterdir())
        except OSError:
            return
        for entry in entries:
            if ignore_fn("", [entry.name]):
                continue
            relative = entry.relative_to(dst_root)
            if any(
                relative == protected or protected in relative.parents
                for protected in _PROTECTED_DEPLOYED_ROOTS
            ):
                continue
            if entry.is_dir():
                _walk(entry)
                dirs_to_check.append(entry)
            elif entry.is_file() and entry.suffix in _STALE_EXTENSIONS:
                rel = entry.relative_to(dst_root)
                if not any((source_root / rel).exists() for source_root in source_roots):
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


def copy_tree(src: Path, dst: Path, merge_content: bool = False, cleanup: bool = True) -> None:
    if not src.exists():
        echo(f"[editor-copy] Skip (not exists): {src}")
        return
    # Ensure destination directory exists
    dst.mkdir(parents=True, exist_ok=True)

    if merge_content:
        # Copy the content of src directory directly into dst (no subdirectory created)
        echo(f"[editor-copy] Merging content: {src}/* -> {dst}")
        for item in src.iterdir():
            # Check if should be ignored
            if _should_ignore(item.name):
                echo(f"[editor-copy] Ignoring: {item.name}")
                continue

            target = dst / item.name
            try:
                if item.is_dir():
                    shutil.copytree(item, target, dirs_exist_ok=True, ignore=_ignore)
                else:
                    shutil.copy2(item, target)
            except Exception as e:
                print(f"[editor-copy] ERROR copying {item} -> {target}: {e}")
        # Sweep from the merge root. Per-directory cleanup cannot see a source
        # directory that was removed entirely (for example an old compatibility
        # package), so stale Python/JS sources at that depth would otherwise
        # survive in the deployed CabbageEditor tree. With multiple merge roots
        # (editor remainder + engine-owned python) the caller performs one
        # union cleanup instead, so copy-time cleanup is opt-in.
        if cleanup:
            _cleanup_stale_files([src], dst, _ignore)
    else:
        # Original behavior: copy src as a subdirectory of dst
        target = dst / src.name
        echo(f"[editor-copy] Copying: {src} -> {target}")
        try:
            shutil.copytree(src, target, dirs_exist_ok=True, ignore=_ignore)
            # 删除目标中源已不存在的源码文件
            if cleanup:
                _cleanup_stale_files([src], target, _ignore)
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


def _can_write_and_delete(directory: Path) -> bool:
    """Probe whether a directory both accepts writes and honours deletes.

    esbuild ships a Go binary that cleans up its temp files with os.Remove.
    Restricted environments (file sandboxes whose ACLs only grant delete rights
    inside the source tree) let the write succeed but reject the unlink with
    "Access is denied", which aborts the Vite build via [vite:esbuild-transpile].
    A directory is therefore only usable when the delete half also works.
    """
    try:
        directory.mkdir(parents=True, exist_ok=True)
        probe = directory / f".corona-write-probe-{os.getpid()}"
        probe.write_bytes(b"probe")
        probe.unlink()
        return True
    except OSError:
        return False


def _select_npm_tmp_dir(frontend_dir: Path) -> Optional[Path]:
    """Return a fresh temp directory to export as TMP/TEMP for the npm children.

    The probe cannot be applied to the inherited temp directory: this Python
    process deletes there happily, while the esbuild Go binary can be refused by
    a file sandbox even though ordinary children (cmd.exe, node) succeed. That
    asymmetry is what turns a Vite build into
    "[vite:esbuild-transpile] remove <path>: Access is denied". So the directory
    is chosen by construction instead: the frontend always lives inside the
    build tree, which is writable and deletable in every configuration observed.
    """
    for base in (
        frontend_dir.parent / ".tmp",  # CabbageEditor/.tmp, outside the Vite root
        frontend_dir / ".tmp",
        REPO_ROOT / "build" / ".tmp",
    ):
        if not _can_write_and_delete(base):
            continue
        unique = base / f"npm-tmp-{os.getpid()}"
        try:
            unique.mkdir(parents=True, exist_ok=True)
        except OSError:
            continue
        echo(f"[npm-build] TMP/TEMP -> {unique}")
        return unique
    echo(
        "[npm-build] WARNING: no writable-and-deletable temp directory found; "
        "esbuild may fail while cleaning up its temp files"
    )
    return None


def maybe_run_npm(frontend_dir: Path, node_dir: Path) -> int:
    if not frontend_dir.is_dir():
        echo(f"[npm-build] ERROR: Frontend directory not found: {frontend_dir}")
        return 1
    npm_cmd = node_dir / ("npm.cmd" if os.name == "nt" else "npm")
    if not npm_cmd.is_file():
        echo(f"[npm-build] ERROR: npm executable not found: {npm_cmd}")
        return 127
    env = os.environ.copy()
    env["PATH"] = str(node_dir) + os.pathsep + env.get("PATH", "")
    tmp_dir = _select_npm_tmp_dir(frontend_dir)
    if tmp_dir is not None:
        for key in ("TMP", "TEMP", "TMPDIR"):
            env[key] = str(tmp_dir)
    # Reproducible bundles come from the lockfile the project uses. The editor pins its
    # tree with pnpm-lock.yaml (a pnpm lock cannot be consumed by npm), so the deployed
    # frontend is installed with pnpm --frozen-lockfile; pnpm comes from the npm cache so
    # the build environment does not need a separate global install. npm is the fallback.
    pnpm_lock = frontend_dir / "pnpm-lock.yaml"
    npm_lock = frontend_dir / "package-lock.json"
    if pnpm_lock.is_file():
        runner = [str(npm_cmd), "exec", "--yes", "pnpm@9", "--"]
        install = [*runner, "install", "--frozen-lockfile"]
        build = [*runner, "run", "build"]
        label = "pnpm install --frozen-lockfile"
    elif npm_lock.is_file():
        install = [str(npm_cmd), "ci"]
        build = [str(npm_cmd), "run", "build"]
        label = "npm ci"
    else:
        install = [str(npm_cmd), "install"]
        build = [str(npm_cmd), "run", "build"]
        label = "npm install (no lockfile: dependency resolution is not pinned)"
    try:
        echo(f"[npm-build] Installing dependencies ({label})...")
        rc = stream_run(install, cwd=frontend_dir, env=env)
        if rc != 0:
            print(f"[npm-build] ERROR: '{label}' failed (exit code {rc}).", flush=True)
            return rc
        echo("[npm-build] Building frontend...")
        rc = stream_run(build, cwd=frontend_dir, env=env)
        if rc != 0:
            print(f"[npm-build] ERROR: frontend build failed (exit code {rc}).", flush=True)
            return rc
    finally:
        # Only ever remove the directory this function created for the build.
        if tmp_dir is not None:
            container = tmp_dir.parent
            shutil.rmtree(tmp_dir, ignore_errors=True)
            try:
                container.rmdir()  # drop the shared .tmp container once empty
            except OSError:
                pass
    frontend_index = frontend_dir / "dist" / "index.html"
    if not frontend_index.is_file():
        echo(f"[npm-build] ERROR: Build output is missing or not a file: {frontend_index}")
        return 1
    echo("[npm-build] Frontend build completed successfully.")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Copy editor resources and run npm build")
    ap.add_argument("--dest-root", required=True, help="Destination root directory (CabbageEditor under target dir)")
    ap.add_argument("--src-dir", action="append", default=[], help="Source directory to copy (can be specified multiple times)")
    ap.add_argument("--python-dir", default=None,
                    help="Engine-owned python source directory merged into the destination root")
    ap.add_argument("--merge-content", action="store_true",
                    help="Merge source directory content directly into dest (don't create subdirectory)")
    ap.add_argument("--frontend-dir", required=True, help="Frontend directory path for npm build")
    ap.add_argument("--node-dir", required=True, help="Conda Node directory selected during CMake configure")
    args = ap.parse_args(argv)

    dest_root = Path(args.dest_root).resolve()
    src_dirs = [Path(p).resolve() for p in args.src_dir]
    python_dir = Path(args.python_dir).resolve() if args.python_dir else None
    frontend_dir = Path(args.frontend_dir).resolve()
    node_dir = Path(args.node_dir).resolve()

    echo(f"[editor-copy] Dest root: {dest_root}")
    dest_root.mkdir(parents=True, exist_ok=True)

    merge_sources = [*src_dirs, *([python_dir] if python_dir else [])]
    for source in merge_sources:
        copy_tree(source, dest_root, merge_content=args.merge_content, cleanup=False)

    # One union sweep so a file is only removed when no merge source owns it.
    # Runs before materializing the block program: that file comes from
    # frontend_assets/ and has no same-path owner in any merge source.
    _cleanup_stale_files(merge_sources, dest_root, _ignore)

    # Development-only python assets must not survive in a deployed tree.
    for name in ("frontend_assets",):
        stale = dest_root / name
        if stale.is_dir():
            shutil.rmtree(stale, ignore_errors=True)
            echo(f"[editor-copy] Removed deployed dev assets: {stale}")
    if python_dir:
        # The frontend imports the block program template with Vite's `?raw`
        # loader, so it must exist inside the deployed Frontend tree before the
        # npm build even though its source now lives in the engine repository.
        block_program = python_dir / "frontend_assets" / "block_program.py"
        if block_program.is_file():
            target = dest_root / "Frontend" / "src" / "blockly" / "generators" / "runtime" / "block_program.py"
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(block_program, target)
            echo(f"[editor-copy] Materialized block program: {target}")

    # Run npm build last
    return maybe_run_npm(frontend_dir, node_dir)


if __name__ == "__main__":
    sys.exit(main())

"""Deploy the Python standard library incrementally.

The old POST_BUILD step ran `cmake -E copy_directory_if_different` over the whole conda
`Lib` (about 2 GB and 53k files). CMake compares file contents, so every build re-read both
trees even when nothing changed, which often cost more than compiling the engine.

This script keeps a manifest of (relative path, size, mtime) for the managed roots and only
touches files that actually differ:

- unchanged tree: a quick manifest compare plus one stat per deployed file (no reads), then
  it reports "up to date" and exits;
- changed tree: copies only new/changed files and removes files that disappeared from the
  source, so the runtime mirrors the environment without a full recopy.

Usage:
    python deploy_python_runtime.py --source-lib <env>/Lib --source-dlls <env>/DLLs \
        --destination <runtime>/python-runtime
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

ROOTS = ("Lib", "DLLs")
MANIFEST_NAME = ".deploy-manifest.json"


def scan(root: Path) -> dict[str, tuple[int, int]]:
    """Walk a tree with os.scandir; Windows fills DirEntry.stat() from the directory read."""
    entries: dict[str, tuple[int, int]] = {}
    stack = [str(root)]
    while stack:
        directory = stack.pop()
        try:
            with os.scandir(directory) as iterator:
                for entry in iterator:
                    if entry.is_dir(follow_symlinks=False):
                        stack.append(entry.path)
                    elif entry.is_file(follow_symlinks=False):
                        stat = entry.stat(follow_symlinks=False)
                        relative = os.path.relpath(entry.path, root).replace(os.sep, "/")
                        entries[relative] = (stat.st_size, stat.st_mtime_ns)
        except OSError:
            continue
    return entries


def collect(source_lib: Path, source_dlls: Path) -> dict[str, list]:
    """Map "<root>/<relative path>" -> [size, mtime_ns] for every managed file."""
    entries: dict[str, list] = {}
    for root_name, root in (("Lib", source_lib), ("DLLs", source_dlls)):
        for relative, (size, mtime) in scan(root).items():
            entries[f"{root_name}/{relative}"] = [size, mtime]
    return entries


def load_manifest(path: Path) -> dict[str, list] | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) else None


def write_manifest(path: Path, entries: dict[str, list]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix=".deploy-", dir=path.parent)
    try:
        with os.fdopen(handle, "w", encoding="utf-8") as stream:
            json.dump(entries, stream, ensure_ascii=False, indent=0, sort_keys=True)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def deployed_matches(destination: Path, entries: dict[str, list]) -> bool:
    deployed: dict[str, tuple[int, int]] = {}
    for root_name in ROOTS:
        for relative, (size, mtime) in scan(destination / root_name).items():
            deployed[f"{root_name}/{relative}"] = (size, mtime)
    return {relative: (size, mtime) for relative, (size, mtime) in entries.items()} == deployed


def deploy(source_lib: Path, source_dlls: Path, destination: Path) -> int:
    manifest_path = destination / MANIFEST_NAME
    entries = collect(source_lib, source_dlls)
    previous = load_manifest(manifest_path)

    if previous == entries and deployed_matches(destination, entries):
        print(f"[python-runtime] up to date ({len(entries)} files)")
        return 0

    copied = 0
    deployed: dict[str, tuple[int, int]] = {}
    for root_name in ROOTS:
        for relative, stat in scan(destination / root_name).items():
            deployed[f"{root_name}/{relative}"] = stat
    for relative, (size, mtime) in entries.items():
        if deployed.get(relative) == (size, mtime):
            continue
        source = (source_lib if relative.startswith("Lib/") else source_dlls) / relative.split("/", 1)[1]
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        copied += 1

    removed = 0
    if previous is not None:
        for relative in set(previous) - set(entries):
            target = destination / relative
            if target.is_file():
                target.unlink()
                removed += 1

    write_manifest(manifest_path, entries)
    print(f"[python-runtime] deployed {copied} changed file(s), removed {removed} stale file(s), "
          f"{len(entries)} managed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-lib", type=Path, required=True)
    parser.add_argument("--source-dlls", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()

    for path in (args.source_lib, args.source_dlls):
        if not path.is_dir():
            print(f"[python-runtime] missing source directory: {path}", file=sys.stderr)
            return 2
    args.destination.mkdir(parents=True, exist_ok=True)
    return deploy(args.source_lib, args.source_dlls, args.destination)


if __name__ == "__main__":
    raise SystemExit(main())

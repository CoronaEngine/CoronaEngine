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
import re
from importlib import metadata
from pathlib import Path
import shutil
import sys
import tempfile

ROOTS = ("Lib", "DLLs")
MANIFEST_NAME = ".deploy-manifest.json"


def _normalize(name: str) -> str:
    """PEP 503 normalization: numpy, NumPy, numpy.libs and numpy-2.0.dist-info agree."""
    return re.sub(r"[-_.]+", "-", name.strip().lower())


def _package_name(head: str) -> str:
    """Normalize a site-packages entry to its distribution name."""
    name = head.lower()
    if name.endswith(".dist-info"):
        # "numpy-2.1.0.dist-info": strip the metadata suffix, then the version.
        name = name[: -len(".dist-info")].rsplit("-", 1)[0]
    elif name.endswith(".libs"):
        name = name[: -len(".libs")]
    return _normalize(name)


def read_requirements(path: Path) -> set[str]:
    """Top-level distributions the deployed runtime is allowed to contain."""
    names: set[str] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line or line.startswith("-"):
            continue
        match = re.match(r"[A-Za-z0-9][A-Za-z0-9._-]*", line)
        if match:
            names.add(_normalize(match.group(0)))
    return names


def expand(requirements: set[str]) -> set[str]:
    """Add the import package names a distribution provides (Pillow -> PIL)."""
    names = set(requirements)
    for name in requirements:
        try:
            distribution = metadata.distribution(name)
        except metadata.PackageNotFoundError:
            continue
        top_level = distribution.read_text("top_level.txt")
        if top_level:
            names.update(_normalize(line) for line in top_level.splitlines() if line.strip())
            continue
        for file in distribution.files or []:
            parts = file.parts
            if len(parts) > 1 and not parts[0].endswith((".dist-info", ".libs")):
                head = parts[0]
                names.add(_normalize(head[:-3] if head.endswith(".py") else head))
    return names


def allowed(relative: str, requirements: set[str] | None) -> bool:
    """With a requirements list, site-packages is limited to those distributions."""
    if requirements is None:
        return True
    prefix = "site-packages/"
    if not relative.startswith(prefix):
        return True
    head = relative[len(prefix):].split("/", 1)[0]
    return _package_name(head) in requirements


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


def collect(source_lib: Path, source_dlls: Path,
            requirements: set[str] | None = None) -> dict[str, list]:
    """Map "<root>/<relative path>" -> [size, mtime_ns] for every managed file."""
    entries: dict[str, list] = {}
    for root_name, root in (("Lib", source_lib), ("DLLs", source_dlls)):
        for relative, (size, mtime) in scan(root).items():
            if root_name == "Lib" and not allowed(relative, requirements):
                continue
            entries[f"{root_name}/{relative}"] = [size, mtime]
    return entries


def load_manifest(path: Path) -> dict[str, list] | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) else None


def prune_empty_dirs(root: Path) -> None:
    """Removing files leaves their directories behind; drop the empty shells."""
    for directory in sorted((path for path in root.rglob("*") if path.is_dir()),
                            key=lambda path: len(path.parts), reverse=True):
        try:
            if not any(directory.iterdir()):
                directory.rmdir()
        except OSError:
            continue


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


def deploy(source_lib: Path, source_dlls: Path, destination: Path,
           requirements: set[str] | None = None) -> int:
    manifest_path = destination / MANIFEST_NAME
    entries = collect(source_lib, source_dlls, requirements)
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
    if removed:
        for root_name in ROOTS:
            prune_empty_dirs(destination / root_name)

    write_manifest(manifest_path, entries)
    print(f"[python-runtime] deployed {copied} changed file(s), removed {removed} stale file(s), "
          f"{len(entries)} managed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-lib", type=Path, required=True)
    parser.add_argument("--source-dlls", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--requirements", type=Path,
                        help="only deploy site-packages distributions listed in this file")
    args = parser.parse_args()
    requirements = expand(read_requirements(args.requirements)) if args.requirements else None

    for path in (args.source_lib, args.source_dlls):
        if not path.is_dir():
            print(f"[python-runtime] missing source directory: {path}", file=sys.stderr)
            return 2
    args.destination.mkdir(parents=True, exist_ok=True)
    return deploy(args.source_lib, args.source_dlls, args.destination, requirements)


if __name__ == "__main__":
    raise SystemExit(main())

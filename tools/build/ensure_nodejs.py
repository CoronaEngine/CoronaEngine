"""Provision the editor's Node.js toolchain inside its selected Conda environment.

CMake runs this with the project's Python, which may differ from CONDA_PREFIX
in the IDE/terminal. Stdout is reserved for the JSON result consumed by CMake.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


NODE_VERSION = "22.19.0"


def log(message: str) -> None:
    print(f"[Node.js] {message}", file=sys.stderr, flush=True)


def probe_node(prefix: Path) -> Path | None:
    """Find a working, pinned Node/npm pair strictly inside this environment."""
    directories = (
        (prefix, prefix / "Library" / "bin")
        if os.name == "nt"
        else (prefix / "bin",)
    )
    for directory in directories:
        node = directory / ("node.exe" if os.name == "nt" else "node")
        npm = directory / ("npm.cmd" if os.name == "nt" else "npm")
        npm_cli = (
            directory / "node_modules" / "npm" / "bin" / "npm-cli.js"
            if os.name == "nt"
            else prefix / "lib" / "node_modules" / "npm" / "bin" / "npm-cli.js"
        )
        if not all(path.is_file() and path.resolve().is_relative_to(prefix)
                   for path in (node, npm, npm_cli)):
            continue
        env = os.environ.copy()
        env["PATH"] = str(directory) + os.pathsep + env.get("PATH", "")
        try:
            version = subprocess.run(
                [str(node), "--version"], env=env, capture_output=True,
                text=True, check=True, timeout=30,
            ).stdout.strip()
            if version != f"v{NODE_VERSION}":
                continue
            # Use this exact Node rather than allowing an npm shim to find a
            # different system installation.
            npm_version = subprocess.run(
                [str(node), str(npm_cli), "--version"], env=env,
                capture_output=True, text=True, check=True, timeout=30,
            ).stdout.strip()
            if npm_version:
                return directory
        except (OSError, subprocess.SubprocessError):
            continue
    return None


def ensure_nodejs(conda: str, prefix: Path) -> Path:
    prefix = prefix.resolve()
    if not (prefix / "conda-meta").is_dir():
        raise RuntimeError(f"Not a Conda environment: {prefix}")
    if prefix != Path(sys.prefix).resolve():
        raise RuntimeError(
            f"Python belongs to {sys.prefix}, but the requested environment is {prefix}. "
            "Run this checker with the project's Conda Python."
        )

    node_dir = probe_node(prefix)
    if node_dir is not None:
        log(f"Using {NODE_VERSION} from {node_dir}")
        return node_dir

    log(f"Installing nodejs={NODE_VERSION} into {prefix}")
    command = [
        conda, "install", "--yes", "--prefix", str(prefix),
        "--override-channels", "--channel", "conda-forge",
        "--freeze-installed", f"nodejs={NODE_VERSION}",
    ]
    # If Conda already records the requested version, repair missing/broken
    # files instead of accepting its "already installed" shortcut.
    if any((prefix / "conda-meta").glob(f"nodejs-{NODE_VERSION}-*.json")):
        command.append("--force-reinstall")
    subprocess.run(command, stdout=sys.stderr, check=True)

    node_dir = probe_node(prefix)
    if node_dir is None:
        raise RuntimeError(
            f"Conda finished, but Node.js {NODE_VERSION} and npm are not usable in {prefix}."
        )
    log(f"Installed {NODE_VERSION} in {node_dir}")
    return node_dir


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--conda", required=True, help="Conda executable selected by CMake")
    parser.add_argument("--prefix", required=True, type=Path, help="Project Conda environment")
    args = parser.parse_args(argv)
    try:
        node_dir = ensure_nodejs(args.conda, args.prefix)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        log(f"ERROR: {error}")
        return 1
    print(json.dumps({"node_dir": node_dir.as_posix(), "version": NODE_VERSION}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Apply the engine-owned Horizon delta to its pinned source workspace."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess

from build_tools.git_workspace import _workspace_lock
from build_tools.source_workspaces import HORIZON


def _source_hash(path: Path) -> str:
    # Git may check source files out with CRLF on Windows.
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def _git(worktree: Path, *arguments: str, input_text: str | None = None) -> str:
    result = subprocess.run(
        ("git", "-C", str(worktree), *arguments),
        text=True, encoding="utf-8", input=input_text, capture_output=True, check=False,
    )
    if result.returncode:
        raise RuntimeError(f"Horizon runtime patch: git {' '.join(arguments)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def ensure_horizon_runtime_patch(repo_root: Path) -> None:
    """Replay the tracked delta, or validate it without overwriting local edits."""
    repo_root = Path(repo_root).resolve()
    patch_dir = repo_root / "build_tools" / "patches" / "horizon"
    manifest = json.loads((patch_dir / "reblur-runtime.json").read_text(encoding="utf-8"))
    patch = patch_dir / "reblur-runtime.patch"
    patch_bytes = patch.read_bytes().replace(b"\r\n", b"\n")
    if hashlib.sha256(patch_bytes).hexdigest() != manifest["patch_sha256"]:
        raise RuntimeError("Horizon runtime patch checksum mismatch; restore the tracked patch artifact.")
    worktree = repo_root / HORIZON.directory
    with _workspace_lock(repo_root, HORIZON):
        if _git(worktree, "rev-parse", "HEAD") != manifest["base_commit"]:
            raise RuntimeError("Horizon runtime patch baseline differs from HEAD; align the source pin and patch first.")
        actual = {relative: _source_hash(worktree / relative) for relative in manifest["files"]}
        if all(actual[relative] == expected["after_sha256"] for relative, expected in manifest["files"].items()):
            print("[INFO] Horizon engine-owned runtime patch already present; source files unchanged.")
            return
        changed = [relative for relative, expected in manifest["files"].items()
                   if actual[relative] != expected["before_sha256"]]
        if changed:
            raise RuntimeError(
                "Horizon runtime patch found local changes in owned files; all edits were left untouched: "
                + ", ".join(changed)
            )
        # Preflight every hunk before writing any file. Other dirty files are
        # deliberately absent from this patch, including the user's math test.
        # Hashes above already exclude whitespace edits; allow Git's Windows
        # worktree line-ending conversion in otherwise identical context lines.
        patch_text = patch_bytes.decode("utf-8")
        _git(worktree, "apply", "--ignore-space-change", "--check", "-", input_text=patch_text)
        _git(worktree, "apply", "--ignore-space-change", "-", input_text=patch_text)
        if any(_source_hash(worktree / relative) != expected["after_sha256"]
               for relative, expected in manifest["files"].items()):
            raise RuntimeError("Horizon runtime patch output checksum mismatch; inspect the source workspace before building.")
        print("[INFO] Applied the tracked engine-owned Horizon runtime patch.")

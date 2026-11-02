"""Replay the owned Horizon changes without touching a developer checkout."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from build_tools.horizon_runtime_patch import ensure_horizon_runtime_patch
from build_tools.paths import REPO_ROOT
from build_tools.source_workspaces import HORIZON
from build_tools.tests.test_git_workspace import git


def source_tree(tmp_path):
    """Use the actual patch and pinned blobs, with no network or CMake run."""
    root = tmp_path / "engine with spaces"
    horizon = root / "Horizon"
    horizon.mkdir(parents=True)
    git(horizon, "init", "-b", "main")
    patch_dir = root / "build_tools" / "patches" / "horizon"
    patch_dir.mkdir(parents=True)
    actual_dir = REPO_ROOT / "build_tools" / "patches" / "horizon"
    manifest = json.loads((actual_dir / "reblur-runtime.json").read_text())
    for relative in manifest["files"]:
        data = subprocess.check_output([
            "git", "-C", str(REPO_ROOT / "Horizon"),
            "show", f"{HORIZON.commit}:{relative}",
        ])
        target = horizon / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    (horizon / "unrelated-user-file.cpp").write_text("original\n")
    git(horizon, "add", ".")
    git(horizon, "commit", "-m", "local pinned fixture")
    manifest["base_commit"] = git(horizon, "rev-parse", "HEAD")
    (patch_dir / "reblur-runtime.json").write_text(json.dumps(manifest))
    (patch_dir / "reblur-runtime.patch").write_bytes((actual_dir / "reblur-runtime.patch").read_bytes())
    return root, horizon, manifest


def canonical_hash(path):
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


class HorizonRuntimePatchTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root, self.horizon, self.manifest = source_tree(Path(temporary.name))

    def snapshot(self):
        return {relative: (self.horizon / relative).read_bytes() for relative in self.manifest["files"]}

    def test_clean_baseline_replays_actual_patch_and_preserves_unrelated_edits(self):
        user_file = self.horizon / "unrelated-user-file.cpp"
        user_file.write_bytes(b"user changes\r\n")
        ensure_horizon_runtime_patch(self.root)
        self.assertEqual(user_file.read_bytes(), b"user changes\r\n")
        for relative, expected in self.manifest["files"].items():
            self.assertEqual(canonical_hash(self.horizon / relative), expected["after_sha256"])

    def test_already_applied_patch_is_a_no_write_operation(self):
        ensure_horizon_runtime_patch(self.root)
        before = {relative: ((self.horizon / relative).read_bytes(), (self.horizon / relative).stat().st_mtime_ns)
                  for relative in self.manifest["files"]}
        ensure_horizon_runtime_patch(self.root)
        self.assertEqual(before, {relative: ((self.horizon / relative).read_bytes(), (self.horizon / relative).stat().st_mtime_ns)
                                  for relative in self.manifest["files"]})

    def test_windows_crlf_baseline_and_patch_replay_without_corrupting_lines(self):
        git(self.horizon, "config", "core.autocrlf", "true")
        for relative in self.manifest["files"]:
            target = self.horizon / relative
            target.write_bytes(target.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        patch = self.root / "build_tools" / "patches" / "horizon" / "reblur-runtime.patch"
        patch.write_bytes(patch.read_bytes().replace(b"\n", b"\r\n"))
        ensure_horizon_runtime_patch(self.root)
        for relative, expected in self.manifest["files"].items():
            self.assertEqual(canonical_hash(self.horizon / relative), expected["after_sha256"])

    def test_partially_applied_patch_is_rejected_without_further_writes(self):
        relative = next(iter(self.manifest["files"]))
        patch = self.root / "build_tools" / "patches" / "horizon" / "reblur-runtime.patch"
        git(self.horizon, "apply", f"--include={relative}", str(patch))
        before = self.snapshot()
        with self.assertRaisesRegex(RuntimeError, "local changes"):
            ensure_horizon_runtime_patch(self.root)
        self.assertEqual(before, self.snapshot())

    def test_conflicting_user_edit_prevents_all_patch_writes(self):
        target = self.horizon / next(iter(self.manifest["files"]))
        target.write_bytes(target.read_bytes() + b"\n// user edit\n")
        before = self.snapshot()
        with self.assertRaisesRegex(RuntimeError, "local changes"):
            ensure_horizon_runtime_patch(self.root)
        self.assertEqual(before, self.snapshot())

    def test_different_base_commit_is_rejected_without_writes(self):
        git(self.horizon, "commit", "--allow-empty", "-m", "another base")
        before = self.snapshot()
        with self.assertRaisesRegex(RuntimeError, "baseline"):
            ensure_horizon_runtime_patch(self.root)
        self.assertEqual(before, self.snapshot())

    def test_modified_patch_artifact_is_rejected_without_writes(self):
        patch = self.root / "build_tools" / "patches" / "horizon" / "reblur-runtime.patch"
        patch.write_bytes(patch.read_bytes() + b"\n")
        with self.assertRaisesRegex(RuntimeError, "patch checksum"):
            ensure_horizon_runtime_patch(self.root)
        for relative, expected in self.manifest["files"].items():
            self.assertEqual(canonical_hash(self.horizon / relative), expected["before_sha256"])


if __name__ == "__main__":
    unittest.main()

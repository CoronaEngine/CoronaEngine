from __future__ import annotations

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from horizon_workspace import (
    SYNC_COMMAND,
    UPDATE_COMMAND,
    _normalized_url,
    ensure_workspace,
    inspect_workspace,
    sync_workspace,
    update_workspace,
)


GIT_CONFIG = (
    "-c", "user.name=CoronaEngine Tests",
    "-c", "user.email=tests@example.invalid",
    "-c", "commit.gpgsign=false",
)


def git(cwd: Path, *arguments: str) -> str:
    result = subprocess.run(
        ("git", *GIT_CONFIG, "-C", str(cwd), *arguments),
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"git {' '.join(arguments)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


class HorizonWorkspaceTests(unittest.TestCase):
    """Exercises the lock/clone drift handling against a local origin repository."""

    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self._temporary.cleanup)
        self.root = Path(self._temporary.name)
        self.origin = self.root / "origin"
        self.origin.mkdir()
        git(self.origin, "init", "-b", "main")
        (self.origin / "file.txt").write_text("one\n", encoding="utf-8")
        git(self.origin, "add", "file.txt")
        git(self.origin, "commit", "-m", "one")
        self.locked = git(self.origin, "rev-parse", "HEAD")
        (self.origin / "file.txt").write_text("two\n", encoding="utf-8")
        git(self.origin, "commit", "-am", "two")
        self.newer = git(self.origin, "rev-parse", "HEAD")

        self.repo_root = self.root / "CoronaEngine"
        (self.repo_root / ".workspace").mkdir(parents=True)
        self.lock_file = self.repo_root / ".workspace" / "horizon.lock.json"
        self.worktree = self.repo_root / ".workspace" / "Horizon"
        self.write_lock(self.locked)

    def write_lock(self, commit: str) -> None:
        self.lock_file.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "url": self.origin.as_posix(),
                    "ref": "main",
                    "commit": commit,
                }
            ),
            encoding="utf-8",
        )

    def advance_origin(self, content: str) -> str:
        (self.origin / "file.txt").write_text(content, encoding="utf-8")
        git(self.origin, "commit", "-am", content.strip())
        return git(self.origin, "rev-parse", "HEAD")

    def silently(self, function, *arguments):
        with contextlib.redirect_stdout(io.StringIO()):
            return function(*arguments)

    def head(self) -> str:
        return git(self.worktree, "rev-parse", "HEAD")

    def test_install_clones_the_locked_commit(self) -> None:
        self.silently(ensure_workspace, self.repo_root)
        self.assertEqual(self.head(), self.locked)
        state = inspect_workspace(self.repo_root)
        self.assertTrue(state.cloned)
        self.assertTrue(state.in_sync)
        self.assertFalse(state.dirty)
        self.assertFalse(state.needs_repair)
        self.assertIn("matches the lock", state.summary())

    def test_missing_clone_is_reported_without_raising(self) -> None:
        state = inspect_workspace(self.repo_root)
        self.assertFalse(state.cloned)
        self.assertIn("missing", state.summary())
        self.assertNotIn("Horizon fix:", state.summary())

    def test_drift_fails_with_actionable_commands(self) -> None:
        self.silently(ensure_workspace, self.repo_root)
        git(self.worktree, "checkout", "--detach", self.newer)
        with self.assertRaises(RuntimeError) as context:
            ensure_workspace(self.repo_root)
        message = str(context.exception)
        self.assertIn(SYNC_COMMAND, message)
        self.assertIn(UPDATE_COMMAND, message)
        state = inspect_workspace(self.repo_root)
        self.assertTrue(state.needs_repair)
        self.assertIn("ahead", state.drift)
        self.assertIn(SYNC_COMMAND, state.summary())

    def test_sync_restores_the_locked_commit(self) -> None:
        self.silently(ensure_workspace, self.repo_root)
        git(self.worktree, "checkout", "--detach", self.newer)
        self.silently(sync_workspace, self.repo_root)
        self.assertEqual(self.head(), self.locked)
        self.assertTrue(inspect_workspace(self.repo_root).in_sync)

    def test_sync_is_a_no_op_when_already_locked(self) -> None:
        self.silently(ensure_workspace, self.repo_root)
        self.silently(sync_workspace, self.repo_root)
        self.assertEqual(self.head(), self.locked)

    def test_sync_refuses_local_changes(self) -> None:
        self.silently(ensure_workspace, self.repo_root)
        git(self.worktree, "checkout", "--detach", self.newer)
        (self.worktree / "file.txt").write_text("local work\n", encoding="utf-8")
        with self.assertRaises(RuntimeError) as context:
            sync_workspace(self.repo_root)
        self.assertIn("local changes", str(context.exception))
        self.assertEqual(self.head(), self.newer)

    def test_dirty_workspace_is_allowed_at_the_locked_commit(self) -> None:
        self.silently(ensure_workspace, self.repo_root)
        (self.worktree / "file.txt").write_text("local work\n", encoding="utf-8")
        self.silently(ensure_workspace, self.repo_root)
        state = inspect_workspace(self.repo_root)
        self.assertTrue(state.in_sync)
        self.assertTrue(state.dirty)
        self.assertFalse(state.needs_repair)

    def test_update_accepts_a_drifted_clean_workspace(self) -> None:
        self.silently(ensure_workspace, self.repo_root)
        git(self.worktree, "checkout", "--detach", self.newer)
        newest = self.advance_origin("three\n")
        lock = self.silently(update_workspace, self.repo_root)
        self.assertEqual(lock.commit, newest)
        self.assertEqual(self.head(), newest)
        self.assertTrue(inspect_workspace(self.repo_root).in_sync)

    def test_rewritten_ssh_origin_still_matches_its_https_lock(self) -> None:
        """A global `insteadOf` rule may print the origin as SSH; that is not drift."""
        self.silently(ensure_workspace, self.repo_root)
        # Clone through the local fixture, then pin the lock to the HTTPS spelling the
        # project publishes while the remote reports the SSH one Git rewrites it into.
        git(self.worktree, "remote", "set-url", "origin",
            "https://github.com/CoronaEngine/Horizon.git")
        git(self.worktree, "config", "--local",
            "url.git@github.com:.insteadOf", "https://github.com/")
        self.assertEqual(git(self.worktree, "remote", "get-url", "origin"),
                         "git@github.com:CoronaEngine/Horizon.git")
        self.lock_file.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "url": "https://github.com/CoronaEngine/Horizon.git",
                    "ref": "main",
                    "commit": self.locked,
                }
            ),
            encoding="utf-8",
        )
        state = inspect_workspace(self.repo_root)
        self.assertTrue(state.url_matches)
        self.assertFalse(state.needs_repair)
        self.assertNotIn("Horizon fix:", state.summary())
        self.silently(ensure_workspace, self.repo_root)

    def test_url_matching_ignores_transport_spelling(self) -> None:
        expected = "https://github.com/CoronaEngine/Horizon.git"
        for remote in (
            "https://github.com/CoronaEngine/Horizon.git",
            "https://github.com/CoronaEngine/Horizon",
            "https://github.com/CoronaEngine/Horizon/",
            "git@github.com:CoronaEngine/Horizon.git",
            "git@github.com:CoronaEngine/Horizon",
            "https://GitHub.com/coronaengine/horizon.git",
        ):
            with self.subTest(remote=remote):
                self.assertEqual(_normalized_url(remote), _normalized_url(expected))
        self.assertNotEqual(
            _normalized_url("git@github.com:Other/Horizon.git"), _normalized_url(expected)
        )
        self.assertNotEqual(
            _normalized_url("https://gitlab.com/CoronaEngine/Horizon.git"),
            _normalized_url(expected),
        )

    def test_local_path_lock_still_compares_unchanged(self) -> None:
        """Non-URL remotes (used by the test fixtures) keep working."""
        self.silently(ensure_workspace, self.repo_root)
        state = inspect_workspace(self.repo_root)
        self.assertTrue(state.url_matches)
        self.assertTrue(state.in_sync)


if __name__ == "__main__":
    unittest.main()

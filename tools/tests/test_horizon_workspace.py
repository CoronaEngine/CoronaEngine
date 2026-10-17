"""Tests for the Horizon workspace handling in ``tools/horizon_workspace.py``.

The workspace is treated as an ordinary Git clone: builds report its state and
always proceed, and the lock file is a hand-edited starting point rather than an
authority. These tests exercise that contract against a local origin repository,
so they need no network access.
"""

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

from horizon_workspace import (  # noqa: E402
    SYNC_COMMAND,
    ensure_workspace,
    inspect_workspace,
    sync_workspace,
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


def git_probe(cwd: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ("git", *GIT_CONFIG, "-C", str(cwd), *arguments),
        text=True,
        capture_output=True,
        check=False,
    )


class HorizonWorkspaceTests(unittest.TestCase):
    """Exercises the ordinary-clone contract against a local origin repository."""

    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self._temporary.cleanup)
        self.root = Path(self._temporary.name)

        # ``origin`` is a bare repository, exactly as a real remote is, so the
        # workspace can push to it. ``seed`` is the working clone used to author
        # the commits that advance origin.
        self.seed = self.root / "seed"
        self.seed.mkdir()
        git(self.seed, "init", "-b", "main")
        (self.seed / "file.txt").write_text("one\n", encoding="utf-8")
        git(self.seed, "add", "file.txt")
        git(self.seed, "commit", "-m", "one")
        self.locked = git(self.seed, "rev-parse", "HEAD")

        self.origin = self.root / "origin.git"
        git(self.root, "clone", "--bare", self.seed.as_posix(), self.origin.as_posix())
        git(self.seed, "remote", "add", "upstream", self.origin.as_posix())

        (self.seed / "file.txt").write_text("two\n", encoding="utf-8")
        git(self.seed, "commit", "-am", "two")
        git(self.seed, "push", "upstream", "main")
        self.newer = git(self.seed, "rev-parse", "HEAD")

        self.repo_root = self.root / "CoronaEngine"
        (self.repo_root / ".workspace").mkdir(parents=True)
        self.lock_file = self.repo_root / ".workspace" / "horizon.lock.json"
        self.worktree = self.repo_root / ".workspace" / "Horizon"
        self.write_lock(self.locked)

    # -- helpers ---------------------------------------------------------------

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
        """Commit ``content`` onto origin/main, as a Horizon contributor would."""
        (self.seed / "file.txt").write_text(content, encoding="utf-8")
        git(self.seed, "commit", "-am", content.strip())
        git(self.seed, "push", "upstream", "main")
        return git(self.seed, "rev-parse", "HEAD")

    def advance_origin_on_a_side_line(self, content: str) -> str:
        """Advance origin/main on a line of history the workspace is not on.

        Real remotes do not always move straight ahead of one clone, and that is
        the case worth protecting: the lock can name a commit that does not have
        the developer's branch behind it.
        """
        (self.seed / "file.txt").write_text(content, encoding="utf-8")
        git(self.seed, "add", "file.txt")
        tree = git(self.seed, "write-tree")
        commit = subprocess.run(
            (
                "git", *GIT_CONFIG, "-C", str(self.seed),
                "commit-tree", tree, "-p", self.newer, "-m", content.strip(),
            ),
            text=True,
            capture_output=True,
            check=True,
        ).stdout.strip()
        git(self.seed, "push", "upstream", f"{commit}:refs/heads/main")
        # Leave the authoring clone where it was: the line that was pushed lives
        # on origin only, which is exactly the divergence being tested.
        git(self.seed, "checkout", "--quiet", "--force", "main")
        return commit

    def origin_subject(self) -> str:
        return git(self.origin, "log", "-1", "--format=%s", "main")

    def silently(self, function, *arguments):
        with contextlib.redirect_stdout(io.StringIO()) as buffer:
            return function(*arguments), buffer.getvalue()

    def clone(self) -> None:
        self.silently(ensure_workspace, self.repo_root)

    def head(self) -> str:
        return git(self.worktree, "rev-parse", "HEAD")

    def branch(self) -> str:
        """The checked-out branch, or "" when HEAD is detached."""
        result = subprocess.run(
            ("git", "-C", str(self.worktree), "symbolic-ref", "--quiet", "--short", "HEAD"),
            text=True,
            capture_output=True,
            check=False,
        )
        return result.stdout.strip()

    def assert_workspace_unchanged(self, snapshot: tuple[str, str, str]) -> None:
        """Assert HEAD, refs and the working tree index are exactly as before."""
        self.assertEqual(self.head(), snapshot[0])
        self.assertEqual(git(self.worktree, "for-each-ref", "--format=%(refname)"), snapshot[1])
        self.assertEqual(git(self.worktree, "status", "--porcelain"), snapshot[2])

    def snapshot(self) -> tuple[str, str, str]:
        return (
            self.head(),
            git(self.worktree, "for-each-ref", "--format=%(refname)"),
            git(self.worktree, "status", "--porcelain"),
        )

    # -- clone creates an ordinary clone --------------------------------------

    def test_clone_lands_on_the_locked_commit_checked_out_on_a_branch(self) -> None:
        self.clone()
        self.assertEqual(self.head(), self.locked)
        self.assertEqual(git(self.worktree, "symbolic-ref", "--short", "HEAD"), "main")
        self.assertEqual(
            git(self.worktree, "rev-parse", "--abbrev-ref", "main@{upstream}"), "origin/main"
        )

    def test_clone_allows_local_commits_to_be_made_and_pushed(self) -> None:
        """The point of the workspace: edit Horizon from inside CoronaEngine."""
        self.clone()
        (self.worktree / "horizon_feature.cpp").write_text("// local horizon work\n", encoding="utf-8")
        git(self.worktree, "add", "horizon_feature.cpp")
        git(self.worktree, "commit", "-m", "local horizon work")
        local = self.head()
        self.assertNotEqual(local, self.locked)
        # A plain push is rejected only because this clone rests on the locked
        # commit, which trails origin/main. Publishing a Horizon change therefore
        # starts by integrating; a fetch is enough to see the newer tip.
        git(self.worktree, "fetch", "origin", "main")
        git(self.worktree, "rebase", "origin/main")
        git(self.worktree, "push", "origin", "main")
        self.assertEqual(self.origin_subject(), "local horizon work")
        self.assertEqual(git(self.origin, "rev-parse", "main^"), self.newer)

    def test_missing_clone_is_reported_without_raising(self) -> None:
        status = inspect_workspace(self.repo_root)
        self.assertFalse(status.cloned)
        self.assertIn("missing", status.summary())
        self.assertEqual(status.advice(), ())

    def test_workspace_path_that_is_not_a_git_checkout_is_fatal(self) -> None:
        self.worktree.mkdir(parents=True)
        with self.assertRaises(RuntimeError) as context:
            ensure_workspace(self.repo_root)
        self.assertIn("not a Git checkout", str(context.exception))

    def test_unreadable_lock_is_reported_but_an_existing_clone_still_builds(self) -> None:
        """A corrupt lock must not fail a build: it is only needed to align by."""
        self.clone()
        self.lock_file.write_text("{ not json", encoding="utf-8")
        status, output = self.silently(ensure_workspace, self.repo_root)
        self.assertTrue(status.cloned)
        self.assertIsNone(status.lock)
        self.assertIn("unreadable", status.summary())
        self.assertIn("lock", " ".join(status.advice()))
        self.assertIn("[WARN]", output)
        self.assertEqual(self.head(), self.locked)

    def test_sync_with_an_unreadable_lock_says_why_it_cannot_align(self) -> None:
        self.clone()
        self.lock_file.write_text("{ not json", encoding="utf-8")
        with self.assertRaises(RuntimeError) as context:
            self.silently(sync_workspace, self.repo_root)
        self.assertIn("Cannot sync Horizon", str(context.exception))

    def test_missing_lock_is_fatal_only_when_the_clone_must_be_created(self) -> None:
        self.lock_file.unlink()
        with self.assertRaises(RuntimeError) as context:
            ensure_workspace(self.repo_root)
        self.assertIn("does not exist", str(context.exception))
        # With a clone present, the same missing lock is merely reported.
        self.write_lock(self.locked)
        self.clone()
        self.lock_file.unlink()
        status, _ = self.silently(ensure_workspace, self.repo_root)
        self.assertTrue(status.cloned)
        self.assertIsNone(status.lock)

    # -- drift is reported, never fatal ---------------------------------------

    def test_local_commit_ahead_of_the_lock_still_builds(self) -> None:
        self.clone()
        git(self.worktree, "checkout", "--detach", self.newer)
        status, output = self.silently(ensure_workspace, self.repo_root)
        self.assertTrue(status.cloned)
        self.assertFalse(status.in_sync)
        self.assertFalse(status.dirty)
        self.assertIn("ahead", status.drift)
        self.assertIn("[WARN]", output)
        self.assertEqual(self.head(), self.newer)

    def test_behind_the_lock_still_builds(self) -> None:
        self.clone()
        git(self.worktree, "checkout", "--detach", self.locked)
        third = self.advance_origin("three\n")
        # A plain fetch is what a developer would run; it brings the commit into
        # the object database without touching the checkout.
        git(self.worktree, "fetch", "origin", "main")
        self.write_lock(third)
        status, output = self.silently(ensure_workspace, self.repo_root)
        self.assertTrue(status.cloned)
        self.assertFalse(status.in_sync)
        self.assertIn("behind", status.drift)
        self.assertIn("the build continues", output)
        self.assertEqual(self.head(), self.locked)

    def test_diverged_from_the_lock_still_builds(self) -> None:
        self.clone()
        (self.worktree / "file.txt").write_text("local branch of history\n", encoding="utf-8")
        git(self.worktree, "commit", "-am", "local branch of history")
        # A commit the clone already knows, so this is genuine divergence rather
        # than "the locked commit has not been fetched yet".
        self.write_lock(self.newer)
        status, output = self.silently(ensure_workspace, self.repo_root)
        self.assertFalse(status.in_sync)
        self.assertIn("diverged", status.drift)
        self.assertIn("[WARN]", output)

    def test_unexpected_origin_is_reported_without_raising(self) -> None:
        self.clone()
        other = self.root / "other-origin.git"
        git(self.root, "init", "--bare", "--initial-branch=main", other.as_posix())
        git(self.worktree, "remote", "set-url", "origin", other.as_posix())
        status, output = self.silently(ensure_workspace, self.repo_root)
        self.assertFalse(status.url_matches)
        self.assertTrue(status.cloned)
        self.assertIn("origin", output.lower())

    # -- uncommitted work is the supported way to work ------------------------

    def test_dirty_workspace_builds_and_is_left_untouched(self) -> None:
        self.clone()
        (self.worktree / "file.txt").write_text("work in progress\n", encoding="utf-8")
        (self.worktree / "untracked.txt").write_text("scratch\n", encoding="utf-8")
        before = self.snapshot()
        status, _ = self.silently(ensure_workspace, self.repo_root)
        self.assertTrue(status.dirty)
        self.assertTrue(status.in_sync)
        self.assertEqual(len(status.modified), 1)
        self.assertEqual(len(status.untracked), 1)
        # The report shows the path itself, not the porcelain status prefix.
        self.assertIn("file.txt", status.summary())
        self.assertIn("untracked.txt", status.summary())
        self.assert_workspace_unchanged(before)

    def test_a_renamed_file_is_reported_by_its_new_name(self) -> None:
        self.clone()
        git(self.worktree, "mv", "file.txt", "renamed.txt")
        status, _ = self.silently(ensure_workspace, self.repo_root)
        self.assertTrue(status.dirty)
        self.assertIn("renamed.txt", status.summary())
        self.assertNotIn("file.txt", status.summary())

    def test_dirty_workspace_ahead_of_the_lock_still_builds_untouched(self) -> None:
        """The headline case: Horizon edited here, lock already advanced elsewhere."""
        self.clone()
        (self.worktree / "file.txt").write_text("horizon edit\n", encoding="utf-8")
        git(self.worktree, "commit", "-am", "horizon edit")
        (self.worktree / "file.txt").write_text("horizon edit plus wip\n", encoding="utf-8")
        self.write_lock(self.advance_origin("upstream moved on\n"))
        before = self.snapshot()
        status, output = self.silently(ensure_workspace, self.repo_root)
        self.assertTrue(status.dirty)
        self.assertFalse(status.in_sync)
        self.assertIn("compiled as-is", output)
        self.assert_workspace_unchanged(before)

    def test_detached_head_is_reported_with_advice(self) -> None:
        self.clone()
        git(self.worktree, "checkout", "--detach", self.locked)
        status, output = self.silently(ensure_workspace, self.repo_root)
        self.assertTrue(status.detached)
        self.assertIn("detached HEAD", status.summary())
        self.assertIn("switch -c", output)

    def test_branch_behind_its_upstream_is_reported(self) -> None:
        """A fresh clone rests on the locked commit, which may trail origin/main."""
        self.clone()
        status, output = self.silently(ensure_workspace, self.repo_root)
        self.assertEqual(status.branch, "main")
        self.assertTrue(status.upstream_configured)
        self.assertEqual(status.behind, 1)
        self.assertIn("origin/main", output)
        self.assertIn("1 behind", output)

    def test_upstream_ahead_is_reported(self) -> None:
        self.clone()
        (self.worktree / "file.txt").write_text("unpushed\n", encoding="utf-8")
        git(self.worktree, "commit", "-am", "unpushed")
        status, output = self.silently(ensure_workspace, self.repo_root)
        self.assertEqual(status.ahead, 1)
        self.assertIn("1 ahead", output)

    # -- horizon-sync only moves HEAD -----------------------------------------

    def test_sync_adopts_a_hand_edited_lock(self) -> None:
        self.clone()
        newest = self.advance_origin("three\n")
        self.write_lock(newest)
        self.silently(sync_workspace, self.repo_root)
        self.assertEqual(self.head(), newest)
        self.assertTrue(inspect_workspace(self.repo_root).in_sync)

    def test_sync_is_a_no_op_when_already_locked(self) -> None:
        self.clone()
        before = self.snapshot()
        _, output = self.silently(sync_workspace, self.repo_root)
        self.assertIn("already at the locked commit", output)
        self.assert_workspace_unchanged(before)

    def test_sync_never_writes_the_lock_file(self) -> None:
        self.clone()
        before = self.lock_file.read_bytes()
        self.advance_origin("three\n")
        self.silently(sync_workspace, self.repo_root)
        self.assertEqual(self.lock_file.read_bytes(), before)

    def test_sync_keeps_uncommitted_work_and_refuses_to_overwrite_it(self) -> None:
        """Git protects the edit; the tooling neither stashes nor discards it."""
        self.clone()
        (self.worktree / "file.txt").write_text("precious local edit\n", encoding="utf-8")
        newest = self.advance_origin("three\n")
        self.write_lock(newest)
        with self.assertRaises(RuntimeError) as context:
            self.silently(sync_workspace, self.repo_root)
        # The message must explain that the refusal is protective, in Git's own words.
        message = str(context.exception)
        self.assertIn("left untouched", message)
        self.assertIn("local changes", message)
        self.assertEqual(
            (self.worktree / "file.txt").read_text(encoding="utf-8"), "precious local edit\n"
        )

    def test_sync_keeps_local_commits_reachable_and_moves_only_head(self) -> None:
        """The common case: local work tree-committed at the lock, origin ahead.

        The two commits are siblings, so the clone's branch cannot fast-forward.
        Sync moves HEAD to the lock and leaves the branch -- and the work on it --
        exactly where it was.
        """
        self.clone()
        (self.worktree / "file.txt").write_text("local horizon work\n", encoding="utf-8")
        git(self.worktree, "commit", "-am", "local horizon work")
        local = self.head()
        newest = self.advance_origin("three\n")
        self.write_lock(newest)
        self.silently(sync_workspace, self.repo_root)
        self.assertEqual(self.head(), newest)
        self.assertEqual(git(self.worktree, "rev-parse", "refs/heads/main"), local)
        # Still reachable from the branch that holds it, not merely present as an
        # object waiting for garbage collection.
        self.assertEqual(
            git(self.worktree, "merge-base", "--is-ancestor", local, "refs/heads/main"), ""
        )

    def test_sync_reports_a_lock_commit_that_is_not_reachable(self) -> None:
        self.clone()
        self.write_lock("b" * 40)
        with self.assertRaises(RuntimeError) as context:
            self.silently(sync_workspace, self.repo_root)
        self.assertIn("not reachable", str(context.exception))

    def test_sync_leaves_the_workspace_on_a_branch_not_a_detached_head(self) -> None:
        """A detached HEAD is the very state the report warns about, so sync avoids it."""
        self.clone()
        git(self.worktree, "checkout", "--detach", self.locked)
        self.write_lock(self.advance_origin("three\n"))
        status, output = self.silently(sync_workspace, self.repo_root)
        self.assertFalse(status.detached)
        self.assertEqual(self.branch(), "main")
        self.assertIn("main", output)

    def test_sync_fast_forwards_the_lock_branch_over_a_behind_tip(self) -> None:
        self.clone()
        newest = self.advance_origin("three\n")
        self.write_lock(newest)
        _, output = self.silently(sync_workspace, self.repo_root)
        self.assertIn("fast-forwarded", output)
        self.assertEqual(git(self.worktree, "rev-parse", "refs/heads/main"), newest)
        self.assertEqual(self.branch(), "main")

    def test_sync_never_resets_a_branch_holding_local_commits(self) -> None:
        """Commits the lock does not contain keep their branch; only HEAD moves."""
        self.clone()
        (self.worktree / "file.txt").write_text("my work\n", encoding="utf-8")
        git(self.worktree, "commit", "-am", "my work")
        local = self.head()
        # Origin moves onto a line the local commit is not on, so reassigning the
        # branch to the lock really would push that commit off it.
        newest = self.advance_origin_on_a_side_line("three\n")
        self.write_lock(newest)
        status, output = self.silently(sync_workspace, self.repo_root)
        self.assertEqual(status.head, newest)
        self.assertEqual(git(self.worktree, "rev-parse", "refs/heads/main"), local)
        self.assertTrue(status.detached)
        self.assertIn("branch 'main' was left", output)
        self.assertEqual(git(self.worktree, "rev-parse", "--verify", "--quiet", local), local)

    def test_sync_command_constant_names_the_documented_entry_point(self) -> None:
        self.assertIn("horizon-sync", SYNC_COMMAND)

if __name__ == "__main__":
    unittest.main()

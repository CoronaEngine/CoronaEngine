"""Shared workspace contract, exercised for both dependencies with local Git remotes."""

from __future__ import annotations

import contextlib
import io
import os
from dataclasses import replace
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from build_tools.git_workspace import ensure_workspace, inspect_workspace, sync_workspace, _workspace_lock, _clone_locked
from build_tools import dev, git_workspace
from build_tools.source_workspaces import HORIZON, EDITOR
from build_tools.workflow import CommandError


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


class WorkspaceContract:
    template = HORIZON
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
        self.repo_root.mkdir()
        self.workspace = replace(self.template, url=self.origin.as_posix(), commit=self.locked)
        self.worktree = self.repo_root / self.workspace.directory

    # -- helpers ---------------------------------------------------------------

    def set_baseline(self, commit: str) -> None:
        self.workspace = replace(self.workspace, commit=commit)

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
        self.silently(ensure_workspace, self.repo_root, self.workspace)

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

    def test_failed_clone_cleans_git_objects_and_allows_retry(self) -> None:
        self.set_baseline("b" * 40)
        with self.assertRaises(CommandError):
            self.clone()
        self.assertFalse(self.worktree.exists())
        self.set_baseline(self.locked)
        self.clone()
        self.assertEqual(self.head(), self.locked)
        self.assertTrue((self.worktree / "file.txt").is_file())

    def test_clone_lands_on_the_locked_commit_checked_out_on_a_branch(self) -> None:
        # Record real Git commands: the clone must target the final root path,
        # with no later rename or temporary source directory.
        with mock.patch.object(git_workspace, "run_command", wraps=git_workspace.run_command) as commands, \
                mock.patch.object(Path, "rename", side_effect=AssertionError("Clone must not rename sources")):
            self.clone()
        clones = [call.args[0] for call in commands.call_args_list if tuple(call.args[0][:2]) == ("git", "clone")]
        self.assertEqual(len(clones), 1)
        self.assertEqual(Path(clones[0][-1]), self.worktree)
        self.assertEqual(self.worktree.parent, self.repo_root)
        self.assertEqual(
            {entry.name for entry in (self.repo_root / "build" / ".locks").iterdir()},
            {f"{self.workspace.key}.lock"},
        )
        self.assertFalse((self.repo_root / ".workspace").exists())
        self.assertEqual(self.head(), self.locked)
        self.assertEqual(git(self.worktree, "symbolic-ref", "--short", "HEAD"), "main")
        self.assertEqual(
            git(self.worktree, "rev-parse", "--abbrev-ref", "main@{upstream}"), "origin/main"
        )

    def test_clone_refuses_to_clean_an_existing_destination(self) -> None:
        self.worktree.mkdir()
        existing = self.worktree / "local.txt"
        existing.write_text("keep local work", encoding="utf-8")
        with self.assertRaises(FileExistsError):
            _clone_locked(self.repo_root, self.worktree, self.workspace)
        self.assertEqual(existing.read_text(encoding="utf-8"), "keep local work")

    def test_interrupted_clone_cleans_only_its_new_directory(self) -> None:
        with mock.patch.object(git_workspace, "run_command", side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.clone()
        self.assertFalse(self.worktree.exists())

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
        status = inspect_workspace(self.repo_root, self.workspace)
        self.assertFalse(status.cloned)
        self.assertIn("missing", status.summary())
        self.assertEqual(status.advice(), ())

    def test_workspace_path_that_is_not_a_git_checkout_is_fatal(self) -> None:
        self.worktree.mkdir(parents=True)
        with self.assertRaises(RuntimeError) as context:
            ensure_workspace(self.repo_root, self.workspace)
        self.assertIn("not a Git checkout", str(context.exception))

    # -- drift is reported, never fatal ---------------------------------------

    def test_local_commit_ahead_of_the_lock_still_builds(self) -> None:
        self.clone()
        git(self.worktree, "checkout", "--detach", self.newer)
        status, output = self.silently(ensure_workspace, self.repo_root, self.workspace)
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
        self.set_baseline(third)
        status, output = self.silently(ensure_workspace, self.repo_root, self.workspace)
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
        self.set_baseline(self.newer)
        status, output = self.silently(ensure_workspace, self.repo_root, self.workspace)
        self.assertFalse(status.in_sync)
        self.assertIn("diverged", status.drift)
        self.assertIn("[WARN]", output)

    def test_unexpected_origin_is_reported_without_raising(self) -> None:
        self.clone()
        other = self.root / "other-origin.git"
        git(self.root, "init", "--bare", "--initial-branch=main", other.as_posix())
        git(self.worktree, "remote", "set-url", "origin", other.as_posix())
        status, output = self.silently(ensure_workspace, self.repo_root, self.workspace)
        self.assertFalse(status.url_matches)
        self.assertTrue(status.cloned)
        self.assertIn("origin", output.lower())

    # -- uncommitted work is the supported way to work ------------------------

    def test_dirty_workspace_builds_and_is_left_untouched(self) -> None:
        self.clone()
        (self.worktree / "file.txt").write_text("work in progress\n", encoding="utf-8")
        (self.worktree / "untracked.txt").write_text("scratch\n", encoding="utf-8")
        before = self.snapshot()
        status, _ = self.silently(ensure_workspace, self.repo_root, self.workspace)
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
        status, _ = self.silently(ensure_workspace, self.repo_root, self.workspace)
        self.assertTrue(status.dirty)
        self.assertIn("renamed.txt", status.summary())
        self.assertNotIn("file.txt", status.summary())

    def test_dirty_workspace_ahead_of_the_lock_still_builds_untouched(self) -> None:
        """The headline case: Horizon edited here, lock already advanced elsewhere."""
        self.clone()
        (self.worktree / "file.txt").write_text("horizon edit\n", encoding="utf-8")
        git(self.worktree, "commit", "-am", "horizon edit")
        (self.worktree / "file.txt").write_text("horizon edit plus wip\n", encoding="utf-8")
        self.set_baseline(self.advance_origin("upstream moved on\n"))
        before = self.snapshot()
        status, output = self.silently(ensure_workspace, self.repo_root, self.workspace)
        self.assertTrue(status.dirty)
        self.assertFalse(status.in_sync)
        self.assertIn("used as-is", output)
        self.assert_workspace_unchanged(before)

    def test_detached_head_is_reported_with_advice(self) -> None:
        self.clone()
        git(self.worktree, "checkout", "--detach", self.locked)
        status, output = self.silently(ensure_workspace, self.repo_root, self.workspace)
        self.assertTrue(status.detached)
        self.assertIn("detached HEAD", status.summary())
        self.assertIn("switch -c", output)

    def test_branch_behind_its_upstream_is_reported(self) -> None:
        """A fresh clone rests on the locked commit, which may trail origin/main."""
        self.clone()
        status, output = self.silently(ensure_workspace, self.repo_root, self.workspace)
        self.assertEqual(status.branch, "main")
        self.assertTrue(status.upstream_configured)
        self.assertEqual(status.behind, 1)
        self.assertIn("origin/main", output)
        self.assertIn("1 behind", output)

    def test_upstream_ahead_is_reported(self) -> None:
        self.clone()
        (self.worktree / "file.txt").write_text("unpushed\n", encoding="utf-8")
        git(self.worktree, "commit", "-am", "unpushed")
        status, output = self.silently(ensure_workspace, self.repo_root, self.workspace)
        self.assertEqual(status.ahead, 1)
        self.assertIn("1 ahead", output)

    # -- explicit sync adopts the code baseline -------------------------------

    def test_sync_adopts_an_updated_code_baseline(self) -> None:
        self.clone()
        newest = self.advance_origin("three\n")
        self.set_baseline(newest)
        self.silently(sync_workspace, self.repo_root, self.workspace)
        self.assertEqual(self.head(), newest)
        self.assertTrue(inspect_workspace(self.repo_root, self.workspace).in_sync)

    def test_sync_is_a_no_op_when_already_locked(self) -> None:
        self.clone()
        before = self.snapshot()
        _, output = self.silently(sync_workspace, self.repo_root, self.workspace)
        self.assertIn("already at the locked commit", output)
        self.assert_workspace_unchanged(before)

    def test_sync_does_not_advance_the_code_baseline_to_remote_tip(self) -> None:
        self.clone()
        baseline = self.workspace
        self.advance_origin("three\n")
        self.silently(sync_workspace, self.repo_root, self.workspace)
        self.assertEqual(self.workspace, baseline)
        self.assertEqual(self.head(), baseline.commit)

    def test_sync_keeps_uncommitted_work_and_refuses_to_overwrite_it(self) -> None:
        """Git protects the edit; the tooling neither stashes nor discards it."""
        self.clone()
        (self.worktree / "file.txt").write_text("precious local edit\n", encoding="utf-8")
        newest = self.advance_origin("three\n")
        self.set_baseline(newest)
        with self.assertRaises(RuntimeError) as context:
            self.silently(sync_workspace, self.repo_root, self.workspace)
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
        self.set_baseline(newest)
        self.silently(sync_workspace, self.repo_root, self.workspace)
        self.assertEqual(self.head(), newest)
        self.assertEqual(git(self.worktree, "rev-parse", "refs/heads/main"), local)
        # Still reachable from the branch that holds it, not merely present as an
        # object waiting for garbage collection.
        self.assertEqual(
            git(self.worktree, "merge-base", "--is-ancestor", local, "refs/heads/main"), ""
        )

    def test_sync_reports_a_lock_commit_that_is_not_reachable(self) -> None:
        self.clone()
        self.set_baseline("b" * 40)
        with self.assertRaises(RuntimeError) as context:
            self.silently(sync_workspace, self.repo_root, self.workspace)
        self.assertIn("not reachable", str(context.exception))

    def test_sync_leaves_the_workspace_on_a_branch_not_a_detached_head(self) -> None:
        """A detached HEAD is the very state the report warns about, so sync avoids it."""
        self.clone()
        git(self.worktree, "checkout", "--detach", self.locked)
        self.set_baseline(self.advance_origin("three\n"))
        status, output = self.silently(sync_workspace, self.repo_root, self.workspace)
        self.assertFalse(status.detached)
        self.assertEqual(self.branch(), "main")
        self.assertIn("main", output)

    def test_sync_fast_forwards_the_lock_branch_over_a_behind_tip(self) -> None:
        self.clone()
        newest = self.advance_origin("three\n")
        self.set_baseline(newest)
        _, output = self.silently(sync_workspace, self.repo_root, self.workspace)
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
        self.set_baseline(newest)
        status, output = self.silently(sync_workspace, self.repo_root, self.workspace)
        self.assertEqual(status.head, newest)
        self.assertEqual(git(self.worktree, "rev-parse", "refs/heads/main"), local)
        self.assertTrue(status.detached)
        self.assertIn("branch 'main' was left", output)
        self.assertEqual(git(self.worktree, "rev-parse", "--verify", "--quiet", local), local)


    def test_existing_shallow_clone_can_sync_to_an_older_baseline(self) -> None:
        git(self.repo_root, "clone", "--depth", "1", self.origin.as_uri(), str(self.worktree))
        self.assertEqual(git(self.worktree, "rev-parse", "--is-shallow-repository"), "true")
        before = self.snapshot()
        self.silently(ensure_workspace, self.repo_root, self.workspace)
        self.assert_workspace_unchanged(before)
        self.silently(sync_workspace, self.repo_root, self.workspace)
        self.assertEqual(self.head(), self.locked)
        self.assertEqual(git(self.worktree, "rev-parse", "refs/heads/main"), self.newer)

    def add_submodule(self) -> tuple[str, str]:
        self.subseed = self.root / "subseed"
        self.subseed.mkdir()
        git(self.subseed, "init", "-b", "main")
        (self.subseed / "module.txt").write_text("one\n", encoding="utf-8")
        git(self.subseed, "add", ".")
        git(self.subseed, "commit", "-m", "module one")
        first = git(self.subseed, "rev-parse", "HEAD")
        git(self.seed, "-c", "protocol.file.allow=always", "submodule", "add",
            self.subseed.as_posix(), "plugins/module")
        git(self.seed, "commit", "-am", "add submodule")
        git(self.seed, "push", "upstream", "main")
        self.set_baseline(git(self.seed, "rev-parse", "HEAD"))
        (self.subseed / "module.txt").write_text("two\n", encoding="utf-8")
        git(self.subseed, "commit", "-am", "module two")
        second = git(self.subseed, "rev-parse", "HEAD")
        # Permit file transport only within these offline test fixtures.
        self.enterContext(mock.patch.dict(os.environ, {"GIT_ALLOW_PROTOCOL": "file"}))
        return first, second

    def test_clone_initializes_pinned_submodule_and_build_leaves_edits_untouched(self) -> None:
        first, second = self.add_submodule()
        self.clone()
        module = self.worktree / "plugins/module"
        self.assertEqual(git(module, "rev-parse", "HEAD"), first)
        git(module, "checkout", "--detach", second)
        (module / "module.txt").write_text("local module work\n", encoding="utf-8")
        self.silently(ensure_workspace, self.repo_root, self.workspace)
        self.assertEqual(git(module, "rev-parse", "HEAD"), second)
        self.assertEqual((module / "module.txt").read_text(), "local module work\n")

    def test_sync_updates_submodule_when_parent_head_already_matches(self) -> None:
        first, second = self.add_submodule()
        self.clone()
        module = self.worktree / "plugins/module"
        git(module, "checkout", "--detach", second)
        self.silently(sync_workspace, self.repo_root, self.workspace)
        self.assertEqual(git(module, "rev-parse", "HEAD"), first)

    def test_sync_refuses_to_overwrite_submodule_edits_and_can_retry(self) -> None:
        first, second = self.add_submodule()
        self.clone()
        module = self.worktree / "plugins/module"
        git(module, "checkout", "--detach", second)
        (module / "module.txt").write_text("precious module work\n", encoding="utf-8")
        with self.assertRaises(CommandError):
            self.silently(sync_workspace, self.repo_root, self.workspace)
        self.assertEqual((module / "module.txt").read_text(), "precious module work\n")
        git(module, "stash", "push")
        self.silently(sync_workspace, self.repo_root, self.workspace)
        self.assertEqual(git(module, "rev-parse", "HEAD"), first)
        self.assertIn("module.txt", git(module, "stash", "show", "--name-only"))

    def test_failed_submodule_clone_cleans_new_directory_and_can_retry(self) -> None:
        first, _ = self.add_submodule()
        missing = self.root / "missing-submodule"
        git(self.seed, "config", "-f", ".gitmodules", "submodule.plugins/module.url", str(missing))
        git(self.seed, "commit", "-am", "broken submodule URL")
        git(self.seed, "push", "upstream", "main")
        good = self.workspace
        self.set_baseline(git(self.seed, "rev-parse", "HEAD"))
        with self.assertRaises(CommandError):
            self.clone()
        self.assertFalse(self.worktree.exists())
        self.workspace = good
        self.clone()
        self.assertEqual(git(self.worktree / "plugins/module", "rev-parse", "HEAD"), first)


class HorizonWorkspaceTests(WorkspaceContract, unittest.TestCase):
    template = HORIZON


class EditorWorkspaceTests(WorkspaceContract, unittest.TestCase):
    template = EDITOR



class WorkspaceLockTests(unittest.TestCase):
    def acquire_in_child(self, root: Path) -> subprocess.CompletedProcess[str]:
        child = (
            "import sys; from pathlib import Path; "
            "from build_tools.git_workspace import _workspace_lock; from build_tools.source_workspaces import EDITOR; "
            "lock = _workspace_lock(Path(sys.argv[1]), EDITOR, timeout=0.1); "
            "lock.__enter__(); lock.__exit__(None, None, None)"
        )
        return subprocess.run(
            [sys.executable, "-B", "-c", child, str(root)],
            cwd=Path(__file__).resolve().parents[2], capture_output=True, text=True, timeout=10,
        )

    def test_clean_and_rebuild_keep_an_active_lock_effective_across_processes(self) -> None:
        for command in ("clean", "rebuild"):
            with self.subTest(command=command), tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve()
                with _workspace_lock(root, EDITOR):
                    artifact = root / "build/conan/examples/relwithdebinfo/output.txt"
                    artifact.parent.mkdir(parents=True)
                    artifact.write_text("generated", encoding="utf-8")
                    with mock.patch.object(dev, "REPO_ROOT", root), \
                            mock.patch.object(dev, "install"), \
                            mock.patch.object(dev, "cmake_configure"), \
                            mock.patch.object(dev, "cmake_build"):
                        dev.execute(dev.create_parser().parse_args([command]))
                    self.assertFalse(artifact.exists())
                    self.assertTrue((root / "build" / ".locks" / "editor.lock").is_file())
                    result = self.acquire_in_child(root)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("Timed out waiting", result.stderr)

                result = self.acquire_in_child(root)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_lock_excludes_another_process_and_releases_after_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            try:
                with _workspace_lock(root, EDITOR):
                    result = self.acquire_in_child(root)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("Timed out waiting", result.stderr)
                    raise ValueError("test release on failure")
            except ValueError:
                pass
            with _workspace_lock(root, EDITOR, timeout=0.1):
                pass


if __name__ == "__main__":
    unittest.main()

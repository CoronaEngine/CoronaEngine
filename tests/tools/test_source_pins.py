"""The workspace pins are text in build_tools/source_workspaces.py.

``dev.py pin`` rewrites that text, so this guard round-trips the rewrite on a
copy: it must change exactly one workspace, keep the others, be idempotent and
reject anything that is not a full lowercase commit.
"""

from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from build_tools.git_workspace import head_on_remote
from build_tools.source_workspaces import WORKSPACES, find_workspace, rewrite_pin

SOURCE = ROOT / "build_tools/source_workspaces.py"


def test_every_pin_is_a_full_lowercase_commit():
    for workspace in WORKSPACES:
        assert len(workspace.commit) == 40, workspace
        assert workspace.commit == workspace.commit.lower(), workspace


def test_rewrite_pin_round_trips_a_copy(tmp_path):
    copy = tmp_path / "source_workspaces.py"
    copy.write_text(SOURCE.read_text(encoding="utf-8"), encoding="utf-8")
    first = WORKSPACES[0]
    replacement = "a" * 40

    assert rewrite_pin(first.key, replacement, copy) is True
    text = copy.read_text(encoding="utf-8")
    assert f'commit="{replacement}"' in text
    for other in WORKSPACES[1:]:
        assert f'commit="{other.commit}"' in text, f"{other.key} pin must not move"

    assert rewrite_pin(first.key, replacement, copy) is False, "rewriting the same pin changes nothing"
    assert rewrite_pin(first.key, first.commit, copy) is True, "the original pin comes back"
    assert f'commit="{first.commit}"' in copy.read_text(encoding="utf-8")


def test_rewrite_pin_refuses_anything_but_a_full_commit(tmp_path):
    copy = tmp_path / "source_workspaces.py"
    copy.write_text(SOURCE.read_text(encoding="utf-8"), encoding="utf-8")
    for bad in ("", "abc", "A" * 40, "0" * 41, "0" * 40 + " "):
        try:
            rewrite_pin("editor", bad, copy)
        except ValueError:
            continue
        raise AssertionError(f"{bad!r} must be rejected")
    try:
        rewrite_pin("nope", "a" * 40, copy)
    except ValueError:
        pass
    else:
        raise AssertionError("an unknown workspace key must be rejected")


def test_find_workspace_knows_every_key():
    for workspace in WORKSPACES:
        assert find_workspace(workspace.key) is workspace


def _git(*args, cwd):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, check=True)


@pytest.fixture()
def repo_with_remote(tmp_path):
    if shutil.which("git") is None:
        pytest.skip("git is not on PATH")
    bare = tmp_path / "bare.git"
    bare.mkdir()
    _git("init", "--bare", "-b", "main", cwd=bare)
    repo = tmp_path / "repo"
    repo.mkdir()
    _git("init", "-b", "main", cwd=repo)
    _git("config", "user.email", "guard@example.com", cwd=repo)
    _git("config", "user.name", "Guard", cwd=repo)
    (repo / "file.txt").write_text("one", encoding="utf-8")
    _git("add", "file.txt", cwd=repo)
    _git("commit", "-m", "one", cwd=repo)
    _git("remote", "add", "origin", str(bare), cwd=repo)
    _git("push", "-u", "origin", "main", cwd=repo)
    return repo


def test_head_on_remote_reports_unknown_without_a_remote(tmp_path):
    if shutil.which("git") is None:
        pytest.skip("git is not on PATH")
    repo = tmp_path / "lonely"
    repo.mkdir()
    _git("init", "-b", "main", cwd=repo)
    _git("config", "user.email", "guard@example.com", cwd=repo)
    _git("config", "user.name", "Guard", cwd=repo)
    (repo / "file.txt").write_text("one", encoding="utf-8")
    _git("add", "file.txt", cwd=repo)
    _git("commit", "-m", "one", cwd=repo)
    assert head_on_remote(repo) is None


def test_head_on_remote_tracks_what_was_pushed(repo_with_remote):
    assert head_on_remote(repo_with_remote) is True
    (repo_with_remote / "file.txt").write_text("two", encoding="utf-8")
    _git("add", "file.txt", cwd=repo_with_remote)
    _git("commit", "-m", "two", cwd=repo_with_remote)
    assert head_on_remote(repo_with_remote) is False, (
        "an unpushed commit must not be pinnable silently")

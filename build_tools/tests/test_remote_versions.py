"""Remote freshness is advisory and independent of local source synchronization."""

from pathlib import Path
import subprocess
from unittest import mock

import pytest

from build_tools import git_workspace
from build_tools.source_workspaces import HORIZON, EDITOR


@pytest.mark.parametrize("workspace", [HORIZON, EDITOR], ids=["horizon", "editor"])
def test_matching_remote_pin_has_no_warning(workspace):
    result = subprocess.CompletedProcess([], 0, f"{workspace.commit}\trefs/heads/{workspace.ref}\n", "")
    with mock.patch.object(git_workspace.subprocess, "run", return_value=result):
        assert git_workspace.remote_version_advice(Path.cwd(), workspace) == ()


@pytest.mark.parametrize("workspace", [HORIZON, EDITOR], ids=["horizon", "editor"])
def test_live_remote_difference_only_warns_using_configured_url(workspace):
    remote = "b" * 40
    result = subprocess.CompletedProcess([], 0, f"{remote}\trefs/heads/{workspace.ref}\n", "")
    pin = workspace.commit
    with mock.patch.object(git_workspace.subprocess, "run", return_value=result) as run:
        notes = git_workspace.remote_version_advice(Path.cwd(), workspace)
    assert len(notes) == 1
    assert workspace.name in notes[0]
    assert pin in notes[0] and remote in notes[0]
    assert "The build uses the Python pin" in notes[0]
    assert workspace.commit == pin
    assert run.call_count == 1
    command = run.call_args.args[0]
    assert "ls-remote" in command
    assert command[-2:] == (workspace.url, f"refs/heads/{workspace.ref}")
    assert run.call_args.kwargs["timeout"] == 10
    assert run.call_args.kwargs["env"]["GIT_TERMINAL_PROMPT"] == "0"


@pytest.mark.parametrize("workspace", [HORIZON, EDITOR], ids=["horizon", "editor"])
@pytest.mark.parametrize("result", [
    subprocess.CompletedProcess([], 128, "", "network unavailable"),
    subprocess.CompletedProcess([], 2, "", ""),
    subprocess.CompletedProcess([], 0, "malformed\trefs/heads/main\n", ""),
    subprocess.CompletedProcess([], 0, f"{'b' * 40}\trefs/heads/other\n", ""),
])
def test_remote_failure_or_missing_branch_is_advisory(workspace, result):
    with mock.patch.object(git_workspace.subprocess, "run", return_value=result):
        notes = git_workspace.remote_version_advice(Path.cwd(), workspace)
    assert len(notes) == 1
    assert "could not determine" in notes[0]
    assert workspace.commit in notes[0]


@pytest.mark.parametrize("workspace", [HORIZON, EDITOR], ids=["horizon", "editor"])
@pytest.mark.parametrize("error", [subprocess.TimeoutExpired("git", 10), OSError("offline")])
def test_remote_timeout_or_launch_failure_is_advisory(workspace, error):
    with mock.patch.object(git_workspace.subprocess, "run", side_effect=error):
        notes = git_workspace.remote_version_advice(Path.cwd(), workspace)
    assert len(notes) == 1
    assert "could not check" in notes[0]
    assert workspace.commit in notes[0]

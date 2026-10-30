"""Git provisioning must work without Git on PATH or an activated shell."""

from contextlib import contextmanager
import os
from pathlib import Path
import subprocess
from unittest import mock

import pytest

from build_tools import conda_environment, dev, git_tool


@pytest.fixture
def environment(tmp_path, monkeypatch):
    prefix = tmp_path / "Conda environment"
    (prefix / "conda-meta").mkdir(parents=True)
    monkeypatch.setattr(git_tool.sys, "prefix", str(prefix))
    monkeypatch.setattr(git_tool.shutil, "which", lambda name: None)
    monkeypatch.setattr(git_tool, "find_conda", lambda selected: "project-conda")
    monkeypatch.setenv("PATH", str(tmp_path / "no-git"))
    monkeypatch.setenv("CONDA_PREFIX", str(tmp_path / "unrelated-base"))
    return prefix


def make_git(prefix):
    executable = prefix / ("Library/bin/git.exe" if os.name == "nt" else "bin/git")
    executable.parent.mkdir(parents=True, exist_ok=True)
    executable.touch()
    return executable


def git_version(command, **kwargs):
    return subprocess.CompletedProcess(command, 0, "git version 2.50.0\n")


def test_reuses_working_git_from_path_without_conda(environment, monkeypatch):
    executable = make_git(environment.parent / "system-git")
    monkeypatch.setattr(git_tool.shutil, "which", lambda name: str(executable))
    monkeypatch.setattr(git_tool.sys, "prefix", str(environment.parent / "not-conda"))
    with mock.patch.object(git_tool.subprocess, "run", side_effect=git_version) as run:
        assert git_tool.ensure_git() == executable
    assert run.call_count == 1
    assert run.call_args.args[0] == [str(executable), "--version"]


def test_reuses_environment_git_and_exposes_it_to_subprocesses(environment):
    executable = make_git(environment)
    original_path = os.environ["PATH"]
    with mock.patch.object(git_tool.subprocess, "run", side_effect=git_version) as run:
        assert git_tool.ensure_git() == executable
    assert run.call_count == 1
    assert os.environ["PATH"] == str(executable.parent) + os.pathsep + original_path


@pytest.mark.parametrize("repair", [False, True])
def test_installs_into_python_environment_and_repairs_broken_package(environment, repair):
    # Other packages starting with git- do not mean Git itself is installed.
    (environment / "conda-meta/git-lfs-3.7.0-0.json").write_text("{}", encoding="utf-8")
    if repair:
        make_git(environment)
        (environment / "conda-meta/git-2.50.0-0.json").write_text("{}", encoding="utf-8")
    installed = False
    installs = []

    def run(command, **kwargs):
        nonlocal installed
        if command[0] == "project-conda":
            installs.append(command)
            make_git(environment)
            installed = True
            return subprocess.CompletedProcess(command, 0)
        if not installed:
            raise subprocess.CalledProcessError(1, command)
        return git_version(command, **kwargs)

    with mock.patch.object(git_tool.subprocess, "run", side_effect=run):
        executable = git_tool.ensure_git()
    assert installed
    assert len(installs) == 1
    command = installs[0]
    assert command[command.index("--prefix") + 1] == str(environment)
    assert command[command.index("--channel") + 1] == "conda-forge"
    assert "--freeze-installed" in command
    assert "git" in command
    assert ("--force-reinstall" in command) == repair
    assert os.environ["PATH"].split(os.pathsep)[0] == str(executable.parent)


def test_installation_failure_stops_the_workflow(environment):
    with mock.patch.object(git_tool.subprocess, "run", side_effect=subprocess.CalledProcessError(9, ["conda"])):
        with pytest.raises(RuntimeError, match="installation failed.*9"):
            git_tool.ensure_git()


def test_successful_conda_exit_still_requires_working_git(environment):
    with mock.patch.object(git_tool.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)):
        with pytest.raises(RuntimeError, match="Git is not usable"):
            git_tool.ensure_git()


def test_missing_git_under_system_python_does_not_install(environment, monkeypatch):
    monkeypatch.setattr(git_tool.sys, "prefix", str(environment.parent / "system-python"))
    with mock.patch.object(git_tool.subprocess, "run") as run:
        with pytest.raises(RuntimeError, match="not in a Conda environment"):
            git_tool.ensure_git()
    run.assert_not_called()


def test_rechecks_git_after_waiting_for_another_installer(environment, monkeypatch):
    @contextmanager
    def another_process_installed_git(prefix):
        make_git(prefix)
        yield

    monkeypatch.setattr(git_tool, "conda_environment_lock", another_process_installed_git)
    with mock.patch.object(git_tool.subprocess, "run", side_effect=git_version) as run:
        git_tool.ensure_git()
    assert run.call_count == 1
    assert run.call_args.args[0][-1] == "--version"


def test_locates_conda_for_absolute_python_without_activation(tmp_path, monkeypatch):
    prefix = tmp_path / "base/envs/project"
    executable = tmp_path / "base/Scripts/conda.exe"
    executable.parent.mkdir(parents=True)
    executable.touch()
    monkeypatch.delenv("CONDA_EXE", raising=False)
    monkeypatch.setattr(conda_environment.shutil, "which", lambda name: None)
    assert conda_environment.find_conda(prefix) == str(executable)


@pytest.mark.parametrize("command", [
    "status", "install", "_bootstrap", "configure", "build", "build-fast",
    "rebuild", "update", "horizon-sync", "editor-sync",
])
def test_every_git_command_prepares_git_before_any_work(command):
    with mock.patch.object(dev, "ensure_git", side_effect=RuntimeError("Git preparation stopped")) as prepare, \
            mock.patch.object(dev, "run_command") as run, \
            mock.patch.object(dev, "ensure_workspaces") as workspaces, \
            mock.patch.object(dev, "sync_workspace") as sync, \
            mock.patch.object(dev, "safe_remove") as remove:
        with pytest.raises(RuntimeError, match="Git preparation stopped"):
            dev.execute(dev.create_parser().parse_args([command]))
    prepare.assert_called_once_with()
    for operation in (run, workspaces, sync, remove):
        operation.assert_not_called()


def test_clean_does_not_need_git():
    with mock.patch.object(dev, "ensure_git") as prepare, mock.patch.object(dev, "clean_repo") as clean:
        dev.execute(dev.create_parser().parse_args(["clean"]))
    prepare.assert_not_called()
    clean.assert_called_once_with(dev.REPO_ROOT)

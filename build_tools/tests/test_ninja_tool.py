"""Ninja provisioning, command boundaries and recovery of stale CMake caches."""

from contextlib import contextmanager
import os
from pathlib import Path
import subprocess
from unittest import mock

import pytest

from build_tools import dev, ninja_tool, workflow


@pytest.fixture
def environment(tmp_path, monkeypatch):
    prefix = tmp_path / "Conda environment"
    (prefix / "conda-meta").mkdir(parents=True)
    monkeypatch.setattr(ninja_tool.sys, "prefix", str(prefix))
    monkeypatch.setattr(ninja_tool.shutil, "which", lambda name: None)
    monkeypatch.setattr(ninja_tool, "find_conda", lambda selected: "project-conda")
    monkeypatch.setenv("PATH", str(tmp_path / "no-ninja"))
    monkeypatch.setenv("CONDA_PREFIX", str(tmp_path / "unrelated-base"))
    return prefix


def make_ninja(prefix):
    executable = prefix / ("Library/bin/ninja.exe" if os.name == "nt" else "bin/ninja")
    executable.parent.mkdir(parents=True, exist_ok=True)
    executable.touch()
    return executable


def ninja_version(command, **kwargs):
    return subprocess.CompletedProcess(command, 0, "1.13.2\n")


@pytest.mark.parametrize("source", ["path", "cache", "environment"])
def test_reuses_existing_ninja_without_installing(environment, monkeypatch, source):
    executable = make_ninja(environment if source == "environment" else environment.parent / "external")
    if source == "path":
        monkeypatch.setattr(ninja_tool.shutil, "which", lambda name: str(executable))
    if source != "environment":
        monkeypatch.setattr(ninja_tool.sys, "prefix", str(environment.parent / "not-conda"))
    with mock.patch.object(ninja_tool.subprocess, "run", side_effect=ninja_version) as run:
        assert ninja_tool.ensure_ninja(str(executable) if source == "cache" else None) == executable
    assert run.call_count == 1
    assert run.call_args.args[0] == [str(executable), "--version"]
    if source != "path":
        assert os.environ["PATH"].split(os.pathsep)[0] == str(executable.parent)


@pytest.mark.parametrize("repair", [False, True])
def test_installs_and_repairs_in_python_environment(environment, repair):
    (environment / "conda-meta/ninja-extra-1.0-0.json").write_text("{}")
    if repair:
        make_ninja(environment)
        (environment / "conda-meta/ninja-1.13.2-0.json").write_text("{}")
    installed = False
    installs = []

    def run(command, **kwargs):
        nonlocal installed
        if command[0] == "project-conda":
            installs.append(command)
            make_ninja(environment)
            installed = True
            return subprocess.CompletedProcess(command, 0)
        if not installed:
            raise subprocess.CalledProcessError(1, command)
        return ninja_version(command, **kwargs)

    with mock.patch.object(ninja_tool.subprocess, "run", side_effect=run):
        executable = ninja_tool.ensure_ninja(str(environment / "missing-cached-ninja"))
    assert len(installs) == 1
    command = installs[0]
    assert command[command.index("--prefix") + 1] == str(environment)
    assert command[command.index("--channel") + 1] == "conda-forge"
    assert "--freeze-installed" in command and "ninja" in command
    assert ("--force-reinstall" in command) == repair
    assert os.environ["PATH"].split(os.pathsep)[0] == str(executable.parent)


@pytest.mark.parametrize("outcome", ["1.13.2\n", "1.13.2.git\n", "", "cmake version 4.0.3\n", "1.2\nnoise"])
def test_version_probe_rejects_non_ninja_output(outcome):
    with mock.patch.object(ninja_tool.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, outcome)):
        assert ninja_tool.probe_ninja("ninja") == outcome.startswith("1.13.2")


@pytest.mark.parametrize("error", [OSError("broken"), subprocess.TimeoutExpired("ninja", 30)])
def test_unrunnable_ninja_is_rejected(error):
    with mock.patch.object(ninja_tool.subprocess, "run", side_effect=error):
        assert not ninja_tool.probe_ninja("ninja")


def test_installation_failure_stops_work(environment):
    with mock.patch.object(ninja_tool.subprocess, "run", side_effect=subprocess.CalledProcessError(9, ["conda"])):
        with pytest.raises(RuntimeError, match="installation failed.*9"):
            ninja_tool.ensure_ninja()


def test_successful_installer_exit_still_requires_working_ninja(environment):
    with mock.patch.object(ninja_tool.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)):
        with pytest.raises(RuntimeError, match="Ninja is not usable"):
            ninja_tool.ensure_ninja()


def test_missing_ninja_outside_conda_does_not_install(environment, monkeypatch):
    monkeypatch.setattr(ninja_tool.sys, "prefix", str(environment.parent / "system-python"))
    with mock.patch.object(ninja_tool.subprocess, "run") as run:
        with pytest.raises(RuntimeError, match="not in a Conda environment"):
            ninja_tool.ensure_ninja()
    run.assert_not_called()


def test_rechecks_after_waiting_for_another_installer(environment, monkeypatch):
    @contextmanager
    def another_installer(prefix):
        make_ninja(prefix)
        yield

    monkeypatch.setattr(ninja_tool, "conda_environment_lock", another_installer)
    with mock.patch.object(ninja_tool.subprocess, "run", side_effect=ninja_version) as run:
        ninja_tool.ensure_ninja()
    assert run.call_count == 1
    assert run.call_args.args[0][-1] == "--version"


@pytest.mark.parametrize("command", ["install", "_bootstrap", "configure", "build", "build-fast", "rebuild", "update"])
def test_build_commands_prepare_ninja_before_work(command):
    with mock.patch.object(dev, "ensure_git"), \
            mock.patch.object(dev, "ensure_ninja", side_effect=RuntimeError("Ninja preparation stopped")) as prepare, \
            mock.patch.object(dev, "install") as install, \
            mock.patch.object(dev, "cmake_build") as build, \
            mock.patch.object(dev, "ensure_workspaces") as workspaces, \
            mock.patch.object(dev, "safe_remove") as remove:
        with pytest.raises(RuntimeError, match="Ninja preparation stopped"):
            dev.execute(dev.create_parser().parse_args([command]))
    prepare.assert_called_once()
    for operation in (install, build, workspaces, remove):
        operation.assert_not_called()


@pytest.mark.parametrize("command", ["status", "horizon-sync", "editor-sync", "clean"])
def test_other_commands_do_not_prepare_ninja(command):
    with mock.patch.object(dev, "ensure_git"), mock.patch.object(dev, "ensure_ninja") as prepare, \
            mock.patch.object(dev, "run_command"), mock.patch.object(dev, "inspect_workspace"), \
            mock.patch.object(dev, "sync_workspace"), mock.patch.object(dev, "clean_repo"):
        dev.execute(dev.create_parser().parse_args([command]))
    prepare.assert_not_called()


@pytest.mark.parametrize("working", [False, True])
def test_build_repairs_stale_cache_before_starting_ninja(tmp_path, working):
    directory = workflow.build_dir(tmp_path, "RelWithDebInfo")
    directory.mkdir(parents=True)
    (directory / "CMakeCache.txt").write_text(
        "CMAKE_GENERATOR:INTERNAL=Ninja Multi-Config\nCMAKE_MAKE_PROGRAM:FILEPATH=cached-ninja\n"
    )
    calls = mock.Mock()
    with mock.patch.object(workflow, "probe_ninja", return_value=working), \
            mock.patch.object(workflow, "write_cmake_build_environment", calls.save), \
            mock.patch.object(workflow, "cmake_configure", calls.configure), \
            mock.patch.object(workflow, "load_conan_build_environment", return_value={}), \
            mock.patch.object(workflow, "run_command", calls.build):
        workflow.cmake_build(tmp_path, "RelWithDebInfo", "test-target")
    assert [call[0] for call in calls.mock_calls] == (["build"] if working else ["save", "configure", "build"])
    assert workflow.cached_ninja(tmp_path, "RelWithDebInfo") == "cached-ninja"


def test_other_generators_are_not_treated_as_ninja(tmp_path):
    directory = workflow.build_dir(tmp_path, "Debug")
    directory.mkdir(parents=True)
    (directory / "CMakeCache.txt").write_text("CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026\n")
    assert workflow.cached_ninja(tmp_path, "Debug") is None
    with mock.patch.object(workflow, "cmake_configure") as configure:
        workflow.repair_ninja_cache(tmp_path, "Debug", "examples")
    configure.assert_not_called()

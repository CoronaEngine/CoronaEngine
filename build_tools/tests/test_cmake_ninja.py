"""Exercise Ninja selection before project(), including a stale CMake cache."""

import os
from pathlib import Path
import shutil
import subprocess

import pytest

from build_tools.paths import REPO_ROOT
from build_tools import workflow
from build_tools.ninja_tool import probe_ninja


@pytest.mark.parametrize("cached", ["absent", "working", "missing", "wrong-program"])
def test_configure_and_build_with_ninja_outside_initial_path(tmp_path, cached):
    cmake = shutil.which("cmake")
    ninja = shutil.which("ninja") or workflow.cached_ninja(REPO_ROOT, "RelWithDebInfo")
    if not cmake or not ninja or not probe_ninja(ninja):
        pytest.skip("requires CMake and an existing Ninja (no installation during tests)")

    # A distinct fallback proves a working cached selection is preserved.
    tools_dir = tmp_path / "tools with spaces"
    tools_dir.mkdir()
    fallback = tools_dir / Path(ninja).name
    shutil.copy2(ninja, fallback)
    source = tmp_path / "source"
    source.mkdir()
    module = (REPO_ROOT / "build_tools/cmake/corona_ninja.cmake").as_posix()
    (source / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.29)\n'
        f'set(ENV{{PATH}} "{tools_dir.as_posix()}{os.pathsep}$ENV{{PATH}}")\n'
        f'include("{module}")\n'
        'corona_configure_ninja()\n'
        'project(NinjaBootstrapTest LANGUAGES NONE)\n'
        'add_custom_target(smoke ALL COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/built")\n',
        encoding="utf-8",
    )
    build = tmp_path / "build"
    # The parent process has neither the chosen Ninja nor its fallback on PATH.
    env = dict(os.environ)
    env["PATH"] = str(tmp_path)
    assert shutil.which("ninja", path=env["PATH"]) is None
    args = [cmake, "-S", str(source), "-B", str(build), "-G", "Ninja Multi-Config"]
    value = {"working": ninja, "missing": str(tmp_path / "removed-ninja"), "wrong-program": cmake}.get(cached)
    if value:
        args.append(f"-DCMAKE_MAKE_PROGRAM:FILEPATH={value}")
    result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    selected = workflow._cache_value(build / "CMakeCache.txt", "CMAKE_MAKE_PROGRAM")
    assert Path(selected).resolve() == Path(ninja if cached == "working" else fallback).resolve()
    result = subprocess.run([cmake, "--build", str(build), "--config", "Release"],
                            env=env, capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    assert (build / "built").is_file()

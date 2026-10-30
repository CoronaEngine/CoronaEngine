"""Exercise relocated entry points and their Conan command boundaries."""

from pathlib import Path
import subprocess
import sys
from unittest import mock

import pytest

from build_tools import conan_cache, workflow
from build_tools.paths import BUILD_TOOLS_ROOT, LOCAL_RECIPES, REPO_ROOT


@pytest.mark.parametrize("script", ["dev.py", "conan_cache.py", "scripts/editor_copy_and_build.py"])
def test_script_can_start_outside_repository(script, tmp_path):
    result = subprocess.run(
        [sys.executable, str(BUILD_TOOLS_ROOT / script), "--help"],
        cwd=tmp_path, capture_output=True, text=True, timeout=30,
    )
    assert result.returncode == 0, result.stderr
    assert "usage:" in result.stdout


def test_install_uses_relocated_recipe_and_profiles():
    with mock.patch.object(workflow, "export_local_recipes") as export, \
            mock.patch.object(workflow, "run_command") as run, \
            mock.patch.object(workflow, "write_cmake_build_environment") as save:
        workflow.conan_install(
            REPO_ROOT, "RelWithDebInfo", target_family="tests",
            options=["&:with_tests=True"], recipes=LOCAL_RECIPES,
            recipe_toggle_env="CORONA_CONAN_EXPORT_LOCAL_RECIPES",
        )
    command = run.call_args.args[0]
    assert Path(command[2]) == BUILD_TOOLS_ROOT / "conanfile.py"
    assert Path(command[2]).is_file()
    for flag in ("-pr:a", "-pr:b"):
        profile = Path(command[command.index(flag) + 1])
        assert profile == BUILD_TOOLS_ROOT / "conan/profiles/windows-msvc-relwithdebinfo"
        assert profile.is_file()
    assert run.call_args.kwargs["cwd"] == REPO_ROOT
    assert "user.corona:target_family=tests" in command
    assert "&:with_tests=True" in command
    assert all((REPO_ROOT / recipe / "conanfile.py").is_file() for recipe in export.call_args.args[1])
    save.assert_called_once_with(REPO_ROOT, "RelWithDebInfo", "tests")


@pytest.mark.parametrize("package", [None, "fmt/12.1.0"])
def test_cache_update_selects_project_recipe_or_package(package):
    argv = ["update"] + ([package] if package else [])
    args = conan_cache.create_parser().parse_args(argv)
    with mock.patch.object(conan_cache, "export_local_recipes"), \
            mock.patch.object(conan_cache, "ensure_ninja") as prepare, \
            mock.patch.object(conan_cache, "run_command") as run:
        conan_cache.update_cache(args)
    prepare.assert_called_once_with()
    command = run.call_args.args[0]
    if package:
        assert command[2:4] == ["--requires", package]
    else:
        assert Path(command[2]) == BUILD_TOOLS_ROOT / "conanfile.py"
    assert run.call_args.kwargs["cwd"] == REPO_ROOT

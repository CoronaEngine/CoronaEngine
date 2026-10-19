from pathlib import Path


def test_tools_tests_have_a_dedicated_owner_directory():
    repo_root = Path(__file__).resolve().parents[1]
    tools_root = repo_root.parent / "tools"
    tests_root = tools_root / "tests"

    assert tests_root.is_dir()
    assert (tests_root / "test_pack.py").is_file()
    assert (tests_root / "test_workflow.py").is_file()
    assert not list(tools_root.glob("test_*.py"))


def test_cmake_python_helpers_have_a_tools_build_owner():
    repo_root = Path(__file__).resolve().parents[1].parent
    tools_build = repo_root / "tools" / "build"
    misc_pytools = repo_root / "misc" / "pytools"

    for name in ("check_pip_modules.py", "copy_files.py", "editor_copy_and_build.py"):
        assert (tools_build / name).is_file()
        assert not (misc_pytools / name).exists()

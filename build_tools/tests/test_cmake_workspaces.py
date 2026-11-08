"""Real Git + Ninja regeneration, without Conan installs or version stamp files."""

from pathlib import Path
import shutil
import subprocess
import sys

import pytest

from build_tools.paths import REPO_ROOT
from build_tools.workflow import cached_ninja, safe_remove
from build_tools.tests.test_git_workspace import git


@pytest.fixture
def workspace_build(tmp_path):
    cmake = shutil.which("cmake")
    ninja = shutil.which("ninja") or cached_ninja(REPO_ROOT, "RelWithDebInfo")
    if not cmake or not ninja:
        pytest.skip("requires existing CMake and Ninja; no tool installation")

    seed = tmp_path / "seed"
    seed.mkdir()
    git(seed, "init", "-b", "main")
    (seed / "value.txt").write_text("old\n", encoding="utf-8")
    git(seed, "add", ".")
    git(seed, "commit", "-m", "old")
    old = git(seed, "rev-parse", "HEAD")
    (seed / "value.txt").write_text("new\n", encoding="utf-8")
    git(seed, "commit", "-am", "new")
    new = git(seed, "rev-parse", "HEAD")
    origin = tmp_path / "origin.git"
    git(tmp_path, "clone", "--bare", str(seed), str(origin))

    source = tmp_path / "engine with spaces"
    source.mkdir()
    build = tmp_path / "build with spaces"
    pins = source / "source_workspaces.py"

    def pin(horizon, editor):
        pins.write_text(
            "from dataclasses import replace\n"
            "from build_tools.source_workspaces import HORIZON, EDITOR\n"
            "WORKSPACES = (\n"
            f"    replace(HORIZON, url={origin.as_posix()!r}, ref='main', commit={horizon!r}),\n"
            f"    replace(EDITOR, url={origin.as_posix()!r}, commit={editor!r}),\n"
            ")\n",
            encoding="utf-8",
        )

    pin(old, old)
    (source / "prepare.py").write_text(
        "from pathlib import Path\nimport runpy\nimport sys\n"
        f"sys.path.insert(0, {str(REPO_ROOT)!r})\n"
        "from build_tools import dev\n"
        "root = Path(__file__).parent\n"
        "dev.REPO_ROOT = root\n"
        "dev.WORKSPACES = runpy.run_path(str(root / 'source_workspaces.py'))['WORKSPACES']\n"
        "dev.ensure_workspaces()\n",
        encoding="utf-8",
    )
    module = (REPO_ROOT / "build_tools/cmake/corona_dev_bootstrap.cmake").as_posix()
    (source / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 4.0)\n"
        f'include("{module}")\n'
        "set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS\n"
        '    "${CMAKE_SOURCE_DIR}/source_workspaces.py")\n'
        # Use the real source preparation and watcher, excluding unrelated
        # Conda/Conan/compiler provisioning from this offline fixture.
        f'execute_process(COMMAND "{Path(sys.executable).as_posix()}" -B\n'
        '    "${CMAKE_SOURCE_DIR}/prepare.py" COMMAND_ERROR_IS_FATAL ANY)\n'
        "foreach(name IN ITEMS Horizon CoronaEditor)\n"
        '    corona_watch_source_workspace("${CMAKE_SOURCE_DIR}/${name}")\n'
        '    execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}/${name}" rev-parse HEAD\n'
        "        OUTPUT_VARIABLE ${name}_head OUTPUT_STRIP_TRAILING_WHITESPACE\n"
        "        COMMAND_ERROR_IS_FATAL ANY)\n"
        "endforeach()\n"
        "project(WorkspaceRegeneration LANGUAGES NONE)\n"
        'set(heads "${Horizon_head} ${CoronaEditor_head}")\n'
        'file(APPEND "${CMAKE_BINARY_DIR}/configures.log" "${heads}\\n")\n'
        'file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/configured.txt" CONTENT "${heads}")\n'
        'add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/built.txt"\n'
        '    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_BINARY_DIR}/configured.txt"\n'
        '            "${CMAKE_BINARY_DIR}/built.txt"\n'
        '    DEPENDS "${CMAKE_BINARY_DIR}/configured.txt" VERBATIM)\n'
        'add_custom_target(consumer ALL DEPENDS "${CMAKE_BINARY_DIR}/built.txt")\n',
        encoding="utf-8",
    )

    def run(*args, success=True):
        result = subprocess.run(
            [cmake, *map(str, args)], capture_output=True, text=True, timeout=60,
        )
        output = result.stdout + result.stderr
        assert (result.returncode == 0) == success, output
        return output

    def compile(*, named=True, success=True):
        args = ["--build", build, "--config", "Release"]
        if named:
            args += ["--target", "consumer"]
        return run(*args, success=success)

    def assert_noop():
        product = build / "built.txt"
        before = product.stat().st_mtime_ns
        configurations = (build / "configures.log").read_text()
        compile(named=False)
        assert product.stat().st_mtime_ns == before
        assert (build / "configures.log").read_text() == configurations

    output = run("-S", source, "-B", build, "-G", "Ninja Multi-Config", f"-DCMAKE_MAKE_PROGRAM={ninja}")
    for name in ("Horizon", "CoronaEditor"):
        assert f"{name} Python pin {old} differs from the latest remote 'main' {new}" in output
        assert git(source / name, "status", "--porcelain") == ""
    compile()
    assert (build / "built.txt").read_text() == f"{old} {old}"
    return source, build, old, new, origin, pin, compile, assert_noop


def test_build_tracks_pins_refs_and_deleted_checkouts(workspace_build):
    source, build, old, new, _, pin, compile, assert_noop = workspace_build
    horizon, editor = source / "Horizon", source / "CoronaEditor"
    assert_noop()

    pin(new, new)
    assert "Re-running CMake" in compile(named=False)
    assert (build / "built.txt").read_text() == f"{new} {new}"
    assert_noop()

    # Detached HEAD and a different symbolic branch must both be followed.
    git(horizon, "checkout", "--detach", old)
    compile()
    assert git(horizon, "rev-parse", "HEAD") == new
    git(editor, "checkout", "-b", "developer")
    compile()
    git(editor, "reset", "--hard", old)
    compile()
    assert git(editor, "rev-parse", "HEAD") == new

    # Packing removes loose refs; a later reset recreates one without changing HEAD.
    git(editor, "pack-refs", "--all", "--prune")
    compile()
    git(editor, "reset", "--hard", old)
    compile()
    assert git(editor, "rev-parse", "HEAD") == new
    assert_noop()

    safe_remove(source, horizon)
    compile()
    assert git(horizon, "rev-parse", "HEAD") == new
    assert_noop()

    # Failure must stop regeneration before any product can be built.
    product_time = (build / "built.txt").stat().st_mtime_ns
    pin("b" * 40, new)
    assert "not reachable" in compile(success=False)
    assert (build / "built.txt").stat().st_mtime_ns == product_time
    assert git(horizon, "rev-parse", "HEAD") == new

    (horizon / "notes.txt").write_text("local notes", encoding="utf-8")
    pin(old, new)
    # Nonconflicting untracked files survive a switch to the Python pin.
    (build / "built.txt").unlink()
    output = compile()
    assert "[WARN]" in output
    assert f"latest remote 'main' {new}" in output
    assert (horizon / "notes.txt").read_text() == "local notes"
    assert git(horizon, "rev-parse", "HEAD") == old
    assert (build / "built.txt").read_text() == f"{old} {new}"
    assert_noop()

    # A genuine conflict stops before compiling against the wrong version.
    product_time = (build / "built.txt").stat().st_mtime_ns
    (horizon / "value.txt").write_text("local edit\n", encoding="utf-8")
    pin(new, new)
    assert "Git refused the checkout" in compile(success=False)
    assert git(horizon, "rev-parse", "HEAD") == old
    assert (horizon / "value.txt").read_text() == "local edit\n"
    assert (build / "built.txt").stat().st_mtime_ns == product_time
    # Restore Git's working-tree line endings, including on Windows.
    git(horizon, "checkout", "--", "value.txt")
    compile()
    assert (build / "built.txt").read_text() == f"{new} {new}"
    assert_noop()


def test_build_tracks_linked_worktree_git_paths(workspace_build, tmp_path):
    source, _, old, new, origin, _, compile, assert_noop = workspace_build
    horizon = source / "Horizon"
    safe_remove(source, horizon)
    owner = tmp_path / "worktree owner"
    git(tmp_path, "clone", str(origin), str(owner))
    git(owner, "worktree", "add", "-b", "linked", str(horizon), old)
    assert (horizon / ".git").is_file()
    compile()

    git(owner, "pack-refs", "--all", "--prune")
    compile()
    git(horizon, "reset", "--hard", new)
    compile()
    assert git(horizon, "rev-parse", "HEAD") == old
    # main is in use in the owner; synchronization must not reset it.
    assert git(owner, "rev-parse", "HEAD") == new
    assert_noop()

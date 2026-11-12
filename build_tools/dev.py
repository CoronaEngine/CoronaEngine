from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

# Support both `python build_tools/dev.py` and `python -m build_tools.dev`.
if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from build_tools.git_workspace import (ensure_workspace, head_on_remote, inspect_workspace,
                                      remote_version_advice, sync_workspace)
from build_tools.git_tool import ensure_git
from build_tools.ninja_tool import ensure_ninja
from build_tools.paths import LOCAL_RECIPES, REPO_ROOT
from build_tools.source_workspaces import WORKSPACES, find_workspace, rewrite_pin
from build_tools.workflow import (
    CONFIGURATIONS,
    DEFAULT_CONFIGURATION,
    DEFAULT_TARGET_FAMILY,
    TARGET_FAMILIES,
    CommandError,
    build_dir,
    cached_ninja,
    clean_repo,
    cmake_build,
    cmake_configure,
    conan_install,
    run_command,
    safe_remove,
)


DEFAULT_TARGET = "corona_engine"
WORKSPACE_COMMANDS = {f"{workspace.key}-sync": workspace for workspace in WORKSPACES}


def pin_workspaces(repo_root: Path, keys: list[str], commit: str | None = None) -> None:
    """Point the pinned commits at a commit.

    ``commit`` is used when the working checkout is not the one the build reads:
    the build checkout is kept at the pin, so its HEAD cannot advance the pin.
    Without it, the checkout's HEAD is the source, and a commit that is not on
    the remote branch is reported before the file changes.
    """
    if commit is not None and len(keys) != 1:
        raise RuntimeError("Name one workspace when pinning an explicit commit")
    for key in keys:
        workspace = find_workspace(key)
        directory = repo_root / workspace.directory
        status = inspect_workspace(repo_root, workspace)
        if not status.cloned and commit is None:
            raise RuntimeError(f"{workspace.name} is not cloned at {directory}")
        if commit is not None:
            head = commit.lower()
            if not status.cloned:
                print(f"[WARN] {workspace.name} is not cloned, so the commit cannot be verified")
        else:
            head = status.head.lower()
            if not head:
                raise RuntimeError(f"{workspace.name} HEAD could not be read")
            if status.dirty:
                print(f"[WARN] {workspace.name} has uncommitted changes; the pin will point at HEAD anyway")
            if status.detached:
                print(f"[WARN] {workspace.name} is on a detached HEAD, so the commit belongs to no branch")
            if head_on_remote(directory) is False:
                print(f"[WARN] {workspace.name} HEAD is on no remote branch; push it before pinning")
            if status.branch and status.branch != workspace.ref:
                print(f"[WARN] {workspace.name} is on {status.branch}, not the pinned {workspace.ref}")
        if head == workspace.commit:
            print(f"{workspace.name}: already pinned at {head}")
            continue
        rewrite_pin(key, head)
        print(f"{workspace.name}: {workspace.commit[:12]} -> {head[:12]}")
        print(f"  next: python build_tools/dev.py {workspace.key}-sync")


def materialize_editor_frontend_assets(repo_root: Path) -> None:
    """Copy engine-owned raw templates into the editor checkout's Frontend tree.

    `fileProgram.js` imports the block program template with Vite's `?raw`
    loader, so frontend development and packaging both need the file even
    though its source moved to the engine repository.
    """
    source = repo_root / "embedded_python" / "frontend_assets" / "block_program.py"
    target = (repo_root / "CoronaEditor" / "editor" / "Frontend" / "src"
              / "blockly" / "generators" / "runtime" / "block_program.py")
    if source.is_file():
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
RECIPE_TOGGLE_ENV = "CORONA_CONAN_EXPORT_LOCAL_RECIPES"
TARGET_FAMILY_OPTIONS: dict[str, tuple[str, ...]] = {
    "core": (
        "&:with_examples=False", "&:with_tests=False",
        "&:with_vision_tests=False",
    ),
    "examples": (
        "&:with_examples=True", "&:with_tests=False",
        "&:with_vision_tests=False",
    ),
    "tests": (
        "&:with_examples=False", "&:with_tests=True",
        "&:with_vision_tests=False",
    ),
    "vision": (
        "&:with_examples=False", "&:with_tests=False",
        "&:with_vision_tests=False",
    ),
    "vision-tests": (
        "&:with_examples=False", "&:with_tests=True",
        "&:with_vision_tests=True",
    ),
}
VISION_TEST_TARGETS = {
    "corona_vision_render_mode_config_tests",
    "corona_lightfield_geometry_tests",
    "corona_vision_material_adapter_tests",
    "corona_vision_pipeline_key_tests",
    "corona_vision_scene_resource_tests",
    "corona_vision_interop_lifetime_tests",
    "corona_vision_device_lifetime_tests",
    "corona_vision_geometry_gpu_resource_tests",
}


def target_family_for_target(target: str) -> str:
    lowered = target.lower()
    if lowered in {"all", "corona_engine"}:
        return "examples"
    if target.startswith("test-") or target.startswith("vision-test") or target in VISION_TEST_TARGETS:
        return "vision-tests"
    if target.startswith("vision-"):
        return "vision"
    if lowered.endswith("_tests") or target == "corona_ui_multisurface_smoke":
        return "tests"
    return "core"


def target_family_for_targets(targets: list[str]) -> str:
    if not targets:
        return DEFAULT_TARGET_FAMILY
    families = {target_family_for_target(target) for target in targets}
    if len(families) != 1:
        choices = ", ".join(sorted(families))
        raise ValueError(
            f"Targets require different dependency families ({choices}). "
            "Configure and build one target family at a time."
        )
    return families.pop()


def conan_options(target_family: str) -> list[str]:
    try:
        return list(TARGET_FAMILY_OPTIONS[target_family])
    except KeyError as error:
        raise ValueError(f"Unsupported target family: {target_family}") from error


def ensure_workspaces() -> None:
    for workspace in WORKSPACES:
        ensure_workspace(REPO_ROOT, workspace)


def install(configuration: str, target_family: str, *, update: bool = False) -> None:
    ensure_workspaces()
    conan_install(
        REPO_ROOT,
        configuration,
        target_family=target_family,
        options=conan_options(target_family),
        recipes=LOCAL_RECIPES,
        recipe_toggle_env=RECIPE_TOGGLE_ENV,
        update=update,
    )


def execute(args: argparse.Namespace) -> None:
    targets = args.targets or [DEFAULT_TARGET]
    configuration = args.configuration
    target_family = args.target_family or target_family_for_targets(targets)
    if args.command != "clean":
        ensure_git()
    if args.command in {"install", "_bootstrap", "configure", "build", "build-fast", "rebuild", "update"}:
        ensure_ninja(cached_ninja(REPO_ROOT, configuration, target_family))
    if args.command == "status":
        run_command(("git", "status", "--short", "--branch"), cwd=REPO_ROOT)
        for workspace in WORKSPACES:
            try:
                status = inspect_workspace(REPO_ROOT, workspace)
            except RuntimeError as error:
                print(f"[WARN] {workspace.name} workspace could not be inspected: {error}")
            else:
                print(status.summary())
                for note in status.advice():
                    print(f"[WARN] {note}")
                for note in remote_version_advice(REPO_ROOT, workspace):
                    print(f"[WARN] {note}")
        run_command(("conan", "--version"), cwd=REPO_ROOT)
        run_command(("cmake", "--list-presets"), cwd=REPO_ROOT)
    elif args.command in {"install", "_bootstrap"}:
        install(configuration, target_family)
    elif args.command == "configure":
        install(configuration, target_family)
        cmake_configure(REPO_ROOT, configuration, target_family)
    elif args.command == "build":
        install(configuration, target_family)
        cmake_configure(REPO_ROOT, configuration, target_family)
        cmake_build(REPO_ROOT, configuration, targets, target_family)
    elif args.command == "build-fast":
        ensure_workspaces()
        cmake_build(REPO_ROOT, configuration, targets, target_family)
    elif args.command == "rebuild":
        safe_remove(REPO_ROOT, build_dir(REPO_ROOT, configuration, target_family))
        install(configuration, target_family)
        cmake_configure(REPO_ROOT, configuration, target_family)
        cmake_build(REPO_ROOT, configuration, targets, target_family)
    elif args.command == "update":
        install(configuration, target_family, update=True)
        cmake_configure(REPO_ROOT, configuration, target_family)
    elif args.command in WORKSPACE_COMMANDS:
        workspace = WORKSPACE_COMMANDS[args.command]
        sync_workspace(REPO_ROOT, workspace)
        if workspace.key == "editor":
            materialize_editor_frontend_assets(REPO_ROOT)
    elif args.command == "pin":
        targets = list(args.targets)
        # `pin editor <commit>` pins an explicit commit; `pin` alone uses HEAD.
        explicit = None
        if len(targets) > 1:
            explicit = targets.pop()
        pin_workspaces(REPO_ROOT, targets or [workspace.key for workspace in WORKSPACES], explicit)
    elif args.command == "clean":
        clean_repo(REPO_ROOT)
    else:
        raise RuntimeError(f"Unsupported command: {args.command}")


def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="CoronaEngine developer workflow",
        epilog=(
            "Horizon/ and CoronaEditor/ are ordinary Git clones at the engine repository root. "
            "Builds align local HEAD to build_tools/source_workspaces.py, preserving "
            "nonconflicting edits. Differences from the latest remote branch only warn. "
            "horizon-sync and editor-sync also synchronize submodules explicitly. "
            "update additionally refreshes Conan dependencies. "
            "pin writes the checkouts' current HEAD into source_workspaces.py "
            "(optionally for one workspace: pin editor), or an explicit commit "
            "(pin editor <sha>) when the build checkout is pin-aligned."
        ),
    )
    parser.add_argument(
        "command", nargs="?", default="status",
        choices=(
            "status", "install", "configure", "build", "build-fast", "rebuild", "update",
            *WORKSPACE_COMMANDS,
            "pin", "clean", "_bootstrap",
        ),
    )
    parser.add_argument("targets", nargs="*")
    parser.add_argument("--configuration", choices=CONFIGURATIONS, default=DEFAULT_CONFIGURATION)
    parser.add_argument(
        "--target-family",
        choices=TARGET_FAMILIES,
        help="Configure a named target family instead of inferring it from the build target.",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    try:
        execute(create_parser().parse_args(argv))
        return 0
    except CommandError as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        return error.returncode
    except (OSError, RuntimeError, ValueError) as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Support both `python build_tools/dev.py` and `python -m build_tools.dev`.
if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from build_tools.git_workspace import ensure_workspace, inspect_workspace, sync_workspace
from build_tools.git_tool import ensure_git
from build_tools.ninja_tool import ensure_ninja
from build_tools.paths import LOCAL_RECIPES, REPO_ROOT
from build_tools.source_workspaces import WORKSPACES
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
        sync_workspace(REPO_ROOT, WORKSPACE_COMMANDS[args.command])
    elif args.command == "clean":
        clean_repo(REPO_ROOT)
    else:
        raise RuntimeError(f"Unsupported command: {args.command}")


def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="CoronaEngine developer workflow",
        epilog=(
            "Horizon/ and CoronaEditor/ are ordinary Git clones at the engine repository root. "
            "Builds use existing sources as-is, including local edits. Baselines are defined "
            "in build_tools/source_workspaces.py; use horizon-sync or editor-sync to adopt them "
            "explicitly (including submodules). update refreshes Conan dependencies only."
        ),
    )
    parser.add_argument(
        "command", nargs="?", default="status",
        choices=(
            "status", "install", "configure", "build", "build-fast", "rebuild", "update",
            *WORKSPACE_COMMANDS,
            "clean", "_bootstrap",
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

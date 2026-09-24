from __future__ import annotations

import argparse
import sys
from pathlib import Path

from horizon_workspace import ensure_workspace, inspect_workspace, sync_workspace, update_workspace
from workflow import (
    CONFIGURATIONS,
    DEFAULT_CONFIGURATION,
    DEFAULT_TARGET_FAMILY,
    TARGET_FAMILIES,
    CommandError,
    build_dir,
    clean_repo,
    cmake_build,
    cmake_configure,
    conan_install,
    run_command,
    safe_remove,
)


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TARGET = "corona_engine"
LOCAL_RECIPES = (
    "conan/recipes/ktm",
    "conan/recipes/pfr",
    "conan/recipes/slang",
    "conan/recipes/vulkan-memory-allocator",
    "conan/recipes/astc-encoder",
    "conan/recipes/cef-binary",
    "conan/recipes/ffmpeg",
)
RECIPE_TOGGLE_ENV = "CORONA_CONAN_EXPORT_LOCAL_RECIPES"
TARGET_FAMILY_OPTIONS: dict[str, tuple[str, ...]] = {
    "core": (
        "&:with_examples=False", "&:with_tests=False", "&:with_vision=False",
        "&:with_vision_tests=False", "&:with_oidn=False",
    ),
    "examples": (
        "&:with_examples=True", "&:with_tests=False", "&:with_vision=True",
        "&:with_vision_tests=False", "&:with_oidn=False",
    ),
    "tests": (
        "&:with_examples=False", "&:with_tests=True", "&:with_vision=False",
        "&:with_vision_tests=False", "&:with_oidn=False",
    ),
    "vision": (
        "&:with_examples=False", "&:with_tests=False", "&:with_vision=True",
        "&:with_vision_tests=False", "&:with_oidn=False",
    ),
    "vision-tests": (
        "&:with_examples=False", "&:with_tests=True", "&:with_vision=True",
        "&:with_vision_tests=True", "&:with_oidn=False",
    ),
    "vision-oidn": (
        "&:with_examples=False", "&:with_tests=False", "&:with_vision=True",
        "&:with_vision_tests=False", "&:with_oidn=True",
    ),
}
VISION_TEST_TARGETS = {
    "corona_vision_model_normalization_tests",
    "corona_vision_render_mode_config_tests",
    "corona_lightfield_geometry_tests",
    "corona_vision_material_adapter_tests",
    "corona_vision_pipeline_key_tests",
    "corona_vision_scene_resource_tests",
    "corona_vision_interop_lifetime_tests",
    "corona_vision_geometry_gpu_resource_tests",
}


def target_family_for_target(target: str) -> str:
    lowered = target.lower()
    if lowered in {"all", "corona_engine"}:
        return "examples"
    if lowered == "cp_oidn_cuda" or "oidn" in lowered:
        return "vision-oidn"
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


def install(configuration: str, target_family: str, *, update: bool = False) -> None:
    ensure_workspace(REPO_ROOT)
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
    target = targets[0]
    configuration = args.configuration
    target_family = args.target_family or target_family_for_targets(targets)
    if args.command == "status":
        run_command(("git", "status", "--short", "--branch"), cwd=REPO_ROOT)
        try:
            print(inspect_workspace(REPO_ROOT).summary())
        except RuntimeError as error:
            print(f"[WARN] Horizon workspace could not be inspected: {error}")
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
        cmake_build(REPO_ROOT, configuration, target, target_family)
    elif args.command == "build-fast":
        ensure_workspace(REPO_ROOT)
        cmake_build(REPO_ROOT, configuration, target, target_family)
    elif args.command == "rebuild":
        safe_remove(REPO_ROOT, build_dir(REPO_ROOT, configuration, target_family))
        install(configuration, target_family)
        cmake_configure(REPO_ROOT, configuration, target_family)
        cmake_build(REPO_ROOT, configuration, target, target_family)
    elif args.command == "update":
        update_workspace(REPO_ROOT)
        install(configuration, target_family, update=True)
        cmake_configure(REPO_ROOT, configuration, target_family)
    elif args.command == "horizon-sync":
        sync_workspace(REPO_ROOT)
        print(inspect_workspace(REPO_ROOT).summary())
    elif args.command == "clean":
        clean_repo(REPO_ROOT)
    else:
        raise RuntimeError(f"Unsupported command: {args.command}")


def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="CoronaEngine developer workflow",
        epilog=(
            "Horizon lock drift: 'horizon-sync' restores the commit pinned by the lock file "
            "(refused when the workspace is dirty); 'update' moves the lock to the ref tip."
        ),
    )
    parser.add_argument(
        "command", nargs="?", default="status",
        choices=(
            "status", "install", "configure", "build", "build-fast", "rebuild", "update", "horizon-sync",
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

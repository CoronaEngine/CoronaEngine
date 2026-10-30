"""Paths shared by build commands; never depend on the caller's working directory."""

from pathlib import Path


BUILDER_ROOT = Path(__file__).resolve().parent
REPO_ROOT = BUILDER_ROOT.parent
LOCAL_RECIPES = tuple(
    f"builder/conan/recipes/{name}"
    for name in (
        "ktm", "pfr", "slang", "vulkan-memory-allocator",
        "astc-encoder", "cef-binary", "ffmpeg",
    )
)


def builder_dir(repo_root: Path) -> Path:
    return repo_root / "builder"


def conanfile_path(repo_root: Path) -> Path:
    return builder_dir(repo_root) / "conanfile.py"


def profiles_dir(repo_root: Path) -> Path:
    return builder_dir(repo_root) / "conan" / "profiles"

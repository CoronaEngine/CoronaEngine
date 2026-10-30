"""Paths shared by build commands; never depend on the caller's working directory."""

from pathlib import Path


BUILD_TOOLS_ROOT = Path(__file__).resolve().parent
REPO_ROOT = BUILD_TOOLS_ROOT.parent
WORKSPACE_LOCK_DIR = Path("build/.locks")
LOCAL_RECIPES = tuple(
    f"build_tools/conan/recipes/{name}"
    for name in (
        "ktm", "pfr", "slang", "vulkan-memory-allocator",
        "astc-encoder", "cef-binary", "ffmpeg",
    )
)


def build_tools_dir(repo_root: Path) -> Path:
    return repo_root / "build_tools"


def conanfile_path(repo_root: Path) -> Path:
    return build_tools_dir(repo_root) / "conanfile.py"


def profiles_dir(repo_root: Path) -> Path:
    return build_tools_dir(repo_root) / "conan" / "profiles"

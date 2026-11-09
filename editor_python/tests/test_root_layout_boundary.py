from pathlib import Path


def test_cmake_modules_live_under_build_tools():
    repo_root = Path(__file__).resolve().parents[2]
    cmake_root = repo_root / "build_tools" / "cmake"
    root_cmake = (repo_root / "CMakeLists.txt").read_text(encoding="utf-8")

    assert cmake_root.is_dir()
    assert (cmake_root / "corona_options.cmake").is_file()
    assert "build_tools/cmake/corona_paths.cmake" in root_cmake

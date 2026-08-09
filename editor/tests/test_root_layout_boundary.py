from pathlib import Path


def test_cmake_modules_have_a_named_root_owner():
    repo_root = Path(__file__).resolve().parents[1].parent
    cmake_root = repo_root / "cmake"
    misc_root = repo_root / "misc"
    legacy_root = repo_root / "misc" / "cmake"
    root_cmake = (repo_root / "CMakeLists.txt").read_text(encoding="utf-8")

    assert cmake_root.is_dir()
    assert (cmake_root / "corona_options.cmake").is_file()
    assert not misc_root.exists()
    assert not any(legacy_root.glob("*.cmake"))
    assert '"${CMAKE_CURRENT_SOURCE_DIR}/cmake"' in root_cmake

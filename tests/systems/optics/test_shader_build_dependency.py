from pathlib import Path


def test_optics_sources_explicitly_depend_on_gizmo_shader():
    cmake = (
        Path(__file__).resolve().parents[3]
        / "src"
        / "systems"
        / "optics"
        / "CMakeLists.txt"
    ).read_text(encoding="utf-8")

    assert "OBJECT_DEPENDS" in cmake
    assert "OPTICS_GIZMO_SHADER" in cmake
    assert "optics_system.cpp" in cmake
    assert "hardware.h" in cmake

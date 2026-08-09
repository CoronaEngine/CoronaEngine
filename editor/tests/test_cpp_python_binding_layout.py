from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1].parent
PYTHON_BINDINGS_ROOT = REPO_ROOT / "src" / "systems" / "script" / "python"


def test_editor_compatibility_bindings_have_a_separate_translation_unit():
    engine_bindings = PYTHON_BINDINGS_ROOT / "engine_bindings.cpp"
    compat_bindings = PYTHON_BINDINGS_ROOT / "editor_compat_bindings.cpp"
    module_source = (PYTHON_BINDINGS_ROOT / "corona_engine_module.cpp").read_text(
        encoding="utf-8"
    )
    cmake_source = (
        REPO_ROOT / "src" / "systems" / "script" / "CMakeLists.txt"
    ).read_text(encoding="utf-8")

    assert compat_bindings.is_file()
    assert "editor_compat_bindings.cpp" in cmake_source
    assert "BindEditorCompatibility(m);" in module_source
    header_source = (
        REPO_ROOT
        / "src"
        / "systems"
        / "script"
        / "include"
        / "corona"
        / "systems"
        / "script"
        / "engine_scripts.h"
    ).read_text(encoding="utf-8")
    assert "void BindEditorCompatibility" in header_source

    engine_source = engine_bindings.read_text(encoding="utf-8")
    compat_source = compat_bindings.read_text(encoding="utf-8")
    for name in (
        "create_editor_actor",
        "remove_editor_actor",
        "get_editor_scene_snapshot",
        "set_editor_actor_transform",
        "capture_editor_camera_view",
    ):
        assert f'm.def("{name}"' not in engine_source
        assert f'm.def("{name}"' in compat_source


def test_editor_compatibility_binding_is_not_the_public_manifest_owner():
    source = (
        PYTHON_BINDINGS_ROOT / "editor_compat_bindings.cpp"
    ).read_text(encoding="utf-8")

    assert "cef_editor_api.cpp" in source
    assert "stable editor surface" in source
    assert "_invoke_cpp_editor_api" not in source


def test_legacy_camera_follow_bindings_are_isolated_from_script_runtime_bindings():
    engine_source = (PYTHON_BINDINGS_ROOT / "engine_bindings.cpp").read_text(
        encoding="utf-8"
    )
    compat_source = (PYTHON_BINDINGS_ROOT / "editor_compat_bindings.cpp").read_text(
        encoding="utf-8"
    )

    for name in (
        "camera_follow_set_target",
        "camera_follow_clear",
        "camera_follow_set_input_enabled",
        "camera_follow_inject_rmb",
    ):
        assert f'm.def("{name}"' not in engine_source
        assert f'm.def("{name}"' in compat_source


def test_editor_host_lifecycle_binding_isolated_from_script_runtime_bindings():
    engine_source = (PYTHON_BINDINGS_ROOT / "engine_bindings.cpp").read_text(
        encoding="utf-8"
    )
    compat_source = (PYTHON_BINDINGS_ROOT / "editor_compat_bindings.cpp").read_text(
        encoding="utf-8"
    )

    assert 'm.def("request_engine_exit"' not in engine_source
    assert 'm.def("request_engine_exit"' in compat_source
    assert 'm.def("drain_input_events"' in engine_source
    assert 'm.def("python_runtime_phase"' in engine_source
    assert 'm.def("drain_input_events"' not in compat_source
    assert 'm.def("python_runtime_phase"' not in compat_source

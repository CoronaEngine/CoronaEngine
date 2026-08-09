from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1].parent
PYTHON_BINDINGS_ROOT = REPO_ROOT / "src" / "systems" / "script" / "python"


def test_editor_host_bindings_are_narrow_and_separate_from_script_runtime():
    host = PYTHON_BINDINGS_ROOT / "editor_host_bindings.cpp"
    engine = (PYTHON_BINDINGS_ROOT / "engine_bindings.cpp").read_text(encoding="utf-8")
    module = (PYTHON_BINDINGS_ROOT / "corona_engine_module.cpp").read_text(encoding="utf-8")
    cmake = (REPO_ROOT / "src/systems/script/CMakeLists.txt").read_text(encoding="utf-8")
    header = (PYTHON_BINDINGS_ROOT.parent / "include/corona/systems/script/engine_scripts.h").read_text(encoding="utf-8")

    assert host.is_file()
    assert "editor_host_bindings.cpp" in cmake
    assert "BindEditorHost(m);" in module
    assert "void BindEditorHost" in header
    assert 'm.def("request_engine_exit"' not in engine
    assert 'm.def("create_editor_actor"' not in host.read_text(encoding="utf-8")
    assert 'm.def("camera_follow_set_input_enabled"' in host.read_text(encoding="utf-8")


def test_removed_raw_editor_network_bindings_are_not_compiled():
    module = (PYTHON_BINDINGS_ROOT / "corona_engine_module.cpp").read_text(encoding="utf-8")
    cmake = (REPO_ROOT / "src/systems/script/CMakeLists.txt").read_text(encoding="utf-8")
    header = (PYTHON_BINDINGS_ROOT.parent / "include/corona/systems/script/engine_scripts.h").read_text(encoding="utf-8")
    assert not (PYTHON_BINDINGS_ROOT / "editor_network_bindings.cpp").is_file()
    assert "editor_network_bindings.cpp" not in cmake
    assert "BindEditorNetwork" not in module
    assert "BindEditorNetwork" not in header

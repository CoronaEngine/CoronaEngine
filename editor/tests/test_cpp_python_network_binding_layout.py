from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1].parent
PYTHON_BINDINGS_ROOT = REPO_ROOT / "src" / "systems" / "script" / "python"


def test_editor_network_bindings_have_a_separate_translation_unit():
    engine_bindings = PYTHON_BINDINGS_ROOT / "engine_bindings.cpp"
    network_bindings = PYTHON_BINDINGS_ROOT / "editor_network_bindings.cpp"
    module_source = (PYTHON_BINDINGS_ROOT / "corona_engine_module.cpp").read_text(
        encoding="utf-8"
    )
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
    cmake_source = (
        REPO_ROOT / "src" / "systems" / "script" / "CMakeLists.txt"
    ).read_text(encoding="utf-8")

    assert network_bindings.is_file()
    assert "editor_network_bindings.cpp" in cmake_source
    assert "void BindEditorNetwork" in header_source
    assert "BindEditorNetwork(m);" in module_source

    engine_source = engine_bindings.read_text(encoding="utf-8")
    network_source = network_bindings.read_text(encoding="utf-8")
    assert 'm.def("network_' not in engine_source
    for name in (
        "network_local_peer_id",
        "network_send_agent_reply",
        "network_pop_lanchat_sync_event",
        "network_broadcast_intent",
    ):
        assert f'm.def("{name}"' in network_source


def test_editor_network_binding_is_compatibility_only():
    source = (
        PYTHON_BINDINGS_ROOT / "editor_network_bindings.cpp"
    ).read_text(encoding="utf-8")

    assert "editor/AITool" in source
    assert "manifest-backed network" in source
    assert "_invoke_cpp_editor_api" not in source

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
BINDINGS_ROOT = REPO_ROOT / "src" / "systems" / "script" / "python"


def test_editor_aggregate_manifest_owns_vision_and_media_entries():
    engine_source = (BINDINGS_ROOT / "engine_bindings.cpp").read_text(encoding="utf-8")
    manifest_source = (REPO_ROOT / "src/systems/ui/editor_api/cef_editor_api.cpp").read_text(encoding="utf-8")

    for entry in (
        '"scene_tools.is_vision_available"',
        '"scene_tools.set_render_backend"',
        '"scene_tools.load_vision_scene"',
        '"scene_tools.play_audio"',
        '"scene_tools.stop_audio"',
    ):
        assert entry not in engine_source
        assert entry in manifest_source


def test_engine_binding_remains_script_runtime_owner():
    source = (BINDINGS_ROOT / "engine_bindings.cpp").read_text(encoding="utf-8")
    assert 'nb::class_<Geometry>' in source
    assert 'nb::class_<Mechanics>' in source
    assert 'm.def("drain_input_events"' in source

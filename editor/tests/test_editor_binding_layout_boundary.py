from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
BINDINGS_ROOT = REPO_ROOT / "src" / "systems" / "script" / "python"


def test_editor_compat_binding_owns_editor_vision_and_media_entries():
    engine_source = (BINDINGS_ROOT / "engine_bindings.cpp").read_text(encoding="utf-8")
    compat_source = (BINDINGS_ROOT / "editor_compat_bindings.cpp").read_text(
        encoding="utf-8"
    )

    for entry in (
        'm.def("is_vision_available"',
        'm.def("set_render_backend"',
        'm.def("get_render_backend"',
        'm.def("set_vision_render_mode"',
        'm.def("get_vision_render_mode"',
        'm.def("load_vision_scene"',
        'm.def("load_vision_scene_from_json"',
        'nb::class_<MediaInfo>',
        'm.def("import_media"',
        'm.def("play_audio"',
        'm.def("stop_audio"',
    ):
        assert entry not in engine_source
        assert entry in compat_source


def test_engine_binding_remains_script_runtime_owner():
    source = (BINDINGS_ROOT / "engine_bindings.cpp").read_text(encoding="utf-8")
    assert 'nb::class_<Geometry>' in source
    assert 'nb::class_<Mechanics>' in source
    assert 'm.def("drain_input_events"' in source

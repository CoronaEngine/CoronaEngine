from pathlib import Path


HANDLERS = Path(__file__).resolve().parents[4] / "src/systems/ui/cef/cef_editor_native_api_handlers.cpp"


def test_native_scene_reload_uses_current_project_and_does_not_write_an_index():
    source = HANDLERS.read_text(encoding="utf-8")
    start = source.index('{"reload_scene"')
    reload_source = source[start:source.index('{"rebind_actor_resource"', start)]
    assert "reload_native_editor_scene(path_to_utf8(current->project_root), current->route)" in reload_source
    assert "persist_native_project_scene_index" not in reload_source


def test_direct_launch_materializes_a_scene_for_readers():
    source = HANDLERS.read_text(encoding="utf-8")
    start = source.index('{"on_init"', source.index('void register_main_view_api_handlers'))
    init_source = source[start:source.index('{"scene_save"', start)]
    assert "reload_native_editor_scene" in init_source
    assert "make_native_main_init_payload" in init_source
    assert "enqueue_python_project_context_changed" not in init_source

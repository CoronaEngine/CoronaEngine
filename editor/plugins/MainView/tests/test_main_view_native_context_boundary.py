from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
MAIN_VIEW = REPO_ROOT / "plugins" / "MainView" / "compat" / "legacy_main_view.py"
HANDLERS = (
    REPO_ROOT
    / ".."
    / "src"
    / "systems"
    / "ui"
    / "cef"
    / "cef_editor_native_api_handlers.cpp"
).resolve()


def test_main_view_uses_native_project_context_instead_of_settings_manager():
    source = MAIN_VIEW.read_text(encoding="utf-8")
    assert "settings_manager" not in source
    assert "CoronaEditorApi.project_settings.get_active_project_info" in source
    assert "_sync_project_field" not in source
    assert "_save_project_field" not in source


def test_native_scene_reload_persists_active_scene_index():
    source = HANDLERS.read_text(encoding="utf-8")
    start = source.index("void register_scene_tools_api_handlers")
    section = source[start:source.index("void register_network_api_handlers", start)]
    reload_start = section.index('{"reload_scene"')
    reload_source = section[reload_start:section.index('{"rebind_actor_resource"', reload_start)]
    assert "build_native_project_scene_index" in reload_source
    assert "persist_native_project_scene_index" in reload_source


def test_native_main_init_updates_python_project_context_for_direct_launches():
    source = HANDLERS.read_text(encoding="utf-8")
    start = source.index('void register_main_view_api_handlers')
    section = source[start:source.index('void register_project_settings_api_handlers', start)]
    init_start = section.index('{"on_init"')
    init_source = section[init_start:section.index('{"create_scene"', init_start)]
    assert "enqueue_python_project_context_changed" in init_source

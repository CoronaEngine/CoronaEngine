from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]


def test_scene_datas_compatibility_plugin_is_removed_after_object_panel_migration():
    plugin_root = EDITOR_ROOT / "plugins" / "SceneDatas"
    assert not (plugin_root / "main.py").exists()
    assert not (plugin_root / "COMPATIBILITY.md").exists()


def test_object_panel_does_not_call_legacy_scene_datas_namespace():
    source = (
        EDITOR_ROOT / "Frontend" / "src" / "views" / "sidebar" / "Object.vue"
    ).read_text(encoding="utf-8")

    assert "editorApi.scene.getActor" in source
    assert "editorApi.sceneTools" in source
    assert "sceneDatas" not in source
    assert "scene_datas" not in source


def test_object_panel_uses_a_vue_identity_separate_from_legacy_scene_datas_service():
    frontend_root = EDITOR_ROOT / "Frontend" / "src"
    manifest = (frontend_root / "config" / "pluginManifest.js").read_text(
        encoding="utf-8"
    )
    registry = (frontend_root / "views" / "panelRegistry.js").read_text(
        encoding="utf-8"
    )
    router = (frontend_root / "router" / "index.js").read_text(encoding="utf-8")

    assert "id: 'Object'" in manifest
    assert "displayNameKey: 'plugins.Object'" in manifest
    assert "Object: ObjectPanel" in registry
    assert "getPluginComponent('Object')" in router
    assert "id: 'SceneDatas'" not in manifest


def test_scene_datas_cpp_compatibility_module_is_removed_after_native_facade_migration():
    api_source = (
        EDITOR_ROOT
        / ".."
        / "src"
        / "systems"
        / "ui"
        / "editor_api"
        / "cef_editor_api.cpp"
    ).read_text(encoding="utf-8")
    handler_source = (
        EDITOR_ROOT
        / ".."
        / "src"
        / "systems"
        / "ui"
        / "cef"
        / "cef_editor_native_api_handlers.cpp"
    ).read_text(encoding="utf-8")
    registry_source = (
        EDITOR_ROOT
        / ".."
        / "src"
        / "systems"
        / "ui"
        / "cef"
        / "cef_editor_native_api_registry.cpp"
    ).read_text(encoding="utf-8")

    assert "EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneDatas" not in api_source
    assert 'registry.register_module("SceneDatas"' not in handler_source
    assert "register_scene_datas_api_handlers" not in registry_source

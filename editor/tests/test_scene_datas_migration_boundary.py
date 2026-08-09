from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]


def test_scene_datas_document_records_the_in_repo_object_panel_migration():
    source = (
        EDITOR_ROOT / "plugins" / "SceneDatas" / "COMPATIBILITY.md"
    ).read_text(encoding="utf-8")

    assert "仓内 Object 面板已迁移到 manifest 聚合接口" in source
    assert "SceneDatas 注册壳仍仅用于外部兼容" in source


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

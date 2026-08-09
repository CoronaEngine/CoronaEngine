from pathlib import Path


FRONTEND_ROOT = Path(__file__).resolve().parents[2]
SRC_ROOT = FRONTEND_ROOT / "src"

EXPECTED_DIRECTORIES = (
    "api",
    "assets",
    "blockly",
    "components",
    "composables",
    "config",
    "i18n",
    "router",
    "services",
    "stores",
    "utils",
    "views",
)


def test_frontend_source_tree_has_a_local_boundary_inventory():
    boundary = SRC_ROOT / "BOUNDARY.md"

    assert boundary.is_file()
    source = boundary.read_text(encoding="utf-8")
    for marker in (
        "公共契约",
        "兼容入口",
        "领域 service",
        "页面和路由视图",
        "低延迟输入 adapter",
        "删除条件",
    ):
        assert marker in source
    for directory in EXPECTED_DIRECTORIES:
        assert f"`{directory}/`" in source


def test_frontend_source_tree_declares_the_single_transport_and_compatibility_owners():
    source = (SRC_ROOT / "BOUNDARY.md").read_text(encoding="utf-8")

    assert "api/editorApi.js" in source
    assert "services/" in source
    assert "compat/" not in source
    assert "utils/bridge.js" not in source
    assert "不得" in source


def test_frontend_boundary_documents_existing_local_sub_boundaries():
    source = (SRC_ROOT / "BOUNDARY.md").read_text(encoding="utf-8")

    for relative_path in (
        "api/BOUNDARY.md",
        "blockly/BOUNDARY.md",
        "services/BOUNDARY.md",
        "utils/BOUNDARY.md",
    ):
        assert relative_path in source


def test_panel_metadata_does_not_import_vue_components():
    manifest = (SRC_ROOT / "config" / "pluginManifest.js").read_text(encoding="utf-8")

    assert "from '@/views/" not in manifest
    assert "from '@/components/" not in manifest
    assert "from '@/i18n/" not in manifest
    assert "component:" not in manifest


def test_panel_components_have_a_dedicated_registry_owner():
    registry_path = SRC_ROOT / "views" / "panelRegistry.js"
    assert registry_path.is_file()

    registry = registry_path.read_text(encoding="utf-8")
    for panel in (
        "SceneBar",
        "ObjectPanel",
        "Pet",
        "LogView",
        "FileManager",
        "ProjectSettings",
        "NodeGraphPanel",
        "CabbageChatPanel",
        "EditorSettings",
        "NetworkPanel",
        "LightFieldCalibrationPanel",
    ):
        assert panel in registry


def test_reusable_components_do_not_own_page_view_registration():
    assert not (SRC_ROOT / "components" / "panelRegistry.js").exists()


def test_reusable_components_do_not_import_page_views():
    component_sources = list((SRC_ROOT / "components").rglob("*.js")) + list(
        (SRC_ROOT / "components").rglob("*.vue")
    )

    for path in component_sources:
        assert "@/views/" not in path.read_text(encoding="utf-8"), path

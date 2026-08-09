from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]
PLUGIN_ROOT = EDITOR_ROOT / "plugins" / "MainView"


def test_main_view_document_records_native_lifecycle_completion_for_repo_callers():
    source = (PLUGIN_ROOT / "COMPATIBILITY.md").read_text(encoding="utf-8")

    assert "仓内生产代码已完成 native 项目/场景生命周期迁移" in source
    assert "legacy adapter 当前无仓内生产调用" in source
    assert "关闭和外部宿主兼容仍需确认" in source


def test_main_view_production_source_does_not_import_legacy_scene_adapter():
    source = (PLUGIN_ROOT / "compat" / "legacy_main_view.py").read_text(encoding="utf-8")

    assert "legacy_main_view_scene_adapter" not in source
    assert "CoronaEditorApi.main.on_init" in source
    assert "CoronaEditorApi.main.create_scene" in source
    assert "CoronaEditorApi.main.remove_scene" in source
    assert "CoronaEditorApi.scene_tools.reload_scene" in source

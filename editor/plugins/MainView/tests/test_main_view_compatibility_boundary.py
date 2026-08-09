from pathlib import Path


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
MAIN_VIEW_SOURCE = PLUGIN_ROOT / "main.py"
ADAPTER_SOURCE = PLUGIN_ROOT / "compat" / "legacy_main_view_scene_adapter.py"
ADAPTER_SHIM = PLUGIN_ROOT.parents[1] / "runtime" / "legacy_main_view_scene_adapter.py"


def test_main_view_has_a_local_migration_inventory():
    inventory = PLUGIN_ROOT / "COMPATIBILITY.md"

    assert inventory.is_file()
    source = inventory.read_text(encoding="utf-8")
    assert "plugins/MainView/compat/legacy_main_view_scene_adapter.py" in source
    assert "project.*" in source
    assert "scene.*" in source
    assert "删除条件" in source


def test_main_view_no_longer_hosts_unused_legacy_scene_lifecycle_hooks():
    main_source = MAIN_VIEW_SOURCE.read_text(encoding="utf-8")
    adapter_source = ADAPTER_SOURCE.read_text(encoding="utf-8")

    assert "from plugins.MainView.compat.legacy_main_view_scene_adapter import" not in main_source
    assert "_discard_python_runtime_scene" not in main_source
    assert "from runtime.legacy_scene_store import" in adapter_source
    assert "from CoronaCore.core.legacy.managers import scene_manager" not in main_source
    assert "from runtime.legacy.managers import scene_manager" not in main_source
    for operation in ("get_or_create_scene", "get_scene", "remove_scene", "discard_scene"):
        assert operation in adapter_source
    assert ADAPTER_SHIM.is_file()
    assert "from plugins.MainView.compat.legacy_main_view_scene_adapter import *" in ADAPTER_SHIM.read_text(
        encoding="utf-8"
    )


def test_main_view_keeps_public_operations_on_aggregate_contracts():
    source = MAIN_VIEW_SOURCE.read_text(encoding="utf-8")

    assert "CoronaEditorApi.scene.get_snapshot" in source
    assert "CoronaEditorApi.main.scene_save" in source
    assert "CoronaEditorApi.scene_tools" in source
    assert "from CoronaCore.core.corona_editor import CoronaEditor" not in source

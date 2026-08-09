from pathlib import Path


ROOT = Path(__file__).parents[1]
ADAPTER = (ROOT / "cai_extensions" / "mcp" / "tools" / "native_scene_state.py").read_text(
    encoding="utf-8"
)
SCENE_STORE = (ROOT.parents[1] / "runtime" / "legacy_scene_store.py").read_text(
    encoding="utf-8"
)


def test_legacy_scene_manager_import_is_owned_by_native_scene_adapter():
    files = [
        ROOT / "cai_extensions" / "agent" / "agent_adapter.py",
        ROOT / "cai_extensions" / "agent" / "model_reviewer.py",
        ROOT / "cai_extensions" / "agent" / "vlm_capture.py",
        ROOT / "cai_extensions" / "agent" / "scene_composer.py",
        ROOT / "cai_extensions" / "agent" / "scene_composer_progressive.py",
        ROOT / "cai_extensions" / "agent" / "vlm_capture.py",
        ROOT / "cai_extensions" / "mcp" / "tools" / "camera_tools.py",
        ROOT / "cai_extensions" / "mcp" / "tools" / "multi_view_capture.py",
        ROOT / "cai_extensions" / "mcp" / "tools" / "scene_tools.py",
        ROOT / "cai_extensions" / "flows" / "scene_composition_workflow_v2" / "nodes_tier_place.py",
    ]
    assert "from runtime.legacy.managers import scene_manager" in SCENE_STORE
    assert not (ROOT / "compat" / "legacy_aitool_scene_adapter.py").exists()
    assert "from plugins.AITool.compat.legacy_aitool_scene_adapter import" not in ADAPTER
    assert "get_legacy_scene" not in ADAPTER
    assert not (ROOT.parents[1] / "runtime" / "legacy_aitool_scene_adapter.py").exists()
    assert "from runtime.legacy_scene_store import" not in ADAPTER
    assert "from CoronaCore.core.managers import scene_manager" not in ADAPTER
    for path in files:
        source = path.read_text(encoding="utf-8")
        assert "from CoronaCore.core.legacy.managers import scene_manager" not in source, path
        assert "scene_manager.get(" not in source, path
        assert "scene_manager.list_all(" not in source, path


def test_vlm_capture_uses_the_centralized_scene_value_resolver():
    source = (
        ROOT / "cai_extensions" / "agent" / "vlm_capture.py"
    ).read_text(encoding="utf-8")

    assert "resolve_native_scene_value" in source
    assert "get_legacy_scene" not in source


def test_scene_list_uses_native_scene_routes_without_legacy_store_fallback():
    source = (
        ROOT / "cai_extensions" / "mcp" / "tools" / "scene_tools.py"
    ).read_text(encoding="utf-8")

    assert "CoronaEditorApi.scene.list_routes" in source
    assert "list_legacy_scene_routes" not in source


def test_aitool_business_modules_do_not_import_engine_entity_wrappers():
    files = [
        ROOT / "cai_extensions" / "agent" / "agent_adapter.py",
        ROOT / "cai_extensions" / "agent" / "model_reviewer.py",
        ROOT / "cai_extensions" / "agent" / "scene_composer.py",
        ROOT / "cai_extensions" / "mcp" / "tools" / "camera_tools.py",
        ROOT / "cai_extensions" / "mcp" / "tools" / "scene_tools.py",
        ROOT / "cai_extensions" / "flows" / "scene_composition_workflow_v2" / "nodes_tier_place.py",
    ]
    for path in files:
        source = path.read_text(encoding="utf-8")
        assert "CoronaCore.core.entities" not in source, path
        assert "from CoronaCore.core.entities" not in source, path

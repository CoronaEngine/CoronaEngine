from pathlib import Path


ROOT = Path(__file__).parents[1]
ADAPTER = (ROOT / "cai_extensions" / "mcp" / "tools" / "native_scene_state.py").read_text(
    encoding="utf-8"
)
LEGACY_ADAPTER_PATH = ROOT / "compat" / "legacy_aitool_scene_adapter.py"
LEGACY_ADAPTER_SHIM_PATH = ROOT.parents[1] / "runtime" / "legacy_aitool_scene_adapter.py"
LEGACY_ADAPTER = (
    LEGACY_ADAPTER_PATH.read_text(encoding="utf-8")
    if LEGACY_ADAPTER_PATH.is_file()
    else ""
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
    assert LEGACY_ADAPTER_PATH.is_file()
    assert "from runtime.legacy_scene_store import legacy_scene_store" in LEGACY_ADAPTER
    assert "def get_legacy_scene(" in LEGACY_ADAPTER
    assert "def list_legacy_scene_routes(" in LEGACY_ADAPTER
    assert "from plugins.AITool.compat.legacy_aitool_scene_adapter import" in ADAPTER
    assert LEGACY_ADAPTER_SHIM_PATH.is_file()
    assert "from plugins.AITool.compat.legacy_aitool_scene_adapter import *" in LEGACY_ADAPTER_SHIM_PATH.read_text(
        encoding="utf-8"
    )
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

    assert "resolve_scene_value" in source
    assert "get_legacy_scene" not in source


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

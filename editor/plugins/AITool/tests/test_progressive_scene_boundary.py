import sys
from pathlib import Path
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[4]
EDITOR_ROOT = PROJECT_ROOT / "editor"
AI_TOOL_ROOT = EDITOR_ROOT / "plugins" / "AITool"
for path in (PROJECT_ROOT, EDITOR_ROOT, AI_TOOL_ROOT):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

from cai_extensions.agent import scene_composer_progressive
from cai_extensions.mcp.tools import native_scene_state


def test_progressive_workflow_uses_native_scene_value_object():
    source = Path(scene_composer_progressive.__file__).read_text(encoding="utf-8")
    assert "resolve_scene_value" in source
    assert "get_legacy_scene" not in source
    actor = mock.Mock(name="actor")
    with mock.patch(
        "CoronaCore.core.editor_api.get_scene_adapter", return_value=object()
    ), mock.patch(
        "cai_extensions.mcp.tools.native_scene_state.get_native_scene_snapshot",
        return_value={"scene": "Scene/current.scene", "scene_name": "Current"},
    ), mock.patch(
        "cai_extensions.mcp.tools.native_scene_state.find_native_actor",
        return_value=actor,
    ), mock.patch(
        "CoronaCore.core.managers.scene_manager"
    ) as legacy_manager:
        scene = scene_composer_progressive._get_current_scene()
        resolved = scene.get_actor("Chair")

    assert isinstance(scene, native_scene_state.NativeSceneRef)
    assert scene.route == "Scene/current.scene"
    assert resolved is actor
    legacy_manager.get.assert_not_called()

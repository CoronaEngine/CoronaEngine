import sys
from pathlib import Path
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[4]
EDITOR_ROOT = PROJECT_ROOT / "editor"
AI_TOOL_ROOT = EDITOR_ROOT / "plugins" / "AITool"
for path in (PROJECT_ROOT, EDITOR_ROOT, AI_TOOL_ROOT):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

from cai_extensions.mcp.tools import camera_tools
from cai_extensions.mcp.tools import multi_view_capture
from cai_extensions.agent import vlm_capture


def test_camera_scene_resolution_prefers_manifest_scene_route():
    with mock.patch(
        "CoronaCore.core.editor_api.get_scene_adapter", return_value=object()
    ), mock.patch(
        "cai_extensions.mcp.tools.native_scene_state.get_native_scene_snapshot",
        return_value={"scene": "Scene/current.scene", "scene_name": "Current"},
    ), mock.patch(
        "CoronaCore.core.managers.scene_manager"
    ) as legacy_manager:
        scene = camera_tools._resolve_scene(None, "")

    assert scene.route == "Scene/current.scene"
    assert scene.name == "Current"
    legacy_manager.get.assert_not_called()


def test_camera_scene_resolution_falls_back_to_explicit_legacy_manager():
    legacy_scene = mock.Mock(route="Scene/legacy.scene", name="Legacy")
    manager = mock.Mock()
    manager.get.return_value = legacy_scene

    with mock.patch(
        "CoronaCore.core.editor_api.get_scene_adapter", return_value=None
    ):
        scene = camera_tools._resolve_scene(manager, "Scene/legacy.scene")

    assert scene is legacy_scene
    manager.get.assert_called_once_with("Scene/legacy.scene")


def test_vlm_scene_resolution_prefers_native_snapshot():
    with mock.patch(
        "api.editor_api.get_scene_adapter", return_value=object()
    ), mock.patch(
        "cai_extensions.mcp.tools.native_scene_state.get_native_scene_snapshot",
        return_value={"scene": "Scene/current.scene", "scene_name": "Current"},
    ), mock.patch(
        "CoronaCore.core.managers.scene_manager"
    ) as legacy_manager:
        scene = vlm_capture._resolve_scene("")

    assert scene.route == "Scene/current.scene"
    assert scene.name == "Current"
    legacy_manager.get.assert_not_called()


def test_vlm_screenshot_fallback_uses_canonical_project_context(monkeypatch, tmp_path):
    project_root = tmp_path / "ActiveProject"
    project_root.mkdir()
    monkeypatch.delenv("CAI_SCREENSHOTS_DIR", raising=False)
    monkeypatch.delenv("CAI_PROJECT_ROOT", raising=False)
    monkeypatch.setattr(
        "runtime.project_context.get_project_root",
        lambda: project_root,
    )

    with mock.patch(
        "config.paths_config.get_project_screenshots_dir",
        side_effect=RuntimeError("path resolver unavailable"),
    ):
        screenshots_root = vlm_capture.get_project_screenshots_root()

    assert screenshots_root == project_root / "screenshots"


def test_multi_view_scene_resolution_prefers_native_snapshot():
    with mock.patch(
        "CoronaCore.core.editor_api.get_scene_adapter", return_value=object()
    ), mock.patch(
        "cai_extensions.mcp.tools.native_scene_state.get_native_scene_snapshot",
        return_value={"scene": "Scene/current.scene", "scene_name": "Current"},
    ), mock.patch(
        "CoronaCore.core.managers.scene_manager"
    ) as legacy_manager:
        scene = multi_view_capture._resolve_scene(None, "")

    assert scene.route == "Scene/current.scene"
    assert scene.name == "Current"
    legacy_manager.get.assert_not_called()

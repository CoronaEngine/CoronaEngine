import sys
from pathlib import Path
from unittest import mock

import pytest

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
        "cai_extensions.mcp.tools.native_scene_state.get_native_scene_snapshot",
        return_value={"scene": "Scene/current.scene", "scene_name": "Current"},
    ):
        scene = camera_tools._resolve_scene(None, "")

    assert scene.route == "Scene/current.scene"
    assert scene.name == "Current"


def test_camera_scene_resolution_fails_closed_without_native_contract():
    manager = mock.Mock()

    with mock.patch(
        "cai_extensions.mcp.tools.native_scene_state.get_native_scene_snapshot",
        side_effect=RuntimeError("native scene unavailable"),
    ), pytest.raises(RuntimeError, match="native scene unavailable"):
        camera_tools._resolve_scene(manager, "Scene/legacy.scene")

    manager.get.assert_not_called()


def test_vlm_scene_resolution_prefers_native_snapshot():
    with mock.patch(
        "cai_extensions.mcp.tools.native_scene_state.get_native_scene_snapshot",
        return_value={"scene": "Scene/current.scene", "scene_name": "Current"},
    ):
        scene = vlm_capture._resolve_scene("")

    assert scene.route == "Scene/current.scene"
    assert scene.name == "Current"


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
        "cai_extensions.mcp.tools.native_scene_state.get_native_scene_snapshot",
        return_value={"scene": "Scene/current.scene", "scene_name": "Current"},
    ):
        scene = multi_view_capture._resolve_scene(None, "")

    assert scene.route == "Scene/current.scene"
    assert scene.name == "Current"


def test_camera_tools_do_not_keep_python_scene_camera_fallbacks():
    source = (
        AI_TOOL_ROOT / "cai_extensions" / "mcp" / "tools" / "camera_tools.py"
    ).read_text(encoding="utf-8")

    assert "scene.find_camera" not in source
    assert "scene.get_cameras" not in source
    assert "Explicit compatibility path" not in source
    assert "get_scene_adapter" not in source
    assert "get_scene_tools_adapter" not in source
    assert "get_viewport_adapter" not in source
    assert "resolve_native_scene_value" in source
    multi_view_source = (
        AI_TOOL_ROOT / "cai_extensions" / "mcp" / "tools" / "multi_view_capture.py"
    ).read_text(encoding="utf-8")
    assert "resolve_native_scene_value" in multi_view_source

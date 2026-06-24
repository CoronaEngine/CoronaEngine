"""Offline regression tests for camera_multiview_capture viewport safety."""
from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../../../../.."))
sys.path.insert(0, os.path.join(ROOT, "editor", "plugins", "AITool"))
sys.path.insert(0, os.path.join(ROOT, "editor"))
sys.path.insert(0, os.path.dirname(__file__))

import camera_tools  # noqa: E402


class FakeGeometry:
    def get_aabb(self):
        return [-0.5, 0.0, -0.5, 0.5, 1.0, 0.5]


class FakeActor:
    def __init__(self, name="chair"):
        self.name = name
        self._geometry = FakeGeometry()

    def get_position(self):
        return [0.0, 0.0, 0.0]

    def get_scale(self):
        return [1.0, 1.0, 1.0]


class FakeCamera:
    def __init__(self, name="main", **kwargs):
        self.name = name
        self.kwargs = kwargs
        self.position = [9.0, 9.0, 9.0]
        self.forward = [0.0, 0.0, 1.0]
        self.up = [0.0, 1.0, 0.0]
        self.fov = 45.0
        self.output_mode = kwargs.get("output_mode", "final_color")
        self.surface = 123
        self.offscreen_capture_mode = False
        self.offscreen_capture_calls = []
        self.set_calls = []
        self.async_screenshots = []
        self.sync_screenshots = []

    def set(self, position, forward, up, fov):
        self.position = list(position)
        self.forward = list(forward)
        self.up = list(up)
        self.fov = float(fov)
        self.set_calls.append((list(position), list(forward), list(up), float(fov)))

    def get_world_up(self):
        return list(self.up)

    def get_fov(self):
        return self.fov

    def get_output_mode(self):
        return self.output_mode

    def set_output_mode(self, mode):
        self.output_mode = mode

    def set_offscreen_capture_mode(self, enabled):
        self.offscreen_capture_mode = bool(enabled)
        self.offscreen_capture_calls.append(bool(enabled))
        if enabled:
            self.surface = 0

    def set_view_state(self, *_args):
        pass

    def set_surface(self, surface):
        self.surface = surface

    def get_surface(self):
        return self.surface

    def save_screenshot(self, path):
        self.async_screenshots.append(path)

    def save_screenshot_sync(self, path):
        self.sync_screenshots.append(path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(b"fake-png")
        return True


class FakeScene:
    def __init__(self):
        self.active = FakeCamera("main_camera")
        self.actor = FakeActor()
        self.cameras = [self.active]
        self.added = []

    def find_actor(self, name):
        return self.actor if name == self.actor.name else None

    def find_camera(self, name):
        if not name:
            return self.active
        for camera in self.cameras:
            if camera.name == name:
                return camera
        return None

    def ensure_default_camera(self):
        return False

    def get_active_camera(self):
        return self.active

    def add_camera_to_scene(self, camera):
        self.cameras.append(camera)
        self.added.append(camera)


class FakeSceneManager:
    def __init__(self, scene):
        self.scene = scene

    def list_all(self):
        return ["Scene/fake.scene"]

    def get(self, _route):
        return self.scene


def _parse_envelope(envelope):
    data = json.loads(envelope)
    text = data["llm_content"][0]["part"][0]["content_text"]
    return json.loads(text)


def test_multiview_uses_offscreen_review_camera_not_main():
    scene = FakeScene()
    tool = camera_tools._build_camera_multiview_tool(FakeSceneManager(scene))
    assert "output_modes" not in tool.args_schema.model_fields
    output_dir = tempfile.mkdtemp(prefix="camera_multiview_offscreen_")
    try:
        result = _parse_envelope(tool.invoke({
            "actor_name": "chair",
            "output_dir": output_dir,
        }))
    finally:
        shutil.rmtree(output_dir, ignore_errors=True)

    assert result["status"] == "success"
    assert result["view_count"] == 4
    assert result["output_mode"] == "base_color"
    assert "output_modes" not in result
    assert scene.active.set_calls == [], "main camera must not be moved by VLM multiview capture"
    assert scene.active.async_screenshots == []
    assert scene.active.sync_screenshots == []
    assert scene.added, "multiview capture should create an offscreen review camera"
    review_camera = scene.added[0]
    assert review_camera.name == "vlm_review_camera"
    assert review_camera.offscreen_capture_calls == []
    assert review_camera.get_surface() == 0
    assert len(review_camera.set_calls) == 4
    assert len(review_camera.sync_screenshots) == 4
    assert review_camera.async_screenshots == []
    print("[OK] camera_multiview_capture uses offscreen review camera")


def test_screenshot_uses_offscreen_review_camera_by_default():
    scene = FakeScene()
    tool = camera_tools._build_camera_screenshot_tool(FakeSceneManager(scene))
    assert "output_mode" not in tool.args_schema.model_fields
    output_path = os.path.join(tempfile.mkdtemp(prefix="camera_screenshot_offscreen_"), "shot.png")
    try:
        result = _parse_envelope(tool.invoke({
            "output_path": output_path,
        }))
    finally:
        shutil.rmtree(os.path.dirname(output_path), ignore_errors=True)

    assert result["status"] == "success"
    assert scene.active.async_screenshots == []
    assert scene.active.sync_screenshots == []
    assert scene.active.output_mode == "final_color"
    assert scene.added, "camera_screenshot should create an offscreen review camera by default"
    review_camera = scene.added[0]
    assert review_camera.name == "vlm_review_camera"
    assert review_camera.offscreen_capture_calls == []
    assert review_camera.get_surface() == 0
    assert review_camera.sync_screenshots == [output_path]
    assert review_camera.async_screenshots == []
    assert review_camera.output_mode == "base_color"
    assert result["output_mode"] == "base_color"
    print("[OK] camera_screenshot uses offscreen review camera by default")


if __name__ == "__main__":
    test_multiview_uses_offscreen_review_camera_not_main()
    test_screenshot_uses_offscreen_review_camera_by_default()

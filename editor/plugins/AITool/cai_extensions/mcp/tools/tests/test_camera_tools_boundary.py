from pathlib import Path
import sys

PROJECT_ROOT = Path(__file__).resolve().parents[7]
EDITOR_ROOT = PROJECT_ROOT / "editor"
AI_TOOL_ROOT = EDITOR_ROOT / "plugins" / "AITool"
for _path in (PROJECT_ROOT, EDITOR_ROOT, AI_TOOL_ROOT):
    if str(_path) not in sys.path:
        sys.path.append(str(_path))

from editor.plugins.AITool.cai_extensions.mcp.tools import camera_tools
from editor.plugins.AITool.cai_extensions.mcp.tools import native_scene_state
from api import editor_api


CAMERA_TOOLS_SOURCE = Path(__file__).resolve().parents[1] / "camera_tools.py"


def _install_native_api(monkeypatch, *, scene=None, scene_tools=None, viewport=None):
    class DefaultScene:
        @staticmethod
        def get_snapshot(scene_name):
            return {"status": "success", "scene": scene_name or "Scene/test.scene"}

    class NativeApi:
        pass

    NativeApi.scene = scene or DefaultScene()
    NativeApi.scene_tools = scene_tools
    NativeApi.viewport = viewport
    monkeypatch.setattr(native_scene_state, "EDITOR_API_OVERRIDE", NativeApi)
    monkeypatch.setattr(editor_api.CoronaEditorApi, "scene", NativeApi.scene)
    monkeypatch.setattr(editor_api.CoronaEditorApi, "scene_tools", NativeApi.scene_tools)
    monkeypatch.setattr(editor_api.CoronaEditorApi, "viewport", NativeApi.viewport)


def test_camera_screenshot_uses_viewport_aggregate_instead_of_camera_binding():
    source = CAMERA_TOOLS_SOURCE.read_text(encoding="utf-8")

    assert "CoronaEditorApi.viewport" in source
    assert "viewport.capture" in source
    assert "save_screenshot_sync" not in source


def test_camera_focus_prefers_scene_tools_aggregate_for_manifest_host():
    source = CAMERA_TOOLS_SOURCE.read_text(encoding="utf-8")

    assert "CoronaEditorApi.scene_tools" in source
    assert "focus_actor" in source


def test_camera_move_routes_pose_updates_through_viewport_adapter():
    source = CAMERA_TOOLS_SOURCE.read_text(encoding="utf-8")

    assert "CoronaEditorApi.viewport" in source
    assert "set_camera_pose" in source


def test_camera_tools_do_not_import_editor_runtime_container():
    source = CAMERA_TOOLS_SOURCE.read_text(encoding="utf-8")

    assert "from CoronaCore.core.corona_editor import CoronaEditor" not in source


def test_camera_move_passes_pose_value_object_to_viewport_adapter(monkeypatch):
    class Scene:
        route = "Scene/test.scene"

        @staticmethod
        def find_camera(_name):
            raise AssertionError("native camera_move must not mutate a Python camera")

    class SceneManager:
        @staticmethod
        def get(_name):
            return Scene()

        @staticmethod
        def list_all():
            return ["Scene/test.scene"]

    class Viewport:
        def __init__(self):
            self.calls = []

        def set_camera_pose(self, *args):
            self.calls.append(args)
            return {
                "status": "success",
                "camera": {
                    "name": "MainCamera",
                    "position": [1.0, 2.0, 3.0],
                    "forward": [0.0, 0.0, 1.0],
                    "world_up": [0.0, 1.0, 0.0],
                    "fov": 60.0,
                },
            }

    viewport = Viewport()
    class SceneApi:
        @staticmethod
        def get_snapshot(_scene_name):
            return {
                "status": "success",
                "camera": {
                    "name": "MainCamera",
                    "position": [1.0, 2.0, 3.0],
                    "forward": [0.0, 0.0, 1.0],
                    "world_up": [0.0, 1.0, 0.0],
                    "fov": 45.0,
                    "width": 640,
                    "height": 480,
                },
                "cameras": [],
            }

    _install_native_api(monkeypatch, scene=SceneApi(), viewport=viewport)

    tool = camera_tools._build_camera_move_tool(SceneManager())
    result = tool.func(
        scene_name="Scene/test.scene",
        camera_name="MainCamera",
        position=(1.0, 2.0, 3.0),
        forward=(0.0, 0.0, 1.0),
        up=(0.0, 1.0, 0.0),
        fov=60.0,
    )

    assert '"status_info": "success"' in result
    assert viewport.calls == [(
        "Scene/test.scene",
        "MainCamera",
        {
            "position": [1.0, 2.0, 3.0],
            "forward": [0.0, 0.0, 1.0],
            "world_up": [0.0, 1.0, 0.0],
            "fov": 60.0,
            "persist": True,
        },
    )]
    assert "MainCamera" in result


def test_camera_focus_uses_manifest_result_without_reading_python_actor_geometry(monkeypatch):
    class Scene:
        route = "Scene/test.scene"

    class SceneManager:
        @staticmethod
        def get(_name):
            return Scene()

        @staticmethod
        def list_all():
            return ["Scene/test.scene"]

    class SceneTools:
        def __init__(self):
            self.calls = []

        def focus_actor(self, *args):
            self.calls.append(args)
            return {
                "status": "success",
                "center": [1.0, 2.0, 3.0],
                "distance": 4.0,
                "camera": {"name": "MainCamera", "position": [1.0, 2.0, -1.0], "forward": [0.0, 0.0, 1.0]},
            }

    scene_tools = SceneTools()
    _install_native_api(monkeypatch, scene_tools=scene_tools)

    tool = camera_tools._build_camera_focus_tool(SceneManager())
    result = tool.func(scene_name="Scene/test.scene", actor_name="Chair")

    assert '"status_info": "success"' in result
    assert scene_tools.calls == [("Scene/test.scene", "Chair", "")]


def test_camera_screenshot_passes_camera_value_object_to_viewport(monkeypatch, tmp_path):
    class Camera:
        name = "MainCamera"
        width = 640
        height = 480

        @staticmethod
        def get_position():
            return [1.0, 2.0, 3.0]

        @staticmethod
        def get_forward():
            return [0.0, 0.0, 1.0]

        @staticmethod
        def get_world_up():
            return [0.0, 1.0, 0.0]

        @staticmethod
        def get_fov():
            return 45.0

    class Scene:
        route = "Scene/test.scene"

        @staticmethod
        def find_camera(_name):
            return Camera()

    class SceneManager:
        @staticmethod
        def get(_name):
            return Scene()

        @staticmethod
        def list_all():
            return ["Scene/test.scene"]

    class Viewport:
        def __init__(self):
            self.calls = []

        def capture(self, *args):
            self.calls.append(args)
            return {"status": "success", "path": args[-1]}

    viewport = Viewport()
    class SceneApi:
        @staticmethod
        def get_snapshot(_scene_name):
            return {
                "status": "success",
                "camera": {
                    "name": "MainCamera",
                    "position": [1.0, 2.0, 3.0],
                    "forward": [0.0, 0.0, 1.0],
                    "world_up": [0.0, 1.0, 0.0],
                    "fov": 45.0,
                    "width": 640,
                    "height": 480,
                },
                "cameras": [],
            }

    _install_native_api(monkeypatch, scene=SceneApi(), viewport=viewport)

    tool = camera_tools._build_camera_screenshot_tool(SceneManager())
    output_path = str(tmp_path / "shot.png")
    result = tool.func(scene_name="Scene/test.scene", output_path=output_path)

    assert '"status_info": "success"' in result
    assert viewport.calls[0][0:2] == ("Scene/test.scene", "MainCamera")
    assert viewport.calls[0][2]["output_mode"] == "base_color"
    assert viewport.calls[0][2]["width"] == 640


def test_camera_screenshot_reads_authoritative_camera_snapshot_on_manifest_host(monkeypatch, tmp_path):
    class Scene:
        route = "Scene/test.scene"

        @staticmethod
        def find_camera(_name):
            raise AssertionError("manifest screenshot must not read a Python camera object")

    class SceneManager:
        @staticmethod
        def get(_name):
            return Scene()

        @staticmethod
        def list_all():
            return ["Scene/test.scene"]

    class SceneApi:
        @staticmethod
        def get_snapshot(_scene_name):
            return {
                "route": "Scene/test.scene",
                "camera": {
                    "name": "MainCamera",
                    "position": [1.0, 2.0, 3.0],
                    "forward": [0.0, 0.0, 1.0],
                    "world_up": [0.0, 1.0, 0.0],
                    "fov": 45.0,
                    "width": 800,
                    "height": 600,
                    "render_backend": "native",
                    "vision_render_mode": "path_tracing",
                },
            }

    class Viewport:
        def __init__(self):
            self.calls = []

        def capture(self, *args):
            self.calls.append(args)
            return {"status": "success", "path": args[-1]}

    viewport = Viewport()
    _install_native_api(monkeypatch, scene=SceneApi(), viewport=viewport)

    tool = camera_tools._build_camera_screenshot_tool(SceneManager())
    output_path = str(tmp_path / "snapshot.png")
    result = tool.func(scene_name="Scene/test.scene", output_path=output_path)

    assert '"status_info": "success"' in result
    assert viewport.calls[0][0:2] == ("Scene/test.scene", "MainCamera")
    assert viewport.calls[0][2]["position"] == [1.0, 2.0, 3.0]
    assert viewport.calls[0][2]["width"] == 800


def test_camera_screenshot_does_not_fall_back_to_python_camera_when_manifest_has_no_camera(monkeypatch, tmp_path):
    class Scene:
        route = "Scene/test.scene"

        @staticmethod
        def find_camera(_name):
            raise AssertionError("manifest screenshot must not fall back to a Python camera")

    class SceneManager:
        @staticmethod
        def get(_name):
            return Scene()

        @staticmethod
        def list_all():
            return ["Scene/test.scene"]

    class SceneApi:
        @staticmethod
        def get_snapshot(_scene_name):
            return {"status": "success", "camera": None, "cameras": []}

    _install_native_api(monkeypatch, scene=SceneApi())

    tool = camera_tools._build_camera_screenshot_tool(SceneManager())
    result = tool.func(
        scene_name="Scene/test.scene",
        output_path=str(tmp_path / "missing.png"),
    )

    assert '"error_code": 1' in result


def test_camera_get_reads_authoritative_snapshot_on_manifest_host(monkeypatch):
    class Scene:
        route = "Scene/test.scene"

        @staticmethod
        def find_camera(_name):
            raise AssertionError("manifest camera_get must not read a Python camera object")

    class SceneManager:
        @staticmethod
        def get(_name):
            return Scene()

        @staticmethod
        def list_all():
            return ["Scene/test.scene"]

    class SceneApi:
        @staticmethod
        def get_snapshot(_scene_name):
            return {
                "status": "success",
                "camera": {
                    "name": "MainCamera",
                    "position": [1.0, 2.0, 3.0],
                    "forward": [0.0, 0.0, 1.0],
                    "world_up": [0.0, 1.0, 0.0],
                    "fov": 55.0,
                },
                "cameras": [],
            }

    _install_native_api(monkeypatch, scene=SceneApi())

    tool = camera_tools._build_camera_get_tool(SceneManager())
    result = tool.func(scene_name="Scene/test.scene")

    assert '"status_info": "success"' in result
    assert "MainCamera" in result
    assert "55.0" in result


def test_camera_list_reads_authoritative_snapshot_on_manifest_host(monkeypatch):
    class Scene:
        route = "Scene/test.scene"

        @staticmethod
        def get_cameras():
            raise AssertionError("manifest camera_list must not read Python camera objects")

    class SceneManager:
        @staticmethod
        def get(_name):
            return Scene()

        @staticmethod
        def list_all():
            return ["Scene/test.scene"]

    class SceneApi:
        @staticmethod
        def get_snapshot(_scene_name):
            return {
                "status": "success",
                "cameras": [
                    {
                        "name": "MainCamera",
                        "position": [1.0, 2.0, 3.0],
                        "fov": 55.0,
                    },
                    {"name": "ReviewCamera", "position": [4.0, 5.0, 6.0], "fov": 40.0},
                ],
            }

    _install_native_api(monkeypatch, scene=SceneApi())

    tool = camera_tools._build_camera_list_tool(SceneManager())
    result = tool.func(scene_name="Scene/test.scene")

    assert '"status_info": "success"' in result
    assert "MainCamera" in result
    assert "ReviewCamera" in result

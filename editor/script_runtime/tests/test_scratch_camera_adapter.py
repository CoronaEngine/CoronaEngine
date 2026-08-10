import unittest
from unittest import mock
from pathlib import Path
import sys
from types import SimpleNamespace

EDITOR_ROOT = Path(__file__).resolve().parents[2]
if str(EDITOR_ROOT) not in sys.path:
    sys.path.insert(0, str(EDITOR_ROOT))

from api import editor_api
from script_runtime.engine import corona_engine as corona_engine_scratch


class ScratchCameraAdapterTests(unittest.TestCase):
    def test_scene_snapshot_prefers_scene_contract(self):
        class Scene:
            def get_snapshot(self, scene_name):
                return {
                    "status": "success",
                    "scene": scene_name,
                    "actors": [{"name": "Box"}],
                }

        with mock.patch.object(
                 editor_api,
                 "get_script_runtime_editor_api",
                 return_value=SimpleNamespace(scene=Scene()),
             ):
            payload, errors = corona_engine_scratch._native_scene_snapshot("Scene/test.scene")

        self.assertEqual(payload["scene"], "Scene/test.scene")
        self.assertEqual(payload["actors"], [{"name": "Box"}])
        self.assertEqual(errors, [])

    def test_actor_transform_prefers_scene_contract(self):
        class Scene:
            def __init__(self):
                self.calls = []

            def set_actor_transform(self, *args):
                self.calls.append(args)
                return {"status": "success", "actor": {"name": "Box"}}

        class RuntimeScene:
            route = "Scene/test.scene"

        scene_api = Scene()
        proxy = corona_engine_scratch._NativeEditorActorProxy(
            RuntimeScene(), {"name": "Box", "geometry": {}}
        )

        with mock.patch.object(
                 editor_api,
                 "get_script_runtime_editor_api",
                 return_value=SimpleNamespace(scene=scene_api),
             ), \
             mock.patch.object(corona_engine_scratch, "assert_engine_operation_allowed"), \
             mock.patch.object(proxy, "_pace_runtime_transform"):
            self.assertTrue(proxy.set_position([1.0, 2.0, 3.0]))

        self.assertEqual(
            scene_api.calls,
            [("Scene/test.scene", "Box", {"position": [1.0, 2.0, 3.0], "persist": False})],
        )

    def test_runtime_camera_update_uses_viewport_contract_and_does_not_call_legacy_binding(self):
        class Viewport:
            def __init__(self):
                self.calls = []

            def set_camera_pose(self, *args):
                self.calls.append(args)
                return {"status": "success", "camera": {"name": "MainCamera"}}

        viewport = Viewport()
        with mock.patch.object(
                 editor_api,
                 "get_script_runtime_editor_api",
                 return_value=SimpleNamespace(viewport=viewport),
             ):
            result = corona_engine_scratch._set_native_runtime_camera(
                "Scene/test.scene",
                [1.0, 2.0, 3.0],
                [0.0, 0.0, 1.0],
                [0.0, 1.0, 0.0],
                45.0,
                "MainCamera",
            )

        self.assertTrue(result)
        self.assertEqual(
            viewport.calls,
            [(
                "Scene/test.scene",
                "MainCamera",
                {
                    "position": [1.0, 2.0, 3.0],
                    "forward": [0.0, 0.0, 1.0],
                    "world_up": [0.0, 1.0, 0.0],
                    "fov": 45.0,
                    "persist": False,
                },
            )],
        )

    def test_actor_physics_uses_script_runtime_scene_tools_contract(self):
        class SceneTools:
            def __init__(self):
                self.calls = []

            def set_actor_physics(self, *args):
                self.calls.append(args)
                return {"status": "success", "actor": {"name": "Box"}}

        class RuntimeScene:
            route = "Scene/test.scene"

        scene_tools = SceneTools()
        proxy = corona_engine_scratch._NativeEditorActorProxy(
            RuntimeScene(), {"name": "Box", "geometry": {}}
        )
        with mock.patch.object(
                 editor_api,
                 "get_script_runtime_editor_api",
                 return_value=SimpleNamespace(scene_tools=scene_tools),
             ), \
             mock.patch.object(corona_engine_scratch, "assert_engine_operation_allowed"):
            self.assertTrue(proxy.set_physics_enabled(True))

        self.assertEqual(
            scene_tools.calls,
            [("Scene/test.scene", "Box", {"physics_enabled": True})],
        )


if __name__ == "__main__":
    unittest.main()

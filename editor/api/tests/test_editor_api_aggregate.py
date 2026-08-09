import unittest
import sys
import types
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from api.editor_api import (
    CoronaEditorApi,
    get_lan_chat_adapter,
    get_lan_chat_queue_adapter,
    get_lan_chat_transport_adapter,
    get_network_adapter,
    get_scene_adapter,
    get_scene_tools_adapter,
    get_script_runtime_editor_api,
    get_script_runtime_adapter,
    set_compat_active_project_path,
    get_viewport_adapter,
)


class EditorAggregateApiTest(unittest.TestCase):
    def test_script_runtime_manifest_lookup_uses_script_channel(self):
        import api.editor_api as editor_api_module

        with patch.object(editor_api_module, "_SCRIPT_RUNTIME_API_METHODS", None), patch(
            "api.editor_api._invoke_script_cpp_api",
            side_effect=[
                {
                    "methods": [
                        {
                            "api": "scene.get_snapshot",
                            "python_wrapper": "scene.get_snapshot",
                        }
                    ]
                },
                {"status": "success", "scene": "level.scene"},
            ],
        ) as invoke:
            result = editor_api_module._invoke_script_manifest_cpp_api(
                "scene.get_snapshot", ["level.scene"]
            )

        self.assertEqual(result["scene"], "level.scene")
        self.assertEqual(invoke.call_args_list[0].args, ("EditorApi.list_methods", []))
        self.assertEqual(invoke.call_args_list[0].kwargs, {"validate_method": False})
        self.assertEqual(invoke.call_args_list[1].args, ("scene.get_snapshot", ["level.scene"]))

    def test_script_runtime_editor_adapter_uses_script_channel_manifest_calls(self):
        with patch(
            "api.editor_api._invoke_script_manifest_cpp_api",
            side_effect=[
                {"status": "success", "scene": "level.scene"},
                {"status": "success", "actor": {"name": "Box"}},
                {"status": "success", "actor": {"name": "Box"}},
                {"status": "success"},
                {"status": "success", "camera": {"name": "MainCamera"}},
            ],
        ) as invoke:
            adapter = get_script_runtime_editor_api()
            snapshot = adapter.scene.get_snapshot("level.scene")
            transformed = adapter.scene.set_actor_transform(
                "level.scene", "Box", {"position": [1, 2, 3]}
            )
            created = adapter.scene_tools.create_actor(
                "level.scene", "box.glb", "model", {"actor_name": "Box"}
            )
            removed = adapter.scene_tools.remove_actor("level.scene", "Box")
            camera = adapter.viewport.set_camera_pose(
                "level.scene", "MainCamera", {"position": [0, 1, 2]}
            )

        self.assertEqual(snapshot["scene"], "level.scene")
        self.assertEqual(transformed["actor"]["name"], "Box")
        self.assertEqual(created["actor"]["name"], "Box")
        self.assertEqual(removed["status"], "success")
        self.assertEqual(camera["camera"]["name"], "MainCamera")
        self.assertEqual(
            [call.args for call in invoke.call_args_list],
            [
                ("scene.get_snapshot", ["level.scene"]),
                (
                    "scene.set_actor_transform",
                    ["level.scene", "Box", {"position": [1, 2, 3]}],
                ),
                (
                    "scene_tools.create_actor",
                    ["level.scene", "box.glb", "model", {"actor_name": "Box"}],
                ),
                ("scene_tools.remove_actor", ["level.scene", "Box"]),
                (
                    "viewport.set_camera_pose",
                    ["level.scene", "MainCamera", {"position": [0, 1, 2]}],
                ),
            ],
        )

    def test_resource_search_adapter_uses_manifest_wrapper(self):
        with patch(
            "api.editor_api._find_cpp_api_method_by_python_wrapper",
            return_value={"api": "ResourceSearch.fuzzy_search"},
        ), patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"status": "success", "items": []},
        ) as invoke:
            result = CoronaEditorApi.resource_search.fuzzy_search(
                "chair", 10, None, "frontend"
            )

        self.assertEqual(result["items"], [])
        invoke.assert_called_once_with(
            "resource_search.fuzzy_search", ["chair", 10, None, "frontend"]
        )

    def test_files_adapter_uses_manifest_wrapper(self):
        with patch(
            "api.editor_api._find_cpp_api_method_by_python_wrapper",
            return_value={"api": "FileManager.get_project_info"},
        ), patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"status": "success", "path": "D:/Project"},
        ) as invoke:
            result = CoronaEditorApi.files.get_project_info()

        self.assertEqual(result["path"], "D:/Project")
        invoke.assert_called_once_with("files.get_project_info", [])

    def test_legacy_scene_datas_adapter_uses_script_runtime_channel(self):
        import api.editor_api as editor_api_module

        native_module = types.ModuleType("CoronaEngine")
        native_module._invoke_cpp_script_api = lambda *_args: (
            '{"success": true, "data": {"scene": "level.scene"}}'
        )
        with patch.dict(sys.modules, {"CoronaEngine": native_module}), patch.object(
            editor_api_module,
            "_SCRIPT_RUNTIME_API_METHODS",
            {
                "scene_datas.get_scene": {
                    "api": "scene_datas.get_scene",
                    "python_wrapper": "scene_datas.get_scene",
                    "params": [{"name": "scene_name", "type": "string"}],
                    "return": "object",
                    "allowed_callers": 4,
                }
            },
        ):
            result = CoronaEditorApi.scene_datas.get_scene("level.scene")

        self.assertEqual(result["scene"], "level.scene")

    def test_editor_python_channel_cannot_call_script_only_scene_datas(self):
        from api.editor_api import _invoke_cpp_api

        with patch(
            "api.editor_api._ensure_cpp_api_method",
            return_value={
                "params": [{"name": "scene_name", "type": "string"}],
                "return": "object",
                "allowed_callers": 4,
            },
        ):
            with self.assertRaisesRegex(RuntimeError, "PythonScript cannot call"):
                _invoke_cpp_api("scene_datas.get_scene", ["level.scene"])

    def test_scene_adapter_wraps_legacy_snapshot_transform_and_bounds_bindings(self):
        class LegacyEngine:
            def get_editor_scene_snapshot(self, scene_name):
                return {"scene": scene_name, "actors": []}

            def set_editor_actor_transform(self, scene_name, actor_name, payload):
                return {"status": "success", "scene": scene_name, "actor": actor_name, "payload": payload}

            def get_editor_actor_bounds(self, scene_name, actor_name):
                return {"status": "success", "aabb": {"min": [0, 0, 0], "max": [1, 1, 1]}}

        adapter = get_scene_adapter(LegacyEngine())
        self.assertIsNotNone(adapter)
        self.assertEqual(adapter.get_snapshot("level.scene")["scene"], "level.scene")
        self.assertEqual(
            adapter.set_actor_transform("level.scene", "Box", {"position": [1, 2, 3]})["status"],
            "success",
        )
        self.assertEqual(adapter.get_actor_bounds("level.scene", "Box")["aabb"]["max"], [1, 1, 1])

    def test_active_project_context_sync_is_hidden_behind_runtime_adapter(self):
        class NativeEngine:
            active_project_path = ""

        engine = NativeEngine()
        self.assertTrue(set_compat_active_project_path("D:/Projects/Example", engine))
        self.assertEqual(engine.active_project_path, "D:/Projects/Example")

    def test_script_runtime_adapter_limits_native_capabilities_to_runtime_methods(self):
        class NativeEngine:
            def get_mouse_delta(self):
                return (1.0, -2.0)

            def set_mouse_locked(self, enabled):
                return bool(enabled)

            def ray_cast(self, origin, direction, distance):
                return (origin, direction, distance)

            def import_media(self, path):
                return path

            def play_audio(self, resource_id, *, loop=False):
                return (resource_id, loop)

            def stop_audio(self, resource_id):
                return resource_id

        adapter = get_script_runtime_adapter(NativeEngine())

        self.assertEqual(adapter.get_mouse_delta(), (1.0, -2.0))
        self.assertTrue(adapter.set_mouse_locked(True))
        self.assertEqual(adapter.ray_cast([0, 0, 0], [0, 0, 1], 4), ([0, 0, 0], [0, 0, 1], 4.0))
        self.assertEqual(adapter.import_media("sound.wav"), "sound.wav")
        self.assertEqual(adapter.play_audio("audio-1", loop=True), ("audio-1", True))
        self.assertEqual(adapter.stop_audio("audio-1"), "audio-1")

    def test_scene_adapter_uses_manifest_for_authoritative_snapshot(self):
        class NativeEngine:
            def _invoke_cpp_editor_api(self, *_args):
                return None

        with patch(
            "api.editor_api._find_cpp_api_method_by_python_wrapper",
            return_value={"api": "scene.get_snapshot"},
        ):
            self.assertIs(get_scene_adapter(NativeEngine()), CoronaEditorApi.scene)

    def test_scene_tools_adapter_uses_manifest_for_actor_creation(self):
        class NativeEngine:
            def _invoke_cpp_editor_api(self, *_args):
                return None

        with patch(
            "api.editor_api._find_cpp_api_method_by_python_wrapper",
            return_value={"api": "SceneTools.create_actor"},
        ), patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"status": "success", "actor": {"actor_guid": "a1"}},
        ) as invoke:
            adapter = get_scene_tools_adapter(NativeEngine())
            result = adapter.create_actor("level.scene", "chair.glb", "model", {"actor_name": "Chair"})

        self.assertEqual(result["actor"]["actor_guid"], "a1")
        invoke.assert_called_once_with(
            "scene_tools.create_actor",
            ["level.scene", "chair.glb", "model", {"actor_name": "Chair"}],
        )

    def test_scene_tools_adapter_falls_back_only_for_legacy_engine(self):
        class LegacyEngine:
            def create_editor_actor(self, scene_name, source_path, actor_type, actor_data):
                return {"status": "success", "scene": scene_name, "actor_type": actor_type}

        adapter = get_scene_tools_adapter(LegacyEngine())

        self.assertEqual(
            adapter.create_actor("level.scene", "chair.glb", "model", {"actor_name": "Chair"})[
                "actor_type"
            ],
            "model",
        )

    def test_viewport_adapter_uses_manifest_for_value_object_capture(self):
        class NativeEngine:
            def _invoke_cpp_editor_api(self, *_args):
                return None

        with patch(
            "api.editor_api._find_cpp_api_method_by_python_wrapper",
            return_value={"api": "viewport.capture"},
        ), patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"status": "success", "path": "shot.png"},
        ) as invoke:
            adapter = get_viewport_adapter(NativeEngine())
            result = adapter.capture(
                "level.scene", "review", {"position": [0, 0, 0]}, "shot.png"
            )

        self.assertEqual(result["path"], "shot.png")
        invoke.assert_called_once_with(
            "viewport.capture",
            ["level.scene", "review", {"position": [0, 0, 0]}, "shot.png"],
        )

    def test_viewport_adapter_resolves_default_embedded_host_inside_adapter(self):
        native_module = types.ModuleType("CoronaEngine")
        native_module._invoke_cpp_editor_api = lambda *_args: None
        with patch.dict(sys.modules, {"CoronaEngine": native_module}), patch(
            "api.editor_api._find_cpp_api_method_by_python_wrapper",
            return_value={"api": "viewport.capture"},
        ):
            self.assertIs(get_viewport_adapter(), CoronaEditorApi.viewport)

    def test_network_adapter_resolves_default_embedded_host_inside_adapter(self):
        native_module = types.ModuleType("CoronaEngine")
        native_module._invoke_cpp_editor_api = lambda *_args: None
        with patch.dict(sys.modules, {"CoronaEngine": native_module}):
            self.assertIs(get_network_adapter(), CoronaEditorApi.network)

    def test_viewport_adapter_uses_manifest_for_camera_pose(self):
        class NativeEngine:
            def _invoke_cpp_editor_api(self, *_args):
                return None

        camera = {"position": [1.0, 2.0, 3.0], "persist": True}
        with patch(
            "api.editor_api._find_cpp_api_method_by_python_wrapper",
            return_value={"api": "viewport.set_camera_pose"},
        ), patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"status": "success", "camera": camera},
        ) as invoke:
            adapter = get_viewport_adapter(NativeEngine())
            result = adapter.set_camera_pose("level.scene", "MainCamera", camera)

        self.assertEqual(result["camera"], camera)
        invoke.assert_called_once_with(
            "viewport.set_camera_pose", ["level.scene", "MainCamera", camera]
        )

    def test_scene_tools_focus_uses_stable_actor_camera_contract(self):
        with patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"status": "success", "distance": 4.0},
        ) as invoke:
            result = CoronaEditorApi.scene_tools.focus_actor(
                "level.scene", "Chair", "MainCamera"
            )

        self.assertEqual(result["distance"], 4.0)
        invoke.assert_called_once_with(
            "scene_tools.focus_actor", ["level.scene", "Chair", "MainCamera"]
        )

    def test_scene_tools_actor_state_uses_stable_state_contract(self):
        with patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"status": "success"},
        ) as invoke:
            result = CoronaEditorApi.scene_tools.set_actor_state(
                "level.scene", "Chair", {"visible": False}
            )

        self.assertEqual(result["status"], "success")
        invoke.assert_called_once_with(
            "scene_tools.set_actor_state",
            ["level.scene", "Chair", {"visible": False}],
        )

    def test_scene_tools_environment_and_physics_use_aggregate_contracts(self):
        with patch(
            "api.editor_api._invoke_manifest_cpp_api",
            side_effect=[
                {"status": "success", "sun": {"enabled": True}},
                {"status": "success", "grid": {"enabled": False}},
                {"status": "success", "physics": {"gravity": [0, -9.8, 0]}},
                {"status": "success", "gravity": [0, -9.8, 0]},
            ],
        ) as invoke:
            sun = CoronaEditorApi.scene_tools.sun_direction(
                "level.scene", True, [1.0, 2.0, 3.0]
            )
            grid = CoronaEditorApi.scene_tools.floor_grid("level.scene", False)
            physics = CoronaEditorApi.scene_tools.set_physics_params(
                "level.scene", [0, -9.8, 0], 0.0, 0.5, 1.0 / 60.0
            )
            current = CoronaEditorApi.scene_tools.get_physics_params("level.scene")

        self.assertTrue(sun["sun"]["enabled"])
        self.assertFalse(grid["grid"]["enabled"])
        self.assertEqual(physics["physics"]["gravity"], [0, -9.8, 0])
        self.assertEqual(current["gravity"], [0, -9.8, 0])
        self.assertEqual(
            [call.args for call in invoke.call_args_list],
            [
                ("scene_tools.sun_direction", ["level.scene", True, [1.0, 2.0, 3.0]]),
                ("scene_tools.floor_grid", ["level.scene", False]),
                (
                    "scene_tools.set_physics_params",
                    ["level.scene", [0, -9.8, 0], 0.0, 0.5, 1.0 / 60.0],
                ),
                ("scene_tools.get_physics_params", ["level.scene"]),
            ],
        )

    def test_scene_tools_camera_debug_uses_aggregate_contracts(self):
        with patch(
            "api.editor_api._invoke_manifest_cpp_api",
            side_effect=[
                {"status": "success", "mode": "vision"},
                {"status": "success", "mode": "vision"},
                {"status": "success", "mode": "path_tracing"},
                {"status": "success", "mode": "path_tracing"},
                {"status": "success", "enabled": True},
                {"status": "success", "enabled": True},
                {"status": "success", "enabled": False},
                {"status": "success", "enabled": False},
                {"status": "success", "mode": "final_color"},
                {"status": "success", "mode": "final_color"},
            ],
        ) as invoke:
            results = [
                CoronaEditorApi.scene_tools.set_render_backend(
                    "vision", "level.scene", "MainCamera"
                ),
                CoronaEditorApi.scene_tools.get_render_backend(
                    "level.scene", "MainCamera"
                ),
                CoronaEditorApi.scene_tools.set_vision_render_mode(
                    "level.scene", "MainCamera", "path_tracing"
                ),
                CoronaEditorApi.scene_tools.get_vision_render_mode(
                    "level.scene", "MainCamera"
                ),
                CoronaEditorApi.scene_tools.set_shadow_cascade_debug(
                    "level.scene", "MainCamera", True
                ),
                CoronaEditorApi.scene_tools.get_shadow_cascade_debug(
                    "level.scene", "MainCamera"
                ),
                CoronaEditorApi.scene_tools.set_ssao_enabled(
                    "level.scene", "MainCamera", False
                ),
                CoronaEditorApi.scene_tools.get_ssao_enabled(
                    "level.scene", "MainCamera"
                ),
                CoronaEditorApi.scene_tools.set_output_mode(
                    "level.scene", "MainCamera", "final_color"
                ),
                CoronaEditorApi.scene_tools.get_output_mode(
                    "level.scene", "MainCamera"
                ),
            ]

        self.assertEqual(results[0]["mode"], "vision")
        self.assertTrue(results[4]["enabled"])
        self.assertFalse(results[7]["enabled"])
        self.assertEqual(
            [call.args for call in invoke.call_args_list],
            [
                ("scene_tools.set_render_backend", ["vision", "level.scene", "MainCamera"]),
                ("scene_tools.get_render_backend", ["level.scene", "MainCamera"]),
                (
                    "scene_tools.set_vision_render_mode",
                    ["level.scene", "MainCamera", "path_tracing"],
                ),
                ("scene_tools.get_vision_render_mode", ["level.scene", "MainCamera"]),
                (
                    "scene_tools.set_shadow_cascade_debug",
                    ["level.scene", "MainCamera", True],
                ),
                ("scene_tools.get_shadow_cascade_debug", ["level.scene", "MainCamera"]),
                ("scene_tools.set_ssao_enabled", ["level.scene", "MainCamera", False]),
                ("scene_tools.get_ssao_enabled", ["level.scene", "MainCamera"]),
                (
                    "scene_tools.set_output_mode",
                    ["level.scene", "MainCamera", "final_color"],
                ),
                ("scene_tools.get_output_mode", ["level.scene", "MainCamera"]),
            ],
        )

    def test_scene_tools_camera_lifecycle_uses_aggregate_contracts(self):
        state = {
            "view_open": True,
            "view_x": 10,
            "view_y": 20,
            "view_width": 640,
            "view_height": 360,
        }
        with patch(
            "api.editor_api._invoke_manifest_cpp_api",
            side_effect=[
                {"status": "success", "camera": {"name": "Review"}},
                {"status": "success", "camera": {"name": "Review"}},
                {"status": "success", "camera": {"name": "Review"}},
                {"status": "success", "cameras": []},
                {"status": "success", "camera": {"name": "Review"}},
                {"status": "success", "camera_id": "review"},
                {"status": "success", "camera": {"name": "Review"}},
            ],
        ) as invoke:
            results = [
                CoronaEditorApi.scene_tools.create_camera_view("level.scene", "Review"),
                CoronaEditorApi.scene_tools.open_camera_view("level.scene", "Review"),
                CoronaEditorApi.scene_tools.close_camera_view("level.scene", "Review"),
                CoronaEditorApi.scene_tools.list_camera_views("level.scene"),
                CoronaEditorApi.scene_tools.update_camera_view("level.scene", "Review", state),
                CoronaEditorApi.scene_tools.delete_camera("level.scene", "Review"),
                CoronaEditorApi.scene_tools.rename_camera_view(
                    "level.scene", "Review", "Review"
                ),
            ]

        self.assertEqual(results[0]["camera"]["name"], "Review")
        self.assertEqual(results[3]["cameras"], [])
        self.assertEqual(results[5]["camera_id"], "review")
        self.assertEqual(
            [call.args for call in invoke.call_args_list],
            [
                ("scene_tools.create_camera_view", ["level.scene", "Review"]),
                ("scene_tools.open_camera_view", ["level.scene", "Review"]),
                ("scene_tools.close_camera_view", ["level.scene", "Review"]),
                ("scene_tools.list_camera_views", ["level.scene"]),
                ("scene_tools.update_camera_view", ["level.scene", "Review", state]),
                ("scene_tools.delete_camera", ["level.scene", "Review"]),
                ("scene_tools.rename_camera_view", ["level.scene", "Review", "Review"]),
            ],
        )

    def test_scene_tools_vision_bridge_uses_aggregate_contracts(self):
        with patch(
            "api.editor_api._invoke_manifest_cpp_api",
            side_effect=[
                {"status": "success", "available": True},
                {"status": "success"},
            ],
        ) as invoke:
            available = CoronaEditorApi.scene_tools.is_vision_available()
            loaded = CoronaEditorApi.scene_tools.load_vision_scene("vision.json")

        self.assertTrue(available["available"])
        self.assertEqual(loaded["status"], "success")
        self.assertEqual(
            [call.args for call in invoke.call_args_list],
            [
                ("scene_tools.is_vision_available", []),
                ("scene_tools.load_vision_scene", ["vision.json"]),
            ],
        )

    def test_scene_tools_screenshot_uses_aggregate_contract(self):
        with patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"status": "success", "path": "review.png"},
        ) as invoke:
            result = CoronaEditorApi.scene_tools.save_screenshot(
                "level.scene", "review.png", "MainCamera"
            )

        self.assertEqual(result["path"], "review.png")
        invoke.assert_called_once_with(
            "scene_tools.save_screenshot",
            ["level.scene", "review.png", "MainCamera"],
        )

    def test_scene_tools_actor_persistence_and_resource_selection_use_aggregate_contracts(self):
        with patch(
            "api.editor_api._invoke_manifest_cpp_api",
            side_effect=[{"status": "success"}, "/assets/chair.fbx"],
        ) as invoke:
            saved = CoronaEditorApi.scene_tools.save_actor("level.scene", "Chair")
            selected = CoronaEditorApi.scene_tools.select_model_file(
                "level.scene", "Chair", "model"
            )

        self.assertEqual(saved["status"], "success")
        self.assertEqual(selected, "/assets/chair.fbx")
        self.assertEqual(
            invoke.call_args_list[0].args,
            ("scene_tools.save_actor", ["level.scene", "Chair"]),
        )
        self.assertEqual(
            invoke.call_args_list[1].args,
            ("scene_tools.select_model_file", ["level.scene", "Chair", "model"]),
        )

    def test_viewport_adapter_preserves_legacy_capture_binding(self):
        class LegacyEngine:
            def capture_editor_camera_view(self, scene, camera, payload, path):
                return {
                    "status": "success",
                    "scene": scene,
                    "camera": camera,
                    "payload": payload,
                    "path": path,
                }

        result = get_viewport_adapter(LegacyEngine()).capture(
            "level.scene", "review", {"position": [0, 0, 0]}, "shot.png"
        )

        self.assertEqual(result["status"], "success")
        self.assertEqual(result["path"], "shot.png")

    def test_viewport_set_camera_pose_uses_value_object_contract(self):
        camera = {
            "position": [1.0, 2.0, 3.0],
            "forward": [0.0, 0.0, 1.0],
            "world_up": [0.0, 1.0, 0.0],
            "fov": 45.0,
        }
        with patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"status": "success", "camera": camera},
        ) as invoke:
            result = CoronaEditorApi.viewport.set_camera_pose(
                "level.scene", "MainCamera", camera
            )

        self.assertEqual(result["camera"], camera)
        invoke.assert_called_once_with(
            "viewport.set_camera_pose", ["level.scene", "MainCamera", camera]
        )

    def test_scene_snapshot_uses_stable_scene_contract(self):
        with patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"status": "success", "scene": "level.scene", "actors": []},
        ) as invoke:
            result = CoronaEditorApi.scene.get_snapshot("level.scene")

        self.assertEqual(result["scene"], "level.scene")
        invoke.assert_called_once_with("scene.get_snapshot", ["level.scene"])

    def test_actor_transform_uses_value_object_contract(self):
        transform = {"geometry": {"position": [1.0, 2.0, 3.0]}}
        with patch(
            "api.editor_api._find_cpp_api_method_by_python_wrapper",
            return_value={"api": "scene_tools.set_actor_physics"},
        ), patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"status": "success", "actor": {"name": "Box"}},
        ) as invoke:
            result = CoronaEditorApi.scene.set_actor_transform("level.scene", "Box", transform)

        self.assertEqual(result["actor"]["name"], "Box")
        invoke.assert_called_once_with(
            "scene.set_actor_transform", ["level.scene", "Box", transform]
        )

    def test_actor_physics_uses_scene_tools_value_object_contract(self):
        physics = {
            "physics_enabled": True,
            "damping": 0.9,
            "restitution": 0.1,
        }
        with patch(
            "api.editor_api._find_cpp_api_method_by_python_wrapper",
            return_value={"api": "scene_tools.set_actor_physics"},
        ), patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"status": "success", "actor": {"name": "Box"}},
        ) as invoke:
            result = CoronaEditorApi.scene_tools.set_actor_physics(
                "level.scene", "Box", physics
            )

        self.assertEqual(result["actor"]["name"], "Box")
        invoke.assert_called_once_with(
            "scene_tools.set_actor_physics", ["level.scene", "Box", physics]
        )

    def test_camera_lock_uses_scene_tools_value_object_contract(self):
        camera_lock = {"enabled": True, "position_offset": [0.0, 0.0, 2.0]}
        with patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"status": "success", "actor": {"name": "Box"}},
        ) as invoke:
            result = CoronaEditorApi.scene_tools.set_actor_camera_lock(
                "level.scene", "Box", camera_lock
            )

        self.assertEqual(result["actor"]["name"], "Box")
        invoke.assert_called_once_with(
            "scene_tools.set_actor_camera_lock",
            ["level.scene", "Box", camera_lock],
        )

    def test_viewport_capture_uses_value_object_contract(self):
        camera = {"position": [0.0, 1.0, 2.0], "forward": [0.0, 0.0, -1.0]}
        with patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"status": "success", "path": "review.png"},
        ) as invoke:
            result = CoronaEditorApi.viewport.capture(
                "level.scene", "vlm_review_camera", camera, "review.png"
            )

        self.assertEqual(result["path"], "review.png")
        invoke.assert_called_once_with(
            "viewport.capture",
            ["level.scene", "vlm_review_camera", camera, "review.png"],
        )

    def test_network_contract_uses_manifest_for_collaboration_intent(self):
        with patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"ok": True},
        ) as invoke:
            result = CoronaEditorApi.network.broadcast_intent(
                "alice", "moving", [1.0, 2.0, 3.0], "placing"
            )

        self.assertEqual(result, {"ok": True})
        invoke.assert_called_once_with(
            "network.broadcast_intent",
            ["alice", "moving", [1.0, 2.0, 3.0], "placing"],
        )

    def test_network_session_info_uses_manifest_for_role_queries(self):
        with patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"role": "client", "active": True},
        ) as invoke:
            result = CoronaEditorApi.network.get_session_info()

        self.assertEqual(result["role"], "client")
        invoke.assert_called_once_with("network.get_session_info", [])

    def test_lan_chat_contract_preserves_object_payload_boundary(self):
        payload = {
            "text": "hello",
            "message_kind": "chat",
            "source_user_id": "alice",
        }
        with patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"ok": True, "message_id": "m1"},
        ) as invoke:
            result = CoronaEditorApi.lan_chat.send_message(payload)

        self.assertEqual(result["message_id"], "m1")
        invoke.assert_called_once_with("lan_chat.send_message", [payload])

    def test_lan_chat_reliable_transport_uses_object_payload_boundary(self):
        payload = {
            "agent_id": "agent-1",
            "agent_name": "Builder",
            "text": "done",
            "message_kind": "progress",
        }
        with patch(
            "api.editor_api._invoke_manifest_cpp_api",
            return_value={"ok": True, "message_id": "m2", "seq": 4},
        ) as invoke:
            result = CoronaEditorApi.lan_chat.send_agent_reply(payload)

        self.assertEqual(result["seq"], 4)
        invoke.assert_called_once_with("lan_chat.send_agent_reply", [payload])

    def test_lan_chat_queue_adapter_preserves_empty_poll_result(self):
        class LegacyEngine:
            def network_pop_lanchat_agent_trigger(self):
                return None

        adapter = get_lan_chat_queue_adapter(LegacyEngine())

        self.assertIsNone(adapter.poll_agent_trigger())

    def test_legacy_network_injection_is_adapted_without_changing_business_surface(self):
        class LegacyEngine:
            def network_broadcast_intent(self, *args):
                return args == ("alice", "moving", [0.0, 0.0, 0.0], "placing")

            def network_session_role_name(self):
                return "client"

        adapter = get_network_adapter(LegacyEngine())

        self.assertTrue(adapter.broadcast_intent(
            "alice", "moving", [0.0, 0.0, 0.0], "placing"
        ))
        self.assertEqual(adapter.get_session_info()["role"], "client")

    def test_lan_chat_roster_uses_aggregate_adapter(self):
        class LegacyEngine:
            def network_lanchat_agents_snapshot(self):
                return [{"agent_id": "agent-1", "name": "Builder"}]

        adapter = get_lan_chat_adapter(LegacyEngine())

        self.assertEqual(adapter.list_agents()["agents"][0]["agent_id"], "agent-1")

    def test_legacy_lan_chat_transport_preserves_specialized_reply_arguments(self):
        class LegacyEngine:
            def __init__(self):
                self.calls = []

            def network_send_agent_reply_ex(self, *args):
                self.calls.append(args)
                return True

        engine = LegacyEngine()
        adapter = get_lan_chat_transport_adapter(engine)

        self.assertTrue(adapter.send_agent_reply(
            "agent-1", "Builder", "done", "progress", "agent-2", "corr-1", "{}"
        ))
        self.assertEqual(
            engine.calls,
            [("agent-1", "Builder", "done", "progress", "agent-2", "corr-1", "{}")],
        )


if __name__ == "__main__":
    unittest.main()

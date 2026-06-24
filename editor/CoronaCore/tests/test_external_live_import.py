import json
import math
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from plugins.SceneTools import main as scene_tools_module
from plugins.MainView import main as main_view_module
from CoronaCore.core.entities.actor import Actor as CoreActor
from CoronaCore.core.entities.scene import Scene


def _find_external_cbox_lf_scene() -> Path | None:
    relative = Path("test_vision") / "render_scene" / "cbox-lf" / "vision_scene.json"
    fixed = Path("D:/Documents/GitHub/CoronaExample") / relative
    if fixed.exists():
        return fixed

    cursor = Path.cwd()
    while True:
        candidate = cursor.parent / "CoronaExample" / relative
        if candidate.exists():
            return candidate
        if cursor.parent == cursor:
            return None
        cursor = cursor.parent


class FakeCamera:
    def __init__(self):
        self.render_backend = "native"
        self.vision_render_mode = "path_tracing"
        self.call_order = []
        self.name = "Camera"
        self.camera_id = "camera-1"
        self.position = [0.0, 0.0, 0.0]
        self.forward = [0.0, 0.0, 1.0]
        self.world_up = [0.0, 1.0, 0.0]
        self.fov = 45.0

    def set(self, position, forward, world_up, fov):
        self.position = list(position)
        self.forward = list(forward)
        self.world_up = list(world_up)
        self.fov = float(fov)

    def set_render_backend(self, mode):
        self.call_order.append(f"set_render_backend:{mode}")
        self.render_backend = mode

    def set_vision_render_mode(self, mode):
        self.call_order.append(f"set_vision_render_mode:{mode}")
        self.vision_render_mode = mode

    def get_vision_render_mode(self):
        return self.vision_render_mode

    def to_dict(self):
        return {
            "name": self.name,
            "position": self.position,
            "forward": self.forward,
            "world_up": self.world_up,
            "fov": self.fov,
            "render_backend": self.render_backend,
            "vision_render_mode": self.vision_render_mode,
        }


class FakeEngineScene:
    def set_active_camera(self, camera):
        self.active_camera = camera


class FakeScene:
    def __init__(self):
        self.route = "Scene/main.scene"
        self.file_data = {}
        self.engine_scene = FakeEngineScene()
        self._actors = []
        self._camera = FakeCamera()
        self.vision_source_path = ""
        self.vision_import_mode = ""
        self.vision_bindings = []
        self.vision_unsupported_shapes = []
        self.saved = False
        self.save_snapshots = []
        self.tree_notified = False

    def ensure_default_camera(self):
        return None

    def get_active_camera(self):
        return self._camera

    def get_cameras(self):
        return [self._camera]

    def get_actors(self):
        return self._actors

    def set_camera(self, position, forward, up, fov, camera_name=None):
        self._camera.set(position, forward, up, fov)
        return True

    def add_actor(self, actor):
        existing_names = {item.name for item in self._actors}
        base_name = actor.name
        suffix = 1
        while actor.name in existing_names:
            actor.name = f"{base_name}_{suffix}"
            suffix += 1
        self._actors.append(actor)

    def save_data(self):
        self.saved = True
        self.save_snapshots.append({
            "render_backend": self._camera.render_backend,
            "vision_render_mode": self._camera.vision_render_mode,
        })

    def _notify_scene_tree_changed(self):
        self.tree_notified = True

    def remove_actor(self, actor):
        if actor in self._actors:
            self._actors.remove(actor)
            return True
        return False


class FakeEngineActor:
    def __init__(self, handle=4242):
        self.actor_guid = ""
        self.external_vision_binding = {}
        self.handle = handle

    def set_actor_guid(self, actor_guid):
        self.actor_guid = actor_guid

    def get_handle(self):
        return self.handle

    def set_external_vision_binding(self, source_path, shape_guid, shape_index,
                                    json_path, shape_type, shape_identity_key,
                                    model_path):
        self.external_vision_binding = {
            "source_path": source_path,
            "shape_guid": shape_guid,
            "shape_index": int(shape_index),
            "json_path": json_path,
            "shape_type": shape_type,
            "shape_identity_key": shape_identity_key,
            "model_path": model_path,
        }

    def clear_external_vision_binding(self):
        self.external_vision_binding = {}


class FakeActor:
    def __init__(self, name="", route=None, source_index=0, actor_type="actor",
                 parent_scene=None, actor_data=None):
        self.name = name
        self.route = route
        self.actor_type = actor_type
        self.parent_scene = parent_scene
        self.actor_guid = actor_data.get("actor_guid", "") if actor_data else ""
        geometry = actor_data.get("geometry", {}) if actor_data else {}
        self.position = geometry.get("position", [0.0, 0.0, 0.0])
        self.rotation = geometry.get("rotation", [0.0, 0.0, 0.0])
        self.scale = geometry.get("scale", [1.0, 1.0, 1.0])
        self.physics_enabled = True
        self.handle = actor_data.get("handle", 4242) if actor_data else 4242
        self.engine_obj = FakeEngineActor(self.handle)
        self.engine_obj.set_actor_guid(self.actor_guid)

    def set_position(self, position, if_init=False):
        self.position = position

    def set_rotation(self, rotation, if_init=False):
        self.rotation = rotation

    def set_scale(self, scale, if_init=False):
        self.scale = scale

    def set_physics_enabled(self, enabled):
        self.physics_enabled = bool(enabled)

    def get_visible(self):
        return True

    def set_external_vision_binding(self, binding):
        self.external_vision_binding = dict(binding)
        self.engine_obj.set_external_vision_binding(
            binding.get("source_path", ""),
            binding.get("shape_guid", ""),
            binding.get("shape_index", -1),
            binding.get("json_path", ""),
            binding.get("shape_type", ""),
            binding.get("shape_identity_key", ""),
            binding.get("model_path", ""),
        )

    def clear_external_vision_binding(self):
        self.external_vision_binding = {}
        self.engine_obj.clear_external_vision_binding()


class ExternalLiveImportTests(unittest.TestCase):
    def test_vision_cube_primitive_uses_vision_dimension_fallbacks(self):
        vertices, faces = scene_tools_module._vision_primitive_vertices({
            "param": {"x": 2, "y": 0, "z": 0},
        }, "cube")

        self.assertEqual(len(vertices), 8)
        self.assertEqual(len(faces), 6)
        self.assertEqual(max(abs(vertex[1]) for vertex in vertices), 1.0)
        self.assertEqual(max(abs(vertex[2]) for vertex in vertices), 1.0)

    def test_vision_sphere_primitive_matches_vision_subdivision_counts(self):
        vertices, faces = scene_tools_module._vision_primitive_vertices({
            "param": {"radius": 2, "sub_div": 3},
        }, "sphere")

        self.assertEqual(len(vertices), 14)
        self.assertEqual(len(faces), 24)
        self.assertEqual(vertices[0], [0.0, 2.0, 0.0])
        self.assertEqual(vertices[-1], [0.0, -2.0, 0.0])

    def test_vision_euler_primitive_transform_matches_corona_rotation_order(self):
        shape = {
            "param": {
                "transform": {
                    "type": "euler",
                    "param": {
                        "position": [10, 20, 30],
                        "pitch": math.pi * 0.5,
                        "yaw": math.pi * 0.5,
                        "roll": math.pi * 0.5,
                    },
                },
            },
        }

        transform = scene_tools_module._extract_vision_shape_transform(shape)
        self.assertEqual(transform["position"], [10.0, 20.0, -30.0])
        self.assertEqual(transform["rotation"], [math.pi * 0.5, -math.pi * 0.5, -math.pi * 0.5])

        [vertex] = scene_tools_module._vision_primitive_world_vertices(shape, [[1, 2, 3]])
        self.assertAlmostEqual(vertex[0], 13.0)
        self.assertAlmostEqual(vertex[1], 22.0)
        self.assertAlmostEqual(vertex[2], -29.0)

    def test_derived_external_live_scene_write_does_not_truncate_existing_file_on_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            vision_scene = root / "scene.json"
            vision_scene.write_text(json.dumps({
                "scene": {
                    "shapes": [{
                        "type": "model",
                        "name": "Chair",
                        "guid": "shape-chair",
                        "param": {"fn": "chair.obj"},
                    }],
                },
            }), encoding="utf-8")

            actor = FakeActor(name="Chair", route=str(root / "chair.obj"),
                              actor_type="model", actor_data={
                                  "actor_guid": "actor-chair",
                                  "geometry": {
                                      "position": [1.0, 2.0, 3.0],
                                      "rotation": [0.0, 0.0, 0.0],
                                      "scale": [1.0, 1.0, 1.0],
                                  },
                              })
            scene = SimpleNamespace(
                route="Scene/main.scene",
                name="main",
                vision_bindings=[{
                    "actor_guid": "actor-chair",
                    "shape_guid": "shape-chair",
                    "shape_index": "0",
                    "json_path": "/scene/shapes/0",
                    "shape_type": "model",
                }],
                get_actors=lambda: [actor],
            )
            derived_path = Path(scene_tools_module._derived_vision_scene_path(
                str(vision_scene), scene))
            derived_path.write_text("{\"previous\": true}\n", encoding="utf-8")

            with patch.object(scene_tools_module.json, "dump",
                              side_effect=RuntimeError("simulated write failure")):
                with self.assertRaises(RuntimeError):
                    scene_tools_module._write_derived_external_live_scene(
                        scene, str(vision_scene))

            self.assertEqual(derived_path.read_text(encoding="utf-8"),
                             "{\"previous\": true}\n")
            self.assertEqual(list(root.glob(f".{derived_path.name}.*.tmp")), [])

    def test_import_creates_proxy_actors_and_persists_bindings(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            model_path = root / "chair.obj"
            model_path.write_text(
                "\n".join([
                    "v -1 0 -2",
                    "v 3 4 6",
                    "f 1 2 1",
                    "",
                ]),
                encoding="utf-8",
            )
            vision_scene = root / "scene.json"
            vision_scene.write_text(json.dumps({
                "scene": {
                    "camera": {
                        "param": {
                            "transform": {
                                "param": {
                                    "position": [4, 5, 6],
                                    "target_pos": [4, 5, 5],
                                    "up": [0, 1, 0],
                                },
                            },
                            "fov_y": 1.0,
                        },
                    },
                    "shapes": [
                        {
                            "type": "model",
                            "name": "Chair",
                            "guid": "shape-chair",
                            "param": {
                                "fn": "chair.obj",
                                "transform": {
                                    "type": "trs",
                                    "param": {
                                        "t": [1, 2, 3],
                                        "s": [2, 2, 2],
                                    },
                                },
                            },
                        },
                        {
                            "type": "quad",
                            "name": "Floor",
                            "param": {
                                "width": 2,
                                "height": 2,
                                "transform": {
                                    "type": "matrix4x4",
                                    "param": {
                                        "matrix4x4": [
                                            [1, 0, 0, 0],
                                            [0, 1, 0, 0],
                                            [0, 0, 1, 0],
                                            [0, 0, 1, 1],
                                        ],
                                    },
                                },
                            },
                        },
                        {
                            "type": "sphere",
                            "name": "Ball",
                            "param": {
                                "radius": 0.5,
                                "sub_div": 4,
                                "transform": {
                                    "type": "trs",
                                    "param": {
                                        "t": [0, 0, 2],
                                        "s": [2, 1, 1],
                                    },
                                },
                            },
                        },
                        {"type": "cylinder", "name": "NotYetSupported"},
                    ],
                },
                "render": {
                    "integrator": {
                        "type": "pt",
                        "param": {
                            "denoiser": {
                                "type": "SSAT",
                                "param": {
                                    "spatial_radius": 1,
                                    "angular_samples": 7,
                                },
                            },
                        },
                    },
                },
                "pipeline": {
                    "type": "fixed",
                    "param": {
                        "frame_buffer": {
                            "type": "lightfield",
                            "param": {
                                "accumulation": True,
                            },
                        },
                    },
                },
                "output": {
                    "denoise": True,
                },
            }), encoding="utf-8")

            scene = FakeScene()
            loaded_paths = []
            call_order = scene._camera.call_order
            fake_editor = SimpleNamespace(
                CoronaEngine=SimpleNamespace(
                    active_project_path=str(root),
                    is_vision_available=lambda: True,
                    load_vision_scene=lambda path: (
                        call_order.append("load_vision_scene"),
                        loaded_paths.append(path),
                    ),
                )
            )
            fake_scene_manager = SimpleNamespace(get=lambda scene_name: scene)

            with patch.object(scene_tools_module, "CoronaEditor", fake_editor), \
                 patch.object(scene_tools_module, "scene_manager", fake_scene_manager), \
                 patch.object(scene_tools_module, "Actor", FakeActor):
                result = scene_tools_module.SceneTools.import_vision_scene_into_current_scene(
                    scene.route, str(vision_scene))

            self.assertEqual(result["status"], "success")
            self.assertEqual(result["import_mode"], "external_live")
            self.assertEqual(result["vision_render_mode"], "ssat")
            self.assertTrue(result["camera_imported"])
            self.assertEqual(scene._camera.position, [4.0, 5.0, -6.0])
            self.assertEqual(scene._camera.forward, [0.0, 0.0, 1.0])
            self.assertAlmostEqual(scene._camera.fov, 57.29577951308232)
            self.assertEqual(result["proxy_actors_created"], 3)
            self.assertEqual(result["proxy_actors_reused"], 0)
            self.assertEqual(result["proxy_actors_removed"], 0)
            self.assertEqual(len(scene.get_actors()), 3)
            self.assertEqual(scene.get_actors()[0].name, "Chair")
            self.assertFalse(scene.get_actors()[0].physics_enabled)
            self.assertEqual(scene.get_actors()[0].position, [1.0, 2.0, -3.0])
            self.assertEqual(scene.get_actors()[0].scale, [2.0, 2.0, 2.0])
            self.assertEqual(scene.get_actors()[1].name, "Floor")
            self.assertFalse(scene.get_actors()[1].physics_enabled)
            self.assertEqual(scene.get_actors()[1].position, [0.0, 0.0, -1.0])
            self.assertEqual(scene.get_actors()[1].scale, [1.0, 1.0, 1.0])
            self.assertTrue(scene.get_actors()[1].route.startswith("Resource/vision_proxies/"))
            self.assertTrue((root / scene.get_actors()[1].route).exists())
            proxy_text = (root / scene.get_actors()[1].route).read_text(encoding="utf-8")
            self.assertIn("v 1 0 -1", proxy_text)
            self.assertIn("f 1 2 3", proxy_text)
            self.assertEqual(scene.get_actors()[2].name, "Ball")
            self.assertFalse(scene.get_actors()[2].physics_enabled)
            self.assertEqual(scene.get_actors()[2].position, [0.0, 0.0, -2.0])
            self.assertEqual(scene.get_actors()[2].scale, [2.0, 1.0, 1.0])
            sphere_proxy_text = (root / scene.get_actors()[2].route).read_text(encoding="utf-8")
            self.assertIn("v 0 0.5 0", sphere_proxy_text)
            self.assertIn("f 1 3 2", sphere_proxy_text)
            self.assertEqual(scene.vision_import_mode, "external_live")
            self.assertEqual(scene._camera.vision_render_mode, "ssat")
            self.assertEqual(result["camera"]["vision_render_mode"], "ssat")
            self.assertTrue(scene.save_snapshots)
            self.assertTrue(all(snapshot["vision_render_mode"] == "ssat"
                                for snapshot in scene.save_snapshots))
            self.assertEqual(scene.vision_bindings[0]["actor_guid"],
                             scene.get_actors()[0].actor_guid)
            self.assertEqual(scene.vision_bindings[0]["shape_guid"], "shape-chair")
            self.assertEqual(scene.vision_bindings[0]["json_path"], "/scene/shapes/0")
            self.assertEqual(scene.vision_bindings[0]["native_local_correction_offset"],
                             [1.0, 2.0, -2.0])
            self.assertEqual(scene.vision_bindings[0]["native_local_correction_scale"],
                             8.0)
            self.assertEqual(scene.vision_bindings[1]["shape_type"], "quad")
            self.assertEqual(scene.vision_bindings[2]["shape_type"], "sphere")
            self.assertEqual(scene.get_actors()[0].engine_obj.actor_guid,
                             scene.get_actors()[0].actor_guid)
            self.assertEqual(scene.vision_bindings[0]["source_path"], str(vision_scene.resolve()))
            self.assertEqual(scene.get_actors()[0].engine_obj.external_vision_binding["source_path"],
                             loaded_paths[0])
            self.assertEqual(scene.get_actors()[0].engine_obj.external_vision_binding["shape_guid"],
                             "shape-chair")
            self.assertEqual(scene.get_actors()[1].engine_obj.external_vision_binding["shape_type"],
                             "quad")
            self.assertEqual(scene.vision_unsupported_shapes[0]["type"], "cylinder")
            self.assertEqual(scene.vision_unsupported_shapes[0]["reason"],
                             "unsupported_shape_type")
            self.assertEqual(result["vision"]["binding_count"], 3)
            self.assertEqual(result["vision"]["unsupported_count"], 1)
            self.assertEqual(result["vision"]["unsupported_by_type"], {"cylinder": 1})
            self.assertEqual(len(loaded_paths), 1)
            self.assertEqual(Path(loaded_paths[0]).parent, vision_scene.parent)
            self.assertNotEqual(loaded_paths[0], str(vision_scene.resolve()))
            self.assertEqual(result["runtime_path"], loaded_paths[0])
            derived = json.loads(Path(loaded_paths[0]).read_text(encoding="utf-8"))
            chair_transform = derived["scene"]["shapes"][0]["param"]["transform"]
            self.assertEqual(chair_transform["type"], "matrix4x4")
            self.assertEqual(chair_transform["param"]["matrix4x4"][3], [1.0, 2.0, 3.0, 1.0])
            floor_transform = derived["scene"]["shapes"][1]["param"]["transform"]
            self.assertEqual(floor_transform["type"], "matrix4x4")
            self.assertEqual(floor_transform["param"]["matrix4x4"][3], [0.0, 0.0, 1.0, 1.0])
            self.assertEqual(floor_transform["param"]["matrix4x4"][0][0], 1.0)
            ball_transform = derived["scene"]["shapes"][2]["param"]["transform"]
            self.assertEqual(ball_transform["type"], "matrix4x4")
            self.assertEqual(ball_transform["param"]["matrix4x4"][3], [0.0, 0.0, 2.0, 1.0])
            self.assertEqual(ball_transform["param"]["matrix4x4"][0][0], 2.0)
            self.assertEqual(ball_transform["param"]["matrix4x4"][1][1], 1.0)
            scene.get_actors()[1].set_position([3.0, 4.0, -5.0])
            scene.get_actors()[1].set_scale([1.5, 2.0, 2.5])
            edited_path = scene_tools_module.prepare_external_live_vision_scene(scene)
            edited = json.loads(Path(edited_path).read_text(encoding="utf-8"))
            edited_floor_matrix = edited["scene"]["shapes"][1]["param"]["transform"]["param"]["matrix4x4"]
            self.assertEqual(edited_floor_matrix[0], [1.5, 0.0, 0.0, 0.0])
            self.assertEqual(edited_floor_matrix[1], [0.0, 2.0, 0.0, 0.0])
            self.assertEqual(edited_floor_matrix[2], [0.0, 0.0, 2.5, 0.0])
            self.assertEqual(edited_floor_matrix[3], [3.0, 4.0, 5.0, 1.0])
            self.assertLess(call_order.index("set_vision_render_mode:ssat"),
                            call_order.index("load_vision_scene"))
            self.assertLess(call_order.index("load_vision_scene"),
                            call_order.index("set_render_backend:vision"))
            self.assertTrue(scene.saved)
            self.assertTrue(scene.tree_notified)

    def test_legacy_external_live_actor_can_compute_native_correction_from_obj(self):
        with tempfile.TemporaryDirectory() as tmp:
            model_path = Path(tmp) / "legacy.obj"
            model_path.write_text(
                "\n".join([
                    "v -2 1 -3",
                    "v 4 5 9",
                    "f 1 2 1",
                    "",
                ]),
                encoding="utf-8",
            )

            correction = CoreActor._obj_native_local_correction(str(model_path))

            self.assertEqual(correction[0], [1.0, 3.0, -3.0])
            self.assertEqual(correction[1], 12.0)

    def test_import_real_cbox_lf_scene_infers_ssat_and_preserves_params(self):
        cbox_lf_scene = _find_external_cbox_lf_scene()
        if cbox_lf_scene is None:
            self.skipTest("cbox-lf Vision scene sample not found")

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scene = FakeScene()
            loaded_paths = []
            call_order = scene._camera.call_order
            fake_editor = SimpleNamespace(
                CoronaEngine=SimpleNamespace(
                    active_project_path=str(root),
                    is_vision_available=lambda: True,
                    load_vision_scene=lambda path: (
                        call_order.append("load_vision_scene"),
                        loaded_paths.append(path),
                    ),
                )
            )
            fake_scene_manager = SimpleNamespace(get=lambda scene_name: scene)

            with patch.object(scene_tools_module, "CoronaEditor", fake_editor), \
                 patch.object(scene_tools_module, "scene_manager", fake_scene_manager), \
                 patch.object(scene_tools_module, "Actor", FakeActor):
                result = scene_tools_module.SceneTools.import_vision_scene_into_current_scene(
                    scene.route, str(cbox_lf_scene))

            self.assertEqual(result["status"], "success")
            self.assertEqual(result["vision_render_mode"], "ssat")
            self.assertEqual(scene._camera.vision_render_mode, "ssat")
            self.assertEqual(result["camera"]["vision_render_mode"], "ssat")
            self.assertEqual(scene.vision_source_path, str(cbox_lf_scene.resolve()))
            self.assertTrue(scene.save_snapshots)
            self.assertTrue(all(snapshot["vision_render_mode"] == "ssat"
                                for snapshot in scene.save_snapshots))
            self.assertEqual(len(loaded_paths), 1)
            self.assertLess(call_order.index("set_vision_render_mode:ssat"),
                            call_order.index("load_vision_scene"))
            self.assertLess(call_order.index("load_vision_scene"),
                            call_order.index("set_render_backend:vision"))

            derived = json.loads(Path(loaded_paths[0]).read_text(encoding="utf-8"))
            denoiser = derived["render"]["integrator"]["param"]["denoiser"]
            self.assertEqual(denoiser["type"], "SSAT")
            self.assertEqual(denoiser["param"]["spatial_radius"], 1)
            self.assertEqual(denoiser["param"]["angular_samples"], 7)
            self.assertEqual(derived["pipeline"]["param"]["frame_buffer"]["type"],
                             "lightfield")
            self.assertTrue(derived["output"]["denoise"])

    def test_reimport_same_vision_scene_reuses_proxies_without_duplicates(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "chair.obj").write_text("mesh", encoding="utf-8")
            vision_scene = root / "scene.json"
            vision_scene.write_text(json.dumps({
                "scene": {
                    "shapes": [{
                        "type": "model",
                        "name": "Chair",
                        "guid": "shape-chair",
                        "param": {"fn": "chair.obj"},
                    }],
                },
            }), encoding="utf-8")

            scene = FakeScene()
            fake_editor = SimpleNamespace(
                CoronaEngine=SimpleNamespace(
                    active_project_path=str(root),
                    is_vision_available=lambda: True,
                    load_vision_scene=lambda path: None,
                )
            )
            fake_scene_manager = SimpleNamespace(get=lambda scene_name: scene)

            with patch.object(scene_tools_module, "CoronaEditor", fake_editor), \
                 patch.object(scene_tools_module, "scene_manager", fake_scene_manager), \
                 patch.object(scene_tools_module, "Actor", FakeActor):
                first = scene_tools_module.SceneTools.import_vision_scene_into_current_scene(
                    scene.route, str(vision_scene))
                first_guid = scene.get_actors()[0].actor_guid
                second = scene_tools_module.SceneTools.import_vision_scene_into_current_scene(
                    scene.route, str(vision_scene))

            self.assertEqual(first["proxy_actors_created"], 1)
            self.assertEqual(second["proxy_actors_created"], 0)
            self.assertEqual(second["proxy_actors_reused"], 1)
            self.assertEqual(second["proxy_actors_removed"], 0)
            self.assertEqual(len(scene.get_actors()), 1)
            self.assertEqual(scene.get_actors()[0].actor_guid, first_guid)

    def test_reimport_matches_by_shape_guid_when_order_changes_and_removes_stale_proxy(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "a.obj").write_text("mesh-a", encoding="utf-8")
            (root / "b.obj").write_text("mesh-b", encoding="utf-8")
            vision_scene = root / "scene.json"
            vision_scene.write_text(json.dumps({
                "scene": {
                    "shapes": [
                        {
                            "type": "model",
                            "name": "Part",
                            "guid": "shape-a",
                            "param": {"fn": "a.obj"},
                        },
                        {
                            "type": "model",
                            "name": "Part",
                            "guid": "shape-b",
                            "param": {"fn": "b.obj"},
                        },
                    ],
                },
            }), encoding="utf-8")

            scene = FakeScene()
            fake_editor = SimpleNamespace(
                CoronaEngine=SimpleNamespace(
                    active_project_path=str(root),
                    is_vision_available=lambda: True,
                    load_vision_scene=lambda path: None,
                )
            )
            fake_scene_manager = SimpleNamespace(get=lambda scene_name: scene)

            with patch.object(scene_tools_module, "CoronaEditor", fake_editor), \
                 patch.object(scene_tools_module, "scene_manager", fake_scene_manager), \
                 patch.object(scene_tools_module, "Actor", FakeActor):
                scene_tools_module.SceneTools.import_vision_scene_into_current_scene(
                    scene.route, str(vision_scene))
                guid_by_shape = {
                    binding["shape_guid"]: binding["actor_guid"]
                    for binding in scene.vision_bindings
                }
                actors_by_guid = {
                    actor.actor_guid: actor
                    for actor in scene.get_actors()
                }

                vision_scene.write_text(json.dumps({
                    "scene": {
                        "shapes": [{
                            "type": "model",
                            "name": "Part",
                            "guid": "shape-b",
                            "param": {"fn": "b.obj"},
                        }],
                    },
                }), encoding="utf-8")
                result = scene_tools_module.SceneTools.import_vision_scene_into_current_scene(
                    scene.route, str(vision_scene))

            self.assertEqual(result["proxy_actors_created"], 0)
            self.assertEqual(result["proxy_actors_reused"], 1)
            self.assertEqual(result["proxy_actors_removed"], 1)
            self.assertEqual(len(scene.get_actors()), 1)
            self.assertEqual(scene.vision_bindings[0]["shape_guid"], "shape-b")
            self.assertEqual(scene.vision_bindings[0]["json_path"], "/scene/shapes/0")
            self.assertEqual(scene.vision_bindings[0]["actor_guid"], guid_by_shape["shape-b"])
            stale_actor = actors_by_guid[guid_by_shape["shape-a"]]
            self.assertEqual(stale_actor.engine_obj.external_vision_binding, {})

    def test_list_scene_tree_exposes_external_live_status_and_proxy_metadata(self):
        scene = FakeScene()
        scene.vision_source_path = "D:/vision/scene.json"
        scene.vision_import_mode = "external_live"
        actor = FakeActor(name="Chair", route="D:/vision/chair.obj", actor_type="model",
                          actor_data={"actor_guid": "actor-chair", "handle": 98765})
        scene._actors.append(actor)
        scene.vision_bindings = [{
            "actor_guid": "actor-chair",
            "shape_guid": "shape-chair",
            "json_path": "/scene/shapes/0",
            "shape_type": "model",
            "shape_identity_key": "guid:shape-chair",
        }]
        scene.vision_unsupported_shapes = [{
            "shape_index": 1,
            "json_path": "/scene/shapes/1",
            "type": "cylinder",
            "reason": "unsupported_shape_type",
        }]
        fake_scene_manager = SimpleNamespace(get=lambda scene_name: scene)

        with patch.object(scene_tools_module, "scene_manager", fake_scene_manager):
            result = scene_tools_module.SceneTools.list_scene_tree(scene.route)

        self.assertEqual(result["vision"]["import_mode"], "external_live")
        self.assertEqual(result["vision"]["binding_count"], 1)
        self.assertEqual(result["vision"]["unsupported_count"], 1)
        self.assertEqual(result["vision"]["unsupported_by_reason"], {"unsupported_shape_type": 1})
        self.assertEqual(result["actors"][0]["handle"], 98765)
        self.assertTrue(result["actors"][0]["vision_proxy"])
        self.assertEqual(result["actors"][0]["vision_binding"]["shape_guid"], "shape-chair")

    def test_scene_binding_sync_rehydrates_actor_engine_binding_by_guid(self):
        scene = SimpleNamespace(
            _actors=[
                FakeActor(name="Chair", route="chair.obj", actor_type="model",
                          actor_data={"actor_guid": "actor-chair"}),
                FakeActor(name="Table", route="table.obj", actor_type="model",
                          actor_data={"actor_guid": "actor-table"}),
            ],
            vision_import_mode="external_live",
            vision_bindings=[{
                "actor_guid": "actor-chair",
                "source_path": "D:/vision/scene.json",
                "shape_guid": "shape-chair",
                "shape_index": "3",
                "json_path": "/scene/shapes/3",
                "shape_type": "model",
                "shape_identity_key": "guid:shape-chair",
                "model_path": "D:/vision/chair.obj",
            }],
        )

        Scene._sync_external_vision_bindings_to_actors(scene)

        chair_binding = scene._actors[0].engine_obj.external_vision_binding
        self.assertEqual(chair_binding["source_path"], "D:/vision/scene.json")
        self.assertEqual(chair_binding["shape_guid"], "shape-chair")
        self.assertEqual(chair_binding["shape_index"], 3)
        self.assertEqual(scene._actors[1].engine_obj.external_vision_binding, {})

    def test_main_view_restores_external_live_with_derived_scene_loader(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "chair.obj").write_text("mesh", encoding="utf-8")
            vision_scene = root / "scene.json"
            vision_scene.write_text(json.dumps({
                "scene": {
                    "shapes": [{
                        "type": "model",
                        "name": "Chair",
                        "guid": "shape-chair",
                        "param": {"fn": "chair.obj"},
                    }],
                },
            }), encoding="utf-8")

            actor = FakeActor(name="Chair", route=str(root / "chair.obj"),
                              actor_type="model", actor_data={
                                  "actor_guid": "actor-chair",
                                  "geometry": {
                                      "position": [7.0, 8.0, -9.0],
                                      "rotation": [0.0, 0.0, 0.0],
                                      "scale": [2.0, 3.0, 4.0],
                                  },
                              })
            scene = SimpleNamespace(
                route="Scene/main.scene",
                name="main",
                vision_source_path=str(vision_scene),
                vision_import_mode="external_live",
                vision_bindings=[{
                    "actor_guid": "actor-chair",
                    "source_path": str(vision_scene),
                    "shape_guid": "shape-chair",
                    "shape_index": "0",
                    "json_path": "/scene/shapes/0",
                    "shape_type": "model",
                }],
                get_actors=lambda: [actor],
            )
            loaded_paths = []
            fake_editor = SimpleNamespace(
                CoronaEngine=SimpleNamespace(
                    is_vision_available=lambda: True,
                    load_vision_scene=lambda path: loaded_paths.append(path),
                )
            )

            with patch.object(main_view_module, "CoronaEditor", fake_editor):
                main_view_module.MainView._apply_vision_source_for_scene(scene)

            self.assertEqual(len(loaded_paths), 1)
            loaded_path = Path(loaded_paths[0])
            self.assertEqual(loaded_path.parent, vision_scene.parent)
            self.assertNotEqual(loaded_path, vision_scene)
            self.assertEqual(scene.vision_bindings[0]["source_path"], str(vision_scene))
            self.assertEqual(actor.engine_obj.external_vision_binding["source_path"],
                             str(loaded_path))
            derived = json.loads(loaded_path.read_text(encoding="utf-8"))
            matrix = derived["scene"]["shapes"][0]["param"]["transform"]["param"]["matrix4x4"]
            self.assertEqual(matrix[0], [2.0, 0.0, 0.0, 0.0])
            self.assertEqual(matrix[1], [0.0, 3.0, 0.0, 0.0])
            self.assertEqual(matrix[2], [0.0, 0.0, 4.0, 0.0])
            self.assertEqual(matrix[3], [7.0, 8.0, 9.0, 1.0])

    def test_main_view_keeps_legacy_external_path_only(self):
        scene = SimpleNamespace(
            route="Scene/main.scene",
            vision_source_path="D:/vision/scene.json",
            vision_import_mode="external",
            vision_bindings=[{"actor_guid": "actor-chair"}],
        )
        legacy_loads = []
        fake_editor = SimpleNamespace(
            CoronaEngine=SimpleNamespace(
                is_vision_available=lambda: True,
                load_vision_scene=lambda path: legacy_loads.append(path),
            )
        )

        with patch.object(main_view_module, "CoronaEditor", fake_editor):
            main_view_module.MainView._apply_vision_source_for_scene(scene)

        self.assertEqual(legacy_loads, ["D:/vision/scene.json"])


if __name__ == "__main__":
    unittest.main()

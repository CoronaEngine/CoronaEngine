import configparser
import unittest
import contextvars
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from CoronaCore.core import network_sync_policy
from CoronaCore.core.entities import actor as actor_module
from CoronaCore.core.entities import scene as scene_module


class FakeActorEngineObject:
    def __init__(self):
        self.active_profile = None
        self.follow_camera = False
        self.profiles = []

    def add_profile(self, profile):
        self.profiles.append(profile)
        return profile

    def remove_profile(self, profile):
        if profile in self.profiles:
            self.profiles.remove(profile)
        if self.active_profile is profile:
            self.active_profile = self.profiles[0] if self.profiles else None

    def set_active_profile(self, profile):
        self.active_profile = profile

    def get_active_profile(self):
        return self.active_profile

    def get_handle(self):
        return 1234

    def set_follow_camera(self, enabled):
        self.follow_camera = bool(enabled)

    def get_follow_camera(self):
        return self.follow_camera


class FakeGeometry:
    def __init__(self, model_path):
        self.engine_obj = object()
        self.model_path = model_path
        self.position = [0.0, 0.0, 0.0]
        self.rotation = [0.0, 0.0, 0.0]
        self.scale = [1.0, 1.0, 1.0]

    def get_position(self):
        return self.position

    def set_position(self, position):
        self.position = position

    def get_rotation(self):
        return self.rotation

    def set_rotation(self, rotation):
        self.rotation = rotation

    def get_scale(self):
        return self.scale

    def set_scale(self, scale):
        self.scale = scale


class FakeOptics:
    def __init__(self, geometry):
        self.engine_obj = object()
        self.visible = True
        self.metallic = 0.0
        self.roughness = 0.5

    def get_visible(self):
        return self.visible

    def set_visible(self, visible):
        self.visible = bool(visible)

    def get_metallic(self):
        return self.metallic

    def set_metallic(self, value):
        self.metallic = value

    def get_roughness(self):
        return self.roughness

    def set_roughness(self, value):
        self.roughness = value

    def to_dict(self):
        return {
            "visible": self.visible,
            "metallic": self.metallic,
            "roughness": self.roughness,
        }


class FakeComponent:
    def __init__(self, geometry):
        self.engine_obj = object()
        self.physics_enabled = True
        self.collision_enabled = True
        self.mass = 1.0
        self.restitution = 0.8
        self.damping = 0.99
        self.linear_lock = [False, False, False]
        self.angular_lock = [False, False, False]

    def set_collision_callback(self, callback):
        self.collision_callback = callback

    def set_on_move_callback(self, callback):
        self.on_move_callback = callback

    def set_physics_enabled(self, enabled):
        self.physics_enabled = enabled

    def get_physics_enabled(self):
        return self.physics_enabled

    def set_collision_enabled(self, enabled):
        self.collision_enabled = enabled

    def get_collision_enabled(self):
        return self.collision_enabled

    def set_mass(self, value):
        self.mass = value

    def get_mass(self):
        return self.mass

    def set_restitution(self, value):
        self.restitution = value

    def get_restitution(self):
        return self.restitution

    def set_damping(self, value):
        self.damping = value

    def get_damping(self):
        return self.damping

    def set_linear_lock(self, lock_x, lock_y, lock_z):
        self.linear_lock = [lock_x, lock_y, lock_z]

    def get_linear_lock(self):
        return self.linear_lock

    def set_angular_lock(self, lock_x, lock_y, lock_z):
        self.angular_lock = [lock_x, lock_y, lock_z]

    def get_angular_lock(self):
        return self.angular_lock

    def to_dict(self):
        return {
            "mass": self.mass,
            "restitution": self.restitution,
            "damping": self.damping,
            "physics_enabled": self.physics_enabled,
            "linear_lock": list(self.linear_lock),
            "angular_lock": list(self.angular_lock),
        }


class ActorNetworkBroadcastTests(unittest.TestCase):
    def setUp(self):
        network_sync_policy.reset_for_tests()

    def tearDown(self):
        network_sync_policy.reset_for_tests()

    def _create_actor_with_events(self, *, route="Resource/cube.obj", name="", parent=None,
                                  actor_data=None, project_path="D:/project/test"):
        events = []
        fake_editor = SimpleNamespace(
            CoronaEngine=SimpleNamespace(
                active_project_path=project_path,
                Actor=FakeActorEngineObject,
                ActorProfile=SimpleNamespace,
            ),
            js_call_func=lambda name, args: events.append((name, args)),
        )
        if parent is None:
            parent = SimpleNamespace(route="Scene/main.scene")

        with patch.object(actor_module, "CoronaEditor", fake_editor), \
             patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
             patch.object(actor_module, "Geometry", FakeGeometry), \
             patch.object(actor_module, "Optics", FakeOptics), \
             patch.object(actor_module, "Mechanics", FakeComponent), \
             patch.object(actor_module, "Acoustics", FakeComponent):
            actor = actor_module.Actor(
                name=name,
                route=route,
                actor_type="model",
                parent_scene=parent,
                actor_data=actor_data,
            )
        return actor, events

    def test_actor_create_broadcast_happens_after_handle_is_available(self):
        events = []
        fake_editor = SimpleNamespace(
            CoronaEngine=SimpleNamespace(
                active_project_path="D:/project/test",
                Actor=FakeActorEngineObject,
                ActorProfile=SimpleNamespace,
            ),
            js_call_func=lambda name, args: events.append((name, args)),
        )
        parent = SimpleNamespace(route="Scene/main.scene")

        with patch.object(actor_module, "CoronaEditor", fake_editor), \
             patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
             patch.object(actor_module, "Geometry", FakeGeometry), \
             patch.object(actor_module, "Optics", FakeOptics), \
             patch.object(actor_module, "Mechanics", FakeComponent), \
             patch.object(actor_module, "Acoustics", FakeComponent):
            actor_module.Actor(route="Resource/cube.obj",
                               actor_type="model",
                               parent_scene=parent)

        self.assertEqual(len(events), 1)
        self.assertEqual(events[0][0], "actor-sync-broadcast")
        actor_data = events[0][1][0]
        self.assertEqual(actor_data["handle"], 1234)
        self.assertTrue(actor_data["actor_guid"])
        self.assertEqual(actor_data["scene"], "Scene/main.scene")

    def test_external_model_path_is_copied_into_project_resource_before_broadcast(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            project_root = root / "Project"
            external_root = root / "External"
            project_root.mkdir()
            external_root.mkdir()
            source_model = external_root / "Ball.obj"
            source_model.write_text("mtllib Ball.mtl\nmesh-data", encoding="utf-8")
            (external_root / "Ball.mtl").write_text(
                "map_Kd textures/Ball.png\n", encoding="utf-8")
            (external_root / "textures").mkdir()
            (external_root / "textures" / "Ball.png").write_bytes(b"png-data")

            events = []
            fake_editor = SimpleNamespace(
                CoronaEngine=SimpleNamespace(
                    active_project_path=str(project_root),
                    Actor=FakeActorEngineObject,
                    ActorProfile=SimpleNamespace,
                ),
                js_call_func=lambda name, args: events.append((name, args)),
            )
            parent = SimpleNamespace(route="Scene/main.scene")
            unsafe_route = "../External/Ball.obj"

            with patch.object(actor_module, "CoronaEditor", fake_editor), \
                 patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
                 patch.object(actor_module, "Geometry", FakeGeometry), \
                 patch.object(actor_module, "Optics", FakeOptics), \
                 patch.object(actor_module, "Mechanics", FakeComponent), \
                 patch.object(actor_module, "Acoustics", FakeComponent):
                actor_module.Actor(route=unsafe_route,
                                   actor_type="model",
                                   parent_scene=parent)

            actor_data = events[0][1][0]
            self.assertEqual(actor_data["path"], "Resource/Ball.obj")
            self.assertEqual(actor_data["model"], "Resource/Ball.obj")
            self.assertEqual(actor_data["scene"], "Scene/main.scene")
            self.assertEqual(actor_data["model_dependencies"], [
                "Resource/Ball.mtl",
                "Resource/textures/Ball.png",
            ])
            self.assertTrue((project_root / "Resource" / "Ball.obj").exists())
            self.assertEqual((project_root / "Resource" / "Ball.obj").read_text(encoding="utf-8"),
                             "mtllib Ball.mtl\nmesh-data")
            self.assertTrue((project_root / "Resource" / "Ball.mtl").exists())
            self.assertTrue((project_root / "Resource" / "textures" / "Ball.png").exists())

    def test_local_model_library_path_is_copied_to_stable_resource_before_broadcast(self):
        with tempfile.TemporaryDirectory() as tmp:
            project_root = Path(tmp)
            library_dir = (project_root / "assets" / "local_model_library" /
                           "models" / "书桌_6db78152")
            library_dir.mkdir(parents=True)
            source_model = library_dir / "base.glb"
            source_model.write_bytes(b"glb-data")

            events = []
            fake_editor = SimpleNamespace(
                CoronaEngine=SimpleNamespace(
                    active_project_path=str(project_root),
                    Actor=FakeActorEngineObject,
                    ActorProfile=SimpleNamespace,
                ),
                js_call_func=lambda name, args: events.append((name, args)),
            )
            parent = SimpleNamespace(route="Scene/main.scene")

            with patch.object(actor_module, "CoronaEditor", fake_editor), \
                 patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
                 patch.object(actor_module, "Geometry", FakeGeometry), \
                 patch.object(actor_module, "Optics", FakeOptics), \
                 patch.object(actor_module, "Mechanics", FakeComponent), \
                 patch.object(actor_module, "Acoustics", FakeComponent):
                actor_module.Actor(
                    route="assets/local_model_library/models/书桌_6db78152/base.glb",
                    actor_type="model",
                    parent_scene=parent,
                )

            actor_data = events[0][1][0]
            stable_path = "Resource/local_model_library/models/书桌_6db78152/base.glb"
            self.assertEqual(actor_data["path"], stable_path)
            self.assertEqual(actor_data["model"], stable_path)
            self.assertEqual(actor_data["model_dependencies"], [])
            self.assertEqual((project_root / stable_path).read_bytes(), b"glb-data")

    def test_models_path_prefers_runtime_sibling_before_broadcast(self):
        with tempfile.TemporaryDirectory() as tmp:
            project_root = Path(tmp)
            model_dir = project_root / "models" / "书桌"
            runtime_dir = model_dir / "runtime"
            runtime_dir.mkdir(parents=True)
            (model_dir / "base.glb").write_bytes(b"original-glb-data")
            (runtime_dir / "base.glb").write_bytes(b"runtime-glb-data")

            events = []
            fake_editor = SimpleNamespace(
                CoronaEngine=SimpleNamespace(
                    active_project_path=str(project_root),
                    Actor=FakeActorEngineObject,
                    ActorProfile=SimpleNamespace,
                ),
                js_call_func=lambda name, args: events.append((name, args)),
            )
            parent = SimpleNamespace(route="Scene/main.scene")

            with patch.object(actor_module, "CoronaEditor", fake_editor), \
                 patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
                 patch.object(actor_module, "Geometry", FakeGeometry), \
                 patch.object(actor_module, "Optics", FakeOptics), \
                 patch.object(actor_module, "Mechanics", FakeComponent), \
                 patch.object(actor_module, "Acoustics", FakeComponent):
                actor_module.Actor(
                    route="models/书桌/base.glb",
                    actor_type="model",
                    parent_scene=parent,
                )

            actor_data = events[0][1][0]
            stable_path = "Resource/models/书桌/runtime/base.glb"
            self.assertEqual(actor_data["path"], stable_path)
            self.assertEqual(actor_data["model"], stable_path)
            self.assertEqual(actor_data["model_dependencies"], [])
            self.assertEqual((project_root / stable_path).read_bytes(), b"runtime-glb-data")
            self.assertFalse((project_root / "Resource" / "models" / "书桌" / "base.glb").exists())

    def test_models_path_is_copied_to_resource_with_obj_dependencies_before_broadcast(self):
        with tempfile.TemporaryDirectory() as tmp:
            project_root = Path(tmp)
            model_dir = project_root / "models" / "矮桌"
            texture_dir = model_dir / "textures"
            texture_dir.mkdir(parents=True)
            (model_dir / "base.obj").write_text(
                "mtllib base.mtl\nmesh-data",
                encoding="utf-8",
            )
            (model_dir / "base.mtl").write_text(
                "map_Kd textures/diffuse.png\n",
                encoding="utf-8",
            )
            (texture_dir / "diffuse.png").write_bytes(b"png-data")

            events = []
            fake_editor = SimpleNamespace(
                CoronaEngine=SimpleNamespace(
                    active_project_path=str(project_root),
                    Actor=FakeActorEngineObject,
                    ActorProfile=SimpleNamespace,
                ),
                js_call_func=lambda name, args: events.append((name, args)),
            )
            parent = SimpleNamespace(route="Scene/main.scene")

            with patch.object(actor_module, "CoronaEditor", fake_editor), \
                 patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
                 patch.object(actor_module, "Geometry", FakeGeometry), \
                 patch.object(actor_module, "Optics", FakeOptics), \
                 patch.object(actor_module, "Mechanics", FakeComponent), \
                 patch.object(actor_module, "Acoustics", FakeComponent):
                actor_module.Actor(
                    route="models/矮桌/base.obj",
                    actor_type="model",
                    parent_scene=parent,
                )

            actor_data = events[0][1][0]
            stable_path = "Resource/models/矮桌/base.obj"
            self.assertEqual(actor_data["path"], stable_path)
            self.assertEqual(actor_data["model"], stable_path)
            self.assertEqual(actor_data["model_dependencies"], [
                "Resource/models/矮桌/base.mtl",
                "Resource/models/矮桌/textures/diffuse.png",
            ])
            self.assertEqual((project_root / stable_path).read_text(encoding="utf-8"),
                             "mtllib base.mtl\nmesh-data")
            self.assertTrue((project_root / "Resource" / "models" / "矮桌" / "base.mtl").exists())
            self.assertTrue((project_root / "Resource" / "models" / "矮桌" / "textures" / "diffuse.png").exists())

    def test_gltf_external_dependencies_are_copied_before_broadcast(self):
        with tempfile.TemporaryDirectory() as tmp:
            project_root = Path(tmp)
            model_dir = project_root / "models" / "plant"
            model_dir.mkdir(parents=True)
            (model_dir / "scene.gltf").write_text(
                '{"buffers":[{"uri":"scene.bin"}],"images":[{"uri":"textures/diffuse.png"},{"uri":"data:image/png;base64,AAAA"}]}',
                encoding="utf-8",
            )
            (model_dir / "scene.bin").write_bytes(b"bin-data")
            (model_dir / "textures").mkdir()
            (model_dir / "textures" / "diffuse.png").write_bytes(b"png-data")

            actor, events = self._create_actor_with_events(
                route="models/plant/scene.gltf",
                project_path=str(project_root),
            )

            actor_data = events[0][1][0]
            self.assertEqual(actor_data["path"], "Resource/models/plant/scene.gltf")
            self.assertEqual(actor_data["model_dependencies"], [
                "Resource/models/plant/scene.bin",
                "Resource/models/plant/textures/diffuse.png",
            ])
            self.assertTrue((project_root / "Resource" / "models" / "plant" / "scene.bin").exists())
            self.assertTrue((project_root / "Resource" / "models" / "plant" / "textures" / "diffuse.png").exists())

    def test_existing_resource_gltf_dependencies_are_advertised_without_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            project_root = Path(tmp)
            model_dir = project_root / "Resource" / "models" / "plant"
            model_dir.mkdir(parents=True)
            (model_dir / "scene.gltf").write_text(
                '{"buffers":[{"uri":"scene.bin"}],"images":[{"uri":"textures/diffuse.png"}]}',
                encoding="utf-8",
            )
            (model_dir / "scene.bin").write_bytes(b"bin-data")
            (model_dir / "textures").mkdir()
            (model_dir / "textures" / "diffuse.png").write_bytes(b"png-data")

            _, events = self._create_actor_with_events(
                route="Resource/models/plant/scene.gltf",
                project_path=str(project_root),
            )

            actor_data = events[0][1][0]
            self.assertEqual(actor_data["path"], "Resource/models/plant/scene.gltf")
            self.assertEqual(actor_data["model_dependencies"], [
                "Resource/models/plant/scene.bin",
                "Resource/models/plant/textures/diffuse.png",
            ])

    def test_fbx_copies_common_same_directory_material_assets(self):
        with tempfile.TemporaryDirectory() as tmp:
            project_root = Path(tmp)
            model_dir = project_root / "models" / "table"
            model_dir.mkdir(parents=True)
            (model_dir / "table.fbx").write_bytes(b"fbx-data")
            (model_dir / "table.png").write_bytes(b"png-data")
            (model_dir / "table.mtl").write_text("material", encoding="utf-8")
            (model_dir / "notes.txt").write_text("ignore", encoding="utf-8")

            _, events = self._create_actor_with_events(
                route="models/table/table.fbx",
                project_path=str(project_root),
            )

            actor_data = events[0][1][0]
            self.assertEqual(actor_data["path"], "Resource/models/table/table.fbx")
            self.assertEqual(actor_data["model_dependencies"], [
                "Resource/models/table/table.mtl",
                "Resource/models/table/table.png",
            ])
            self.assertTrue((project_root / "Resource" / "models" / "table" / "table.mtl").exists())
            self.assertTrue((project_root / "Resource" / "models" / "table" / "table.png").exists())
            self.assertFalse((project_root / "Resource" / "models" / "table" / "notes.txt").exists())

    def test_scene_remove_actor_emits_delete_sync_broadcast(self):
        events = []
        fake_editor = SimpleNamespace(
            js_call_func=lambda name, args: events.append((name, args)),
        )
        scene = scene_module.Scene.__new__(scene_module.Scene)
        scene.route = "Scene/main.scene"
        scene._notify_scene_tree_changed = lambda: None
        scene.engine_scene = SimpleNamespace(remove_actor=lambda engine_obj: None)
        actor = SimpleNamespace(
            name="chair",
            actor_guid="actor-chair",
            engine_obj=object(),
            network_remote=False,
            _suppress_network_broadcast=False,
        )
        actor._optics = object()
        scene._actors = [actor]

        with patch.object(scene_module, "CoronaEditor", fake_editor):
            self.assertTrue(scene.remove_actor(actor))

        self.assertEqual(events, [
            ("actor-delete-sync-broadcast", [{
                "scene": "Scene/main.scene",
                "actor_guid": "actor-chair",
                "actor_name": "chair",
            }])
        ])

    def test_scene_remove_remote_actor_does_not_rebroadcast_delete(self):
        events = []
        fake_editor = SimpleNamespace(
            js_call_func=lambda name, args: events.append((name, args)),
        )
        scene = scene_module.Scene.__new__(scene_module.Scene)
        scene.route = "Scene/main.scene"
        scene._notify_scene_tree_changed = lambda: None
        scene.engine_scene = SimpleNamespace(remove_actor=lambda engine_obj: None)
        actor = SimpleNamespace(
            name="chair",
            actor_guid="actor-chair",
            engine_obj=object(),
            network_remote=True,
            _suppress_network_broadcast=True,
        )
        actor._optics = object()
        scene._actors = [actor]

        with patch.object(scene_module, "CoronaEditor", fake_editor):
            self.assertTrue(scene.remove_actor(actor))

        self.assertEqual(events, [])

    def test_remote_actor_disables_local_physics(self):
        with tempfile.TemporaryDirectory() as tmp:
            project_root = Path(tmp)
            (project_root / "Resource").mkdir()
            (project_root / "Resource" / "cube.obj").write_text("mesh", encoding="utf-8")
            fake_editor = SimpleNamespace(
                CoronaEngine=SimpleNamespace(
                    active_project_path=str(project_root),
                    Actor=FakeActorEngineObject,
                    ActorProfile=SimpleNamespace,
                ),
                js_call_func=lambda name, args: None,
            )
            parent = SimpleNamespace(route="Scene/main.scene")

            with patch.object(actor_module, "CoronaEditor", fake_editor), \
                 patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
                 patch.object(actor_module, "Geometry", FakeGeometry), \
                 patch.object(actor_module, "Optics", FakeOptics), \
                 patch.object(actor_module, "Mechanics", FakeComponent), \
                 patch.object(actor_module, "Acoustics", FakeComponent):
                actor = actor_module.Actor(
                    route="Resource/cube.obj",
                    actor_type="model",
                    parent_scene=parent,
                    actor_data={
                        "actor_guid": "actor-remote",
                        "_suppress_network_broadcast": True,
                        "geometry": {
                            "position": [0, 0, 0],
                            "rotation": [0, 0, 0],
                            "scale": [1, 1, 1],
                        },
                    },
                )

            self.assertFalse(actor._mechanics.get_physics_enabled())

    def test_actor_in_internal_temp_scene_does_not_broadcast(self):
        parent = SimpleNamespace(route="__six_view_tmp_abc__")

        _, events = self._create_actor_with_events(parent=parent)

        self.assertEqual(events, [])

    def test_internal_actor_name_does_not_broadcast(self):
        _, events = self._create_actor_with_events(name="__six_view_tmp_actor")

        self.assertEqual(events, [])

    def test_ai_scene_framework_actor_name_does_broadcast(self):
        _, events = self._create_actor_with_events(name="__room_box")

        self.assertEqual(len(events), 1)
        self.assertEqual(events[0][0], "actor-sync-broadcast")
        self.assertEqual(events[0][1][0]["name"], "__room_box")

    def test_actor_data_policy_reports_all_snapshot_block_reasons(self):
        base = {
            "name": "chair",
            "actor_guid": "actor-chair",
            "actor_type": "model",
            "scene": "Scene/main.scene",
            "path": "Resource/chair.obj",
            "model": "Resource/chair.obj",
            "geometry": {
                "position": [0, 0, 0],
                "rotation": [0, 0, 0],
                "scale": [1, 1, 1],
            },
        }

        self.assertIsNone(network_sync_policy.actor_data_sync_block_reason(base))
        self.assertEqual(
            network_sync_policy.actor_data_sync_block_reason(
                {**base, "actor_type": "actor"}),
            "actor_type_actor",
        )
        data_without_geometry = dict(base)
        data_without_geometry.pop("geometry")
        self.assertEqual(
            network_sync_policy.actor_data_sync_block_reason(data_without_geometry),
            "missing_geometry",
        )
        self.assertEqual(
            network_sync_policy.actor_data_sync_block_reason(
                {**base, "name": "__six_view_tmp_actor"}),
            "internal_actor_name",
        )
        self.assertEqual(
            network_sync_policy.actor_data_sync_block_reason(
                {**base, "scene": "__preview_scene"}),
            "internal_scene_name",
        )
        self.assertEqual(
            network_sync_policy.actor_data_sync_block_reason(
                {**base, "path": "", "model": ""}),
            "missing_model_path",
        )
        self.assertEqual(
            network_sync_policy.actor_data_sync_block_reason(
                {**base, "_suppress_network_broadcast": True}),
            "suppressed",
        )

    def test_actor_data_policy_keeps_ai_framework_names_syncable(self):
        base = {
            "actor_guid": "actor-framework",
            "actor_type": "model",
            "scene": "Scene/main.scene",
            "path": "Resource/framework.obj",
            "model": "Resource/framework.obj",
            "geometry": {
                "position": [0, 0, 0],
                "rotation": [0, 0, 0],
                "scale": [1, 1, 1],
            },
        }

        self.assertIsNone(network_sync_policy.actor_data_sync_block_reason({
            **base,
            "name": "__room_box",
        }))
        self.assertIsNone(network_sync_policy.actor_data_sync_block_reason({
            **base,
            "name": "__shell_wall_01",
        }))

    def test_suppressed_actor_does_not_broadcast(self):
        with tempfile.TemporaryDirectory() as tmp:
            project_root = Path(tmp)
            (project_root / "Resource").mkdir()
            (project_root / "Resource" / "cube.obj").write_text("mesh", encoding="utf-8")

            _, events = self._create_actor_with_events(
                project_path=str(project_root),
                actor_data={
                    "actor_guid": "actor-remote",
                    "_suppress_network_broadcast": True,
                    "geometry": {
                        "position": [0, 0, 0],
                        "rotation": [0, 0, 0],
                        "scale": [1, 1, 1],
                    },
                },
            )

        self.assertEqual(events, [])

    def test_deferred_mode_flushes_only_existing_normal_actors(self):
        events = []
        fake_editor = SimpleNamespace(
            CoronaEngine=SimpleNamespace(
                active_project_path="D:/project/test",
                Actor=FakeActorEngineObject,
                ActorProfile=SimpleNamespace,
            ),
            js_call_func=lambda name, args: events.append((name, args)),
        )
        parent = SimpleNamespace(route="Scene/main.scene")

        with patch.object(actor_module, "CoronaEditor", fake_editor), \
             patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
             patch.object(actor_module, "Geometry", FakeGeometry), \
             patch.object(actor_module, "Optics", FakeOptics), \
             patch.object(actor_module, "Mechanics", FakeComponent), \
             patch.object(actor_module, "Acoustics", FakeComponent):
            with network_sync_policy.deferred_actor_broadcasts():
                kept = actor_module.Actor(
                    name="chair",
                    route="Resource/chair.obj",
                    actor_type="model",
                    parent_scene=parent,
                )
                actor_module.Actor(
                    name="__wb_floor",
                    route="Resource/floor.obj",
                    actor_type="model",
                    parent_scene=parent,
                )
                removed = actor_module.Actor(
                    name="table",
                    route="Resource/table.obj",
                    actor_type="model",
                    parent_scene=parent,
                )
                removed.parent = None
                self.assertEqual(events, [])

        self.assertEqual(len(events), 1)
        self.assertEqual(events[0][0], "actor-sync-broadcast")
        self.assertEqual(events[0][1][0]["actor_guid"], kept.actor_guid)
        self.assertEqual(events[0][1][0]["name"], "chair")

    def test_filtered_actor_create_logs_diagnostic_reason(self):
        events = []

        class SyncableActorWithBadPayload:
            name = "missing-path"
            actor_type = "model"
            _geometry = object()
            _suppress_network_broadcast = False
            parent = SimpleNamespace(route="Scene/main.scene")

            def to_dict(self):
                return {
                    "name": self.name,
                    "actor_type": self.actor_type,
                    "actor_guid": "actor-missing-path",
                    "scene": "Scene/main.scene",
                    "path": "",
                    "model": "",
                    "geometry": {
                        "position": [0, 0, 0],
                        "rotation": [0, 0, 0],
                        "scale": [1, 1, 1],
                    },
                }

        with self.assertLogs(network_sync_policy.logger, level="WARNING") as captured:
            network_sync_policy.publish_actor_created(
                SyncableActorWithBadPayload(),
                prepare=None,
                emit=lambda actor_data: events.append(actor_data),
            )

        self.assertEqual(events, [])
        self.assertTrue(any("missing_model_path" in line for line in captured.output))
        self.assertTrue(any("actor-missing-path" in line for line in captured.output))

    def test_actor_level_filter_log_uses_actor_path_fallbacks(self):
        class ActorWithoutGeometry:
            name = "config-actor"
            actor_type = "actor"
            actor_guid = "actor-config"
            route = "Resource/config.actor"
            model_path = "Resource/config_model.obj"
            _suppress_network_broadcast = False
            parent = SimpleNamespace(route="Scene/main.scene")

        with self.assertLogs(network_sync_policy.logger, level="WARNING") as captured:
            network_sync_policy.publish_actor_created(
                ActorWithoutGeometry(),
                prepare=None,
                emit=lambda actor_data: None,
            )

        joined = "\n".join(captured.output)
        self.assertIn("actor_type_actor", joined)
        self.assertIn("Resource/config.actor", joined)
        self.assertIn("Resource/config_model.obj", joined)
        self.assertIn("Scene/main.scene", joined)

    def test_deferred_mode_discards_events_on_failure(self):
        events = []
        fake_editor = SimpleNamespace(
            CoronaEngine=SimpleNamespace(
                active_project_path="D:/project/test",
                Actor=FakeActorEngineObject,
                ActorProfile=SimpleNamespace,
            ),
            js_call_func=lambda name, args: events.append((name, args)),
        )
        parent = SimpleNamespace(route="Scene/main.scene")

        with patch.object(actor_module, "CoronaEditor", fake_editor), \
             patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
             patch.object(actor_module, "Geometry", FakeGeometry), \
             patch.object(actor_module, "Optics", FakeOptics), \
             patch.object(actor_module, "Mechanics", FakeComponent), \
             patch.object(actor_module, "Acoustics", FakeComponent):
            with self.assertRaises(RuntimeError):
                with network_sync_policy.deferred_actor_broadcasts():
                    actor_module.Actor(
                        name="chair",
                        route="Resource/chair.obj",
                        actor_type="model",
                        parent_scene=parent,
                    )
                    raise RuntimeError("cancelled")

        self.assertEqual(events, [])

    def test_deferred_mode_does_not_capture_actor_created_in_separate_context(self):
        events = []

        fake_editor = SimpleNamespace(
            CoronaEngine=SimpleNamespace(
                active_project_path="D:/project/test",
                Actor=FakeActorEngineObject,
                ActorProfile=SimpleNamespace,
            ),
            js_call_func=lambda name, args: events.append((name, args)),
        )
        parent = SimpleNamespace(route="Scene/main.scene")

        with patch.object(actor_module, "CoronaEditor", fake_editor), \
             patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
             patch.object(actor_module, "Geometry", FakeGeometry), \
             patch.object(actor_module, "Optics", FakeOptics), \
             patch.object(actor_module, "Mechanics", FakeComponent), \
             patch.object(actor_module, "Acoustics", FakeComponent):
            with network_sync_policy.deferred_actor_broadcasts():
                actor_module.Actor(
                    name="ai_chair",
                    route="Resource/ai_chair.obj",
                    actor_type="model",
                    parent_scene=parent,
                )

                def create_manual_actor():
                    actor_module.Actor(
                        name="manual_table",
                        route="Resource/manual_table.obj",
                        actor_type="model",
                        parent_scene=parent,
                    )

                contextvars.Context().run(create_manual_actor)
                self.assertEqual([e[1][0]["name"] for e in events], ["manual_table"])

        self.assertEqual([e[1][0]["name"] for e in events], ["manual_table", "ai_chair"])

    def test_actor_move_emits_ownership_claim(self):
        events = []
        fake_editor = SimpleNamespace(
            CoronaEngine=SimpleNamespace(
                active_project_path="D:/project/test",
                Actor=FakeActorEngineObject,
                ActorProfile=SimpleNamespace,
            ),
            js_call_func=lambda name, args: events.append((name, args)),
        )
        parent = SimpleNamespace(route="Scene/main.scene", save_data=lambda: None)

        with patch.object(actor_module, "CoronaEditor", fake_editor), \
             patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
             patch.object(actor_module, "Geometry", FakeGeometry), \
             patch.object(actor_module, "Optics", FakeOptics), \
             patch.object(actor_module, "Mechanics", FakeComponent), \
             patch.object(actor_module, "Acoustics", FakeComponent):
            actor = actor_module.Actor(route="Resource/cube.obj",
                                       actor_type="model",
                                       parent_scene=parent)
            actor.on_move()

        claims = [args[0] for name, args in events if name == "actor-ownership-claim"]
        self.assertTrue(claims)
        self.assertEqual(claims[-1]["actor_guid"], actor.actor_guid)

    def test_local_transform_setters_emit_actor_transform_sync(self):
        events = []
        fake_editor = SimpleNamespace(
            CoronaEngine=SimpleNamespace(
                active_project_path="D:/project/test",
                Actor=FakeActorEngineObject,
                ActorProfile=SimpleNamespace,
            ),
            js_call_func=lambda name, args: events.append((name, args)),
        )
        parent = SimpleNamespace(route="Scene/main.scene", save_data=lambda: None)

        with patch.object(actor_module, "CoronaEditor", fake_editor), \
             patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
             patch.object(actor_module, "Geometry", FakeGeometry), \
             patch.object(actor_module, "Optics", FakeOptics), \
             patch.object(actor_module, "Mechanics", FakeComponent), \
             patch.object(actor_module, "Acoustics", FakeComponent):
            actor = actor_module.Actor(route="Resource/cube.obj",
                                       actor_type="model",
                                       parent_scene=parent)
            events.clear()
            actor.set_position([1.0, 2.0, 3.0])
            actor.set_rotation([0.0, 1.5708, 0.0])
            actor.set_scale([2.0, 2.0, 2.0])

        updates = [args[0] for name, args in events
                   if name == "actor-transform-sync-broadcast"]
        self.assertEqual(len(updates), 3)
        self.assertEqual(updates[-1]["actor_guid"], actor.actor_guid)
        self.assertEqual(updates[-1]["scene"], "Scene/main.scene")
        self.assertEqual(updates[-1]["geometry"]["position"], [1.0, 2.0, 3.0])
        self.assertEqual(updates[-1]["geometry"]["rotation"], [0.0, 1.5708, 0.0])
        self.assertEqual(updates[-1]["geometry"]["scale"], [2.0, 2.0, 2.0])

    def test_remote_transform_apply_does_not_rebroadcast(self):
        events = []
        fake_editor = SimpleNamespace(
            CoronaEngine=SimpleNamespace(
                active_project_path="D:/project/test",
                Actor=FakeActorEngineObject,
                ActorProfile=SimpleNamespace,
            ),
            js_call_func=lambda name, args: events.append((name, args)),
        )
        parent = SimpleNamespace(route="Scene/main.scene", save_data=lambda: None)

        with patch.object(actor_module, "CoronaEditor", fake_editor), \
             patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
             patch.object(actor_module, "Geometry", FakeGeometry), \
             patch.object(actor_module, "Optics", FakeOptics), \
             patch.object(actor_module, "Mechanics", FakeComponent), \
             patch.object(actor_module, "Acoustics", FakeComponent):
            actor = actor_module.Actor(route="Resource/cube.obj",
                                       actor_type="model",
                                       parent_scene=parent)
            events.clear()
            actor.set_position([4.0, 5.0, 6.0], if_init=True)
            actor.set_rotation([0.0, 0.5, 0.0], if_init=True)
            actor.set_scale([1.5, 1.5, 1.5], if_init=True)

        updates = [event for event in events if event[0] == "actor-transform-sync-broadcast"]
        self.assertEqual(updates, [])

    def test_set_model_replaces_profile_and_preserves_edit_state(self):
        fake_editor = SimpleNamespace(
            CoronaEngine=SimpleNamespace(
                active_project_path="D:/project/test",
                Actor=FakeActorEngineObject,
                ActorProfile=SimpleNamespace,
            ),
            js_call_func=lambda name, args: None,
        )
        parent = SimpleNamespace(route="Scene/main.scene", save_data=lambda: None)

        with patch.object(actor_module, "CoronaEditor", fake_editor), \
             patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
             patch.object(actor_module, "Geometry", FakeGeometry), \
             patch.object(actor_module, "Optics", FakeOptics), \
             patch.object(actor_module, "Mechanics", FakeComponent), \
             patch.object(actor_module, "Acoustics", FakeComponent):
            actor = actor_module.Actor(route="Resource/cube.obj",
                                       actor_type="model",
                                       parent_scene=parent)
            old_profile = actor._profile
            old_geometry = actor._geometry

            actor.set_position([1.0, 2.0, 3.0])
            actor.set_rotation([10.0, 20.0, 30.0])
            actor.set_scale([2.0, 3.0, 4.0])
            actor.set_visible(False)
            actor._optics.set_metallic(0.7)
            actor._optics.set_roughness(0.2)
            actor.set_mass(5.5)
            actor.set_physics_enabled(False)
            actor.set_collision_enabled("none")

            actor.set_model("Resource/sphere.obj")

        self.assertEqual(actor.model_path, "Resource/sphere.obj")
        self.assertEqual(actor.final_model_path, "D:/project/test\\Resource/sphere.obj")
        self.assertIsNot(actor._profile, old_profile)
        self.assertIsNot(actor._geometry, old_geometry)
        self.assertEqual(actor.engine_obj.active_profile, actor._profile)
        self.assertEqual(actor.engine_obj.profiles, [actor._profile])
        self.assertEqual(actor._geometry.model_path, "D:/project/test\\Resource/sphere.obj")
        self.assertEqual(actor.get_position(), [1.0, 2.0, 3.0])
        self.assertEqual(actor.get_rotation(), [10.0, 20.0, 30.0])
        self.assertEqual(actor.get_scale(), [2.0, 3.0, 4.0])
        self.assertFalse(actor.get_visible())
        self.assertEqual(actor._optics.get_metallic(), 0.7)
        self.assertEqual(actor._optics.get_roughness(), 0.2)
        self.assertEqual(actor.get_mass(), 5.5)
        self.assertFalse(actor.get_physics_enabled())
        self.assertEqual(actor.get_collision_enabled(), "none")

    def test_transform_contract_separates_absolute_and_delta_operations(self):
        fake_editor = SimpleNamespace(
            CoronaEngine=SimpleNamespace(
                active_project_path="D:/project/test",
                Actor=FakeActorEngineObject,
                ActorProfile=SimpleNamespace,
            ),
            js_call_func=lambda name, args: None,
        )
        parent = SimpleNamespace(route="Scene/main.scene", save_data=lambda: None)

        with patch.object(actor_module, "CoronaEditor", fake_editor), \
             patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
             patch.object(actor_module, "Geometry", FakeGeometry), \
             patch.object(actor_module, "Optics", FakeOptics), \
             patch.object(actor_module, "Mechanics", FakeComponent), \
             patch.object(actor_module, "Acoustics", FakeComponent):
            actor = actor_module.Actor(route="Resource/cube.obj",
                                       actor_type="model",
                                       parent_scene=parent)

            actor.set_position([1.0, 2.0, 3.0])
            actor.set_rotation([10.0, 20.0, 30.0])
            actor.set_scale([2.0, 3.0, 4.0])
            self.assertEqual(actor.get_position(), [1.0, 2.0, 3.0])
            self.assertEqual(actor.get_rotation(), [10.0, 20.0, 30.0])
            self.assertEqual(actor.get_scale(), [2.0, 3.0, 4.0])

            actor.translate([0.5, -1.0, 2.0])
            actor.rotate_delta([5.0, 0.0, -10.0])
            actor.scale_delta([2.0, 0.5, 1.0])
            self.assertEqual(actor.get_position(), [1.5, 1.0, 5.0])
            self.assertEqual(actor.get_rotation(), [15.0, 20.0, 20.0])
            self.assertEqual(actor.get_scale(), [4.0, 1.5, 4.0])

            actor.move([1.0, 1.0, 1.0])
            actor.rotate([1.0, 2.0, 3.0])
            actor.scale([9.0, 8.0, 7.0])
            self.assertEqual(actor.get_position(), [2.5, 2.0, 6.0])
            self.assertEqual(actor.get_rotation(), [16.0, 22.0, 23.0])
            self.assertEqual(actor.get_scale(), [9.0, 8.0, 7.0])

    def test_follow_camera_round_trips_to_engine_and_to_dict(self):
        fake_editor = SimpleNamespace(
            CoronaEngine=SimpleNamespace(
                active_project_path="D:/project/test",
                Actor=FakeActorEngineObject,
                ActorProfile=SimpleNamespace,
            ),
            js_call_func=lambda name, args: None,
        )
        parent = SimpleNamespace(route="Scene/main.scene", save_data=lambda: None)

        with patch.object(actor_module, "CoronaEditor", fake_editor), \
             patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
             patch.object(actor_module, "Geometry", FakeGeometry), \
             patch.object(actor_module, "Optics", FakeOptics), \
             patch.object(actor_module, "Mechanics", FakeComponent), \
             patch.object(actor_module, "Acoustics", FakeComponent):
            actor = actor_module.Actor(route="Resource/cube.obj",
                                       actor_type="model",
                                       parent_scene=parent)

            self.assertFalse(actor.to_dict()["follow_camera"])

            actor.set_follow_camera(True)
            data = actor.to_dict()
            self.assertTrue(actor.engine_obj.get_follow_camera())
            self.assertTrue(data["follow_camera"])
            self.assertEqual(data["render_space"], "ui")

            actor.set_follow_camera(False)
            data = actor.to_dict()
            self.assertFalse(actor.engine_obj.get_follow_camera())
            self.assertFalse(data["follow_camera"])
            self.assertEqual(data["render_space"], "scene")

    def test_follow_camera_disables_physics_once_without_restore(self):
        fake_editor = SimpleNamespace(
            CoronaEngine=SimpleNamespace(
                active_project_path="D:/project/test",
                Actor=FakeActorEngineObject,
                ActorProfile=SimpleNamespace,
            ),
            js_call_func=lambda name, args: None,
        )
        parent = SimpleNamespace(route="Scene/main.scene", save_data=lambda: None)

        with patch.object(actor_module, "CoronaEditor", fake_editor), \
             patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
             patch.object(actor_module, "Geometry", FakeGeometry), \
             patch.object(actor_module, "Optics", FakeOptics), \
             patch.object(actor_module, "Mechanics", FakeComponent), \
             patch.object(actor_module, "Acoustics", FakeComponent):
            actor = actor_module.Actor(route="Resource/cube.obj",
                                       actor_type="model",
                                       parent_scene=parent)

            self.assertTrue(actor.get_physics_enabled())

            actor.set_follow_camera(True)
            self.assertTrue(actor.get_follow_camera())
            self.assertFalse(actor.get_physics_enabled())

            actor.set_follow_camera(False)
            self.assertFalse(actor.get_follow_camera())
            self.assertFalse(actor.get_physics_enabled())

            actor.set_physics_enabled(True)
            actor.set_follow_camera(True, if_init=True)
            self.assertTrue(actor.get_follow_camera())
            self.assertTrue(actor.get_physics_enabled())

    def test_scene_actor_follow_camera_persists_in_scene_actor_section(self):
        with tempfile.TemporaryDirectory() as tmp:
            scene_path = Path(tmp) / "main.scene"
            scene = scene_module.Scene.__new__(scene_module.Scene)
            scene.route = str(scene_path)
            scene.name = "main"
            scene.file_data = configparser.ConfigParser()
            scene._environment = None
            scene._cameras = []
            scene._main_camera = None
            scene.engine_scene = SimpleNamespace(
                add_camera=lambda camera: None,
                set_active_camera=lambda camera: None,
            )
            scene.script_path = ""
            scene.terrain_path = ""
            scene.terrain_type = ""
            scene.vision_source_path = ""
            scene.vision_import_mode = ""
            scene.get_active_camera = lambda: None
            scene._actors = [
                SimpleNamespace(
                    name="hud_quad",
                    actor_guid="actor-hud",
                    actor_type="model",
                    route="Resource/hud.obj",
                    _geometry=True,
                    get_position=lambda: [
                        0.12345678912345678,
                        -0.9876543219876543,
                        2.000000000000001,
                    ],
                    get_rotation=lambda: [
                        0.000000123456789,
                        -0.000000987654321,
                        1.234567891234567,
                    ],
                    get_scale=lambda: [
                        1.000000000000001,
                        0.999999999999999,
                        1.234567891234567,
                    ],
                    get_follow_camera=lambda: True,
                    get_physics_enabled=lambda: False,
                )
            ]

            scene.save_data()

            saved = configparser.ConfigParser()
            saved.read(scene_path, encoding="utf-8")
            self.assertEqual(saved["actors"]["hud_quad.name"], "hud_quad")
            self.assertTrue(saved["actors"].getboolean("hud_quad.follow_camera"))
            self.assertFalse(saved["actors"].getboolean("hud_quad.mechanics.physics_enabled"))
            self.assertEqual(saved["actors"].get("hud_quad.actor_guid"), "actor-hud")
            self.assertEqual(saved["terrain"]["path"], "")
            self.assertEqual(saved["terrain"]["type"], "")
            self.assertNotIn("vision", saved)

            actor_data = scene._build_actor_json(saved["actors"], "hud_quad")
            self.assertEqual(actor_data["name"], "hud_quad")
            self.assertTrue(actor_data["follow_camera"])
            self.assertFalse(actor_data["mechanics"]["physics_enabled"])
            self.assertEqual(actor_data["actor_guid"], "actor-hud")
            self.assertTrue(actor_data["_suppress_network_broadcast"])
            self.assertEqual(
                actor_data["geometry"]["position"],
                [0.12345678912345678, -0.9876543219876543, 2.000000000000001],
            )
            self.assertEqual(
                actor_data["geometry"]["rotation"],
                [0.000000123456789, -0.000000987654321, 1.234567891234567],
            )
            self.assertEqual(
                actor_data["geometry"]["scale"],
                [1.000000000000001, 0.999999999999999, 1.234567891234567],
            )

    def test_actor_data_restores_physics_enabled_to_mechanics(self):
        with tempfile.TemporaryDirectory() as tmp:
            project_root = Path(tmp)
            (project_root / "Resource").mkdir()
            (project_root / "Resource" / "cube.obj").write_text("mesh", encoding="utf-8")
            fake_editor = SimpleNamespace(
                CoronaEngine=SimpleNamespace(
                    active_project_path=str(project_root),
                    Actor=FakeActorEngineObject,
                    ActorProfile=SimpleNamespace,
                ),
                js_call_func=lambda name, args: None,
            )
            parent = SimpleNamespace(route="Scene/main.scene", save_data=lambda: None)

            with patch.object(actor_module, "CoronaEditor", fake_editor), \
                 patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
                 patch.object(actor_module, "Geometry", FakeGeometry), \
                 patch.object(actor_module, "Optics", FakeOptics), \
                 patch.object(actor_module, "Mechanics", FakeComponent), \
                 patch.object(actor_module, "Acoustics", FakeComponent):
                actor = actor_module.Actor(
                    route="Resource/cube.obj",
                    actor_type="model",
                    parent_scene=parent,
                    actor_data={
                        "actor_guid": "actor-cube",
                        "geometry": {
                            "position": [0, 0, 0],
                            "rotation": [0, 0, 0],
                            "scale": [1, 1, 1],
                        },
                        "mechanics": {
                            "physics_enabled": False,
                        },
                    },
                )

            self.assertFalse(actor.get_physics_enabled())

    def test_scene_vision_bindings_persist_in_indexed_section(self):
        with tempfile.TemporaryDirectory() as tmp:
            scene_path = Path(tmp) / "main.scene"
            scene = scene_module.Scene.__new__(scene_module.Scene)
            scene.route = str(scene_path)
            scene.name = "main"
            scene.file_data = configparser.ConfigParser()
            scene._environment = None
            scene._cameras = []
            scene._main_camera = None
            scene.script_path = ""
            scene.terrain_path = ""
            scene.terrain_type = ""
            scene.vision_source_path = "D:/vision/scene.json"
            scene.vision_import_mode = "external_live"
            scene.vision_bindings = [{
                "actor_guid": "actor-cube",
                "actor_name": "Cube",
                "shape_guid": "vision-shape-cube",
                "shape_index": 0,
                "json_path": "/scene/shapes/0",
                "shape_type": "model",
                "model_path": "D:/vision/cube.obj",
                "source_path": "D:/vision/scene.json",
            }]
            scene.vision_unsupported_shapes = [{
                "shape_index": 1,
                "json_path": "/scene/shapes/1",
                "type": "quad",
                "reason": "unsupported_shape_type",
            }]
            scene._actors = []
            scene.get_active_camera = lambda: None

            scene.save_data()

            saved = configparser.ConfigParser()
            saved.read(scene_path, encoding="utf-8")
            self.assertEqual(saved["vision"]["import_mode"], "external_live")
            self.assertEqual(saved["vision_bindings"]["binding0.actor_guid"], "actor-cube")
            self.assertEqual(saved["vision_bindings"]["binding0.json_path"], "/scene/shapes/0")
            self.assertEqual(saved["vision_unsupported_shapes"]["shape0.type"], "quad")

            restored = scene_module.Scene.__new__(scene_module.Scene)
            restored.file_data = saved
            self.assertEqual(restored._read_indexed_section("vision_bindings")[0]["actor_guid"],
                             "actor-cube")

            read_scene_path = Path(tmp) / "read.scene"
            minimal = configparser.ConfigParser()
            minimal["vision"] = {
                "source_path": "D:/vision/scene.json",
                "import_mode": "external_live",
            }
            minimal["vision_bindings"] = {
                "binding0.actor_guid": "actor-cube",
                "binding0.json_path": "/scene/shapes/0",
            }
            with open(read_scene_path, "w", encoding="utf-8") as handle:
                minimal.write(handle)

            read_scene = scene_module.Scene.__new__(scene_module.Scene)
            read_scene.route = str(read_scene_path)
            read_scene.name = "read"
            read_scene.file_data = configparser.ConfigParser()
            read_scene._actors = []
            read_scene.vision_bindings = []
            read_scene.vision_unsupported_shapes = []
            read_scene.read_data()

            self.assertEqual(read_scene.vision_import_mode, "external_live")
            self.assertEqual(read_scene.vision_bindings[0]["actor_guid"], "actor-cube")

    def test_scene_actor_alias_is_separate_from_unique_ini_key(self):
        with tempfile.TemporaryDirectory() as tmp:
            scene_path = Path(tmp) / "main.scene"
            scene = scene_module.Scene.__new__(scene_module.Scene)
            scene.route = str(scene_path)
            scene.name = "main"
            scene.file_data = configparser.ConfigParser()
            scene._environment = None
            scene._cameras = []
            scene._main_camera = None
            scene.engine_scene = SimpleNamespace(
                add_camera=lambda camera: None,
                set_active_camera=lambda camera: None,
            )
            scene.script_path = ""
            scene.terrain_path = ""

            def make_actor(guid):
                return SimpleNamespace(
                    name="chair",
                    actor_type="model",
                    route="Resource/chair.obj",
                    actor_guid=guid,
                    _geometry=True,
                    get_position=lambda: [0.0, 0.0, 0.0],
                    get_rotation=lambda: [0.0, 0.0, 0.0],
                    get_scale=lambda: [1.0, 1.0, 1.0],
                    get_follow_camera=lambda: False,
                )

            scene._actors = [make_actor("actor-chair-a"), make_actor("actor-chair-b")]

            scene.save_data()

            saved = configparser.ConfigParser()
            saved.read(scene_path, encoding="utf-8")
            self.assertEqual(saved["actors"]["chair.name"], "chair")
            self.assertEqual(saved["actors"]["chair_1.name"], "chair")
            self.assertEqual(scene._build_actor_json(saved["actors"], "chair")["name"], "chair")
            self.assertEqual(scene._build_actor_json(saved["actors"], "chair_1")["name"], "chair")

    def test_actor_config_loads_display_alias_from_base_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            actor_path = Path(tmp) / "resource_name.actor"
            config = configparser.ConfigParser()
            config["base"] = {
                "name": "Display Chair",
                "path": "",
                "actor_guid": "actor-display-chair",
            }
            config["scripts"] = {"path": ""}
            with actor_path.open("w", encoding="utf-8") as handle:
                config.write(handle)

            fake_editor = SimpleNamespace(
                CoronaEngine=SimpleNamespace(
                    active_project_path=str(Path(tmp)),
                    Actor=FakeActorEngineObject,
                    ActorProfile=SimpleNamespace,
                ),
                js_call_func=lambda name, args: None,
            )

            with patch.object(actor_module, "CoronaEditor", fake_editor), \
                 patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine):
                actor = actor_module.Actor(route=str(actor_path), actor_type="actor")

            self.assertEqual(actor.name, "Display Chair")
            self.assertEqual(actor.actor_guid, "actor-display-chair")

    def test_legacy_scene_actor_without_explicit_alias_falls_back_to_ini_key(self):
        scene = scene_module.Scene.__new__(scene_module.Scene)
        actors = configparser.ConfigParser()
        actors["actors"] = {
            "legacy_chair.actor_type": "model",
            "legacy_chair.route": "Resource/chair.obj",
            "legacy_chair.geometry.position": "0.0, 0.0, 0.0",
            "legacy_chair.geometry.rotation": "0.0, 0.0, 0.0",
            "legacy_chair.geometry.scale": "1.0, 1.0, 1.0",
        }

        actor_data = scene._build_actor_json(actors["actors"], "legacy_chair")

        self.assertEqual(actor_data["name"], "legacy_chair")

    def test_actor_data_alias_overrides_model_filename_when_loading_scene_actor(self):
        with tempfile.TemporaryDirectory() as tmp:
            project_root = Path(tmp)
            model_path = project_root / "Resource" / "chair_mesh.obj"
            model_path.parent.mkdir()
            model_path.write_text("mesh", encoding="utf-8")
            fake_editor = SimpleNamespace(
                CoronaEngine=SimpleNamespace(
                    active_project_path=str(project_root),
                    Actor=FakeActorEngineObject,
                    ActorProfile=SimpleNamespace,
                ),
                js_call_func=lambda name, args: None,
            )
            parent = SimpleNamespace(route="Scene/main.scene", save_data=lambda: None)
            actor_data = {
                "name": "Display Chair",
                "actor_guid": "actor-display-chair",
                "_suppress_network_broadcast": True,
                "geometry": {
                    "position": [0.0, 0.0, 0.0],
                    "rotation": [0.0, 0.0, 0.0],
                    "scale": [1.0, 1.0, 1.0],
                },
            }

            with patch.object(actor_module, "CoronaEditor", fake_editor), \
                 patch.object(actor_module, "CoronaEngine", fake_editor.CoronaEngine), \
                 patch.object(actor_module, "Geometry", FakeGeometry), \
                 patch.object(actor_module, "Optics", FakeOptics), \
                 patch.object(actor_module, "Mechanics", FakeComponent), \
                 patch.object(actor_module, "Acoustics", FakeComponent):
                actor = actor_module.Actor(
                    route="Resource/chair_mesh.obj",
                    actor_type="model",
                    parent_scene=parent,
                    actor_data=actor_data,
                )

            self.assertEqual(actor.name, "Display Chair")
            self.assertEqual(actor.actor_guid, "actor-display-chair")


if __name__ == "__main__":
    unittest.main()

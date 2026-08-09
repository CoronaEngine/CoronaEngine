from __future__ import annotations

from pathlib import Path
import json
import builtins
import sys
import tempfile
import types
import unittest
from unittest import mock

PROJECT_ROOT = Path(__file__).resolve().parents[7]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))
EDITOR_ROOT = PROJECT_ROOT / "editor"
if str(EDITOR_ROOT) not in sys.path:
    sys.path.insert(0, str(EDITOR_ROOT))
AI_TOOL_ROOT = PROJECT_ROOT / "editor" / "plugins" / "AITool"
if str(AI_TOOL_ROOT) not in sys.path:
    sys.path.insert(0, str(AI_TOOL_ROOT))

from editor.plugins.AITool.cai_extensions.mcp.tools.model_import_tools import (
    _actor_identity_from_native_result,
    _active_project_path,
    _build_import_model_tool,
    _build_import_environment_component_tool,
    _create_native_editor_actor,
    _pick_model_file,
)
from editor.plugins.AITool.services.agent_runtime.environment_primitives import (
    build_environment_primitive,
)
from api import editor_api


class ModelImportToolsTests(unittest.TestCase):
    def test_model_import_tools_routes_actor_deletion_through_scene_tools_adapter(self) -> None:
        source = (Path(__file__).resolve().parents[1] / "model_import_tools.py").read_text(
            encoding="utf-8"
        )

        self.assertNotIn('getattr(CoronaEngine, "remove_editor_actor"', source)
        self.assertIn("get_scene_tools_adapter", source)

    def test_model_import_tools_do_not_import_editor_runtime_container(self) -> None:
        source = (Path(__file__).resolve().parents[1] / "model_import_tools.py").read_text(
            encoding="utf-8"
        )

        self.assertNotIn(
            "from CoronaCore.core.corona_editor import CoronaEditor",
            source,
        )

    def test_model_import_tools_use_the_canonical_editor_api_package(self) -> None:
        source = (Path(__file__).resolve().parents[1] / "model_import_tools.py").read_text(
            encoding="utf-8"
        )

        self.assertNotIn("from CoronaCore.core import editor_api", source)
        self.assertIn("from api import editor_api", source)

    def test_relative_model_paths_use_editor_project_context(self) -> None:
        with mock.patch(
            "api.editor_api.get_active_project_path",
            return_value="D:/Projects/Example",
        ):
            self.assertEqual(_active_project_path(), "D:/Projects/Example")

    def test_relative_model_paths_use_editor_adapter_fallback(self) -> None:
        fake_settings = types.SimpleNamespace(active_project_path="")
        fake_module = types.SimpleNamespace(settings_manager=fake_settings)
        with mock.patch.dict("sys.modules", {"config.settings": fake_module}), mock.patch(
            "api.editor_api.get_active_project_path",
            return_value="D:/Projects/Legacy",
        ):
            self.assertEqual(_active_project_path(), "D:/Projects/Legacy")

    def test_model_import_preserves_runtime_asset_identity(self) -> None:
        calls = []
        tool = _build_import_model_tool(scene_manager=None)

        def fake_create(**kwargs):
            calls.append(kwargs)
            return {
                "status": "success",
                "scene": kwargs["scene_name"],
                "actor": {
                    "actor_guid": "native-chair-guid",
                    "name": kwargs["actor_data"]["actor_name"],
                    "geometry": kwargs["actor_data"]["geometry"],
                },
            }

        with tempfile.TemporaryDirectory() as tmp:
            model_path = Path(tmp) / "chair.glb"
            model_path.write_bytes(b"glb")
            with mock.patch(
                "editor.plugins.AITool.cai_extensions.mcp.tools.model_import_tools._create_native_editor_actor",
                side_effect=fake_create,
            ):
                raw = tool.func(
                    model_path=str(model_path),
                    actor_name="chair",
                    object_id="batch-01-object-01",
                    asset_id="asset-content-stable",
                    model_ref="model-ref-stable",
                    actor_guid="runtime-actor-stable-guid",
                    entity_id="entity-chair-stable",
                    entity_version=4,
                    source_plan_id="plan-chair",
                    source_batch_id="batch-chair",
                    skip_if_exists=True,
                    scene_name="Scene/test.scene",
                )

        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["actor_data"]["asset_id"], "asset-content-stable")
        self.assertEqual(calls[0]["actor_data"]["model_ref"], "model-ref-stable")
        self.assertEqual(calls[0]["actor_data"]["actor_guid"], "runtime-actor-stable-guid")
        self.assertEqual(calls[0]["actor_data"]["entity_id"], "entity-chair-stable")
        self.assertEqual(calls[0]["actor_data"]["actor_version"], 4)
        self.assertEqual(calls[0]["actor_data"]["source_plan_id"], "plan-chair")
        self.assertEqual(calls[0]["actor_data"]["source_batch_id"], "batch-chair")
        self.assertTrue(calls[0]["actor_data"]["skip_if_exists"])
        self.assertIn("asset-content-stable", str(raw))
        self.assertIn("model-ref-stable", str(raw))

    def test_room_environment_primitives_are_visible_model_assets(self) -> None:
        room_box = build_environment_primitive(
            component_type="room_box",
            component_id="room-box-test",
            scale=[7.0, 3.2, 6.5],
        )
        room_floor = build_environment_primitive(
            component_type="room_floor",
            component_id="room-floor-test",
            scale=[7.0, 0.05, 6.5],
        )

        self.assertTrue(Path(room_box.model_path).is_file())
        self.assertTrue(Path(room_floor.model_path).is_file())
        self.assertEqual(room_box.position, [0.0, 1.6, 0.0])
        self.assertEqual(room_floor.position, [0.0, 0.025, 0.0])
        self.assertEqual(room_box.semantic_role, "indoor_enclosure")
        self.assertEqual(room_floor.semantic_role, "walkable_floor")
        self.assertIn("usemtl wall", Path(room_box.model_path).read_text(encoding="utf-8"))
        self.assertIn("usemtl floor", Path(room_floor.model_path).read_text(encoding="utf-8"))

        terrain = build_environment_primitive(
            component_type="terrain",
            component_id="terrain-test",
            scale=[14.0, 0.05, 12.0],
        )
        boundary = build_environment_primitive(
            component_type="boundary",
            component_id="boundary-test",
            scale=[14.0, 0.8, 12.0],
        )
        transition = build_environment_primitive(
            component_type="transition_zone",
            component_id="transition-zone-test",
            scale=[4.0, 0.05, 4.0],
        )
        self.assertTrue(Path(terrain.model_path).is_file())
        self.assertTrue(Path(boundary.model_path).is_file())
        self.assertTrue(Path(transition.model_path).is_file())
        self.assertEqual(terrain.semantic_role, "walkable_terrain")
        self.assertEqual(boundary.semantic_role, "scene_boundary")
        self.assertEqual(transition.semantic_role, "indoor_outdoor_transition")
        self.assertEqual(transition.position, [0.0, 0.025, 0.0])

    def test_room_environment_import_never_uses_audio_actor_type(self) -> None:
        calls = []
        tool = _build_import_environment_component_tool(scene_manager=None)

        def fake_create(**kwargs):
            calls.append(kwargs)
            actor_data = kwargs["actor_data"]
            return {
                "status": "success",
                "scene": kwargs["scene_name"],
                "actor": {
                    "actor_guid": "room-native-guid",
                    "name": actor_data["name"],
                    "geometry": actor_data["geometry"],
                },
            }

        with mock.patch(
            "editor.plugins.AITool.cai_extensions.mcp.tools.model_import_tools._create_native_editor_actor",
            side_effect=fake_create,
        ):
            tool.func(
                component_id="component-room-box",
                name="room_box",
                component_type="room_box",
                actor_guid="runtime-room-box-guid",
                entity_id="entity-room-box",
                entity_version=2,
                source_plan_id="plan-room",
                source_batch_id="batch-environment",
                scale=[6.5, 3.0, 6.0],
                scene_name="Scene/test.scene",
            )
            tool.func(
                component_id="component-transition-zone",
                name="transition_zone",
                component_type="transition_zone",
                scale=[4.0, 0.05, 4.0],
                scene_name="Scene/test.scene",
            )

        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0]["actor_type"], "model")
        self.assertTrue(calls[0]["source_path"].endswith("room_box.obj"))
        self.assertEqual(calls[0]["actor_data"]["entity_type"], "environment")
        self.assertEqual(calls[0]["actor_data"]["semantic_role"], "indoor_enclosure")
        self.assertEqual(calls[0]["actor_data"]["actor_guid"], "runtime-room-box-guid")
        self.assertEqual(calls[0]["actor_data"]["entity_id"], "entity-room-box")
        self.assertEqual(calls[0]["actor_data"]["actor_version"], 2)
        self.assertEqual(calls[0]["actor_data"]["source_plan_id"], "plan-room")
        self.assertEqual(calls[0]["actor_data"]["source_batch_id"], "batch-environment")
        self.assertEqual(calls[1]["actor_type"], "model")
        self.assertTrue(calls[1]["source_path"].endswith("transition_zone.obj"))
        self.assertEqual(
            calls[1]["actor_data"]["semantic_role"],
            "indoor_outdoor_transition",
        )

    def test_room_environment_import_uses_plugins_namespace_when_editor_namespace_is_unavailable(self) -> None:
        calls = []
        tool = _build_import_environment_component_tool(scene_manager=None)
        real_import = builtins.__import__

        def guarded_import(name, *args, **kwargs):
            if name == "editor.plugins.AITool.services.agent_runtime.environment_primitives":
                raise ModuleNotFoundError(name)
            return real_import(name, *args, **kwargs)

        def fake_create(**kwargs):
            calls.append(kwargs)
            return {
                "status": "success",
                "scene": kwargs["scene_name"],
                "actor": {
                    "actor_guid": "room-plugin-namespace-guid",
                    "name": kwargs["actor_data"]["name"],
                    "geometry": kwargs["actor_data"]["geometry"],
                },
            }

        with mock.patch(
            "builtins.__import__",
            side_effect=guarded_import,
        ), mock.patch(
            "editor.plugins.AITool.cai_extensions.mcp.tools.model_import_tools._create_native_editor_actor",
            side_effect=fake_create,
        ):
            raw = tool.func(
                component_id="component-room-floor",
                name="room_floor",
                component_type="room_floor",
                scale=[6.5, 0.05, 6.0],
                scene_name="Scene/test.scene",
            )

        self.assertEqual(len(calls), 1)
        self.assertTrue(calls[0]["source_path"].endswith("room_floor.obj"))
        envelope = json.loads(raw) if isinstance(raw, str) else raw
        self.assertEqual(envelope["error_code"], 0)
        self.assertEqual(envelope["status_info"], "success")

    def test_pick_model_file_finds_nested_mesh(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "hunyuan_model"
            nested = root / "mesh" / "export"
            nested.mkdir(parents=True)
            mesh = nested / "base.glb"
            mesh.write_bytes(b"glb")

            self.assertEqual(_pick_model_file(str(root)), str(mesh))

    def test_hunyuan_download_retries_incomplete_stream_and_keeps_only_complete_file(self) -> None:
        import httpx
        from Quasar.ai_modules.three_d_generate.tools import model_tools

        class FakeResponse:
            def __init__(self, chunks, *, failure=None):
                self.headers = {"content-length": "10"}
                self._chunks = list(chunks)
                self._failure = failure

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

            @staticmethod
            def raise_for_status():
                return None

            def iter_bytes(self, chunk_size=65536):  # noqa: ARG002
                yield from self._chunks
                if self._failure is not None:
                    raise self._failure

        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(
            model_tools.httpx,
            "stream",
            side_effect=[
                FakeResponse(
                    [b"12345"],
                    failure=httpx.RemoteProtocolError("peer closed incomplete body"),
                ),
                FakeResponse([b"1234567890"]),
            ],
        ), mock.patch.object(model_tools.time, "sleep", return_value=None):
            downloaded = model_tools._download_url_to_dir(
                "https://example.invalid/model.glb",
                tmp,
                preferred_filename="base.glb",
                max_attempts=2,
            )

            self.assertEqual(Path(downloaded).read_bytes(), b"1234567890")
            self.assertFalse(Path(f"{downloaded}.tmp").exists())

    def test_hunyuan_poll_retries_transient_transport_error_without_resubmitting_job(self) -> None:
        import httpx
        import threading
        from Quasar.ai_modules.three_d_generate.tools.client_hunyuan3d import Hunyuan3DClient

        client = object.__new__(Hunyuan3DClient)
        client._generation_semaphore = threading.Semaphore(1)
        client.submit_job = mock.Mock(return_value="job-existing")
        client.query_job = mock.Mock(side_effect=[
            httpx.ReadError("temporary polling disconnect"),
            {
                "Response": {
                    "Status": "DONE",
                    "ResultFile3Ds": [
                        {"Type": "OBJ", "Url": "https://example.invalid/model.zip"},
                    ],
                },
            },
        ])

        result = client.run_to_download_urls(
            prompt="bookshelf",
            poll_interval=0.0,
            poll_timeout=10.0,
        )

        self.assertEqual(result["task_uuid"], "job-existing")
        self.assertEqual(client.submit_job.call_count, 1)
        self.assertEqual(client.query_job.call_count, 2)

    def test_actor_identity_from_native_result_accepts_supported_fields(self) -> None:
        self.assertEqual(
            _actor_identity_from_native_result({"actor": {"actor_guid": "guid-1"}}),
            "guid-1",
        )
        self.assertEqual(
            _actor_identity_from_native_result({"actor_data": {"native_actor_id": "native-2"}}),
            "native-2",
        )
        self.assertEqual(
            _actor_identity_from_native_result({"entity_id": "entity-3"}),
            "entity-3",
        )

    def test_actor_identity_from_native_result_rejects_missing_identity(self) -> None:
        self.assertEqual(
            _actor_identity_from_native_result({"status": "success", "actor": {"name": "box"}}),
            "",
        )

    def test_create_native_editor_actor_prefers_manifest_api(self) -> None:
        manifest_calls = []

        class FakeSceneTools:
            @staticmethod
            def create_actor(scene_name, source_path, actor_type, actor_data):
                manifest_calls.append((scene_name, source_path, actor_type, actor_data))
                return {
                    "status": "success",
                    "actor": {"actor_guid": "manifest-guid"},
                }

        class LegacyEngine:
            @staticmethod
            def create_editor_actor(*_args):
                raise AssertionError("legacy actor API must not be called")

        actor_data = {"position": [1.0, 2.0, 3.0]}
        with mock.patch.object(
            editor_api, "get_scene_tools_adapter", return_value=FakeSceneTools()
        ):
            result = _create_native_editor_actor(
                scene_name="Scene/test.scene",
                source_path="models/chest.glb",
                actor_type="model",
                actor_data=actor_data,
                legacy_engine=LegacyEngine,
            )

        self.assertEqual(result["actor"]["actor_guid"], "manifest-guid")
        self.assertEqual(len(manifest_calls), 1)
        self.assertIs(manifest_calls[0][3], actor_data)

    def test_create_native_editor_actor_falls_back_when_manifest_method_is_missing(self) -> None:
        legacy_calls = []

        class LegacyEngine:
            @staticmethod
            def create_editor_actor(scene_name, source_path, actor_type, actor_data_json):
                legacy_calls.append((scene_name, source_path, actor_type, actor_data_json))
                return {"status": "success", "actor": {"actor_guid": "old-build-guid"}}

        legacy_adapter = types.SimpleNamespace(
            create_actor=lambda scene, source, kind, data: LegacyEngine.create_editor_actor(
                scene, source, kind, json.dumps(data, ensure_ascii=False)
            )
        )
        with mock.patch.object(
            editor_api, "get_scene_tools_adapter", return_value=legacy_adapter
        ):
            result = _create_native_editor_actor(
                scene_name="Scene/test.scene",
                source_path="models/chest.glb",
                actor_type="model",
                actor_data={"position": [0.0, 0.0, 0.0]},
                legacy_engine=LegacyEngine,
            )

        self.assertEqual(result["actor"]["actor_guid"], "old-build-guid")
        self.assertEqual(len(legacy_calls), 1)

    def test_create_native_editor_actor_falls_back_for_old_engine(self) -> None:
        legacy_calls = []
        fake_corona_engine_module = types.ModuleType("CoronaEngine")

        class LegacyEngine:
            @staticmethod
            def create_editor_actor(scene_name, source_path, actor_type, actor_data_json):
                legacy_calls.append(
                    (scene_name, source_path, actor_type, json.loads(actor_data_json))
                )
                return json.dumps(
                    {"status": "success", "actor": {"actor_guid": "legacy-guid"}}
                )

        with mock.patch.dict(
            "sys.modules", {"CoronaEngine": fake_corona_engine_module}
        ):
            result = _create_native_editor_actor(
                scene_name="Scene/test.scene",
                source_path="models/chest.glb",
                actor_type="model",
                actor_data={"scale": [1.2, 1.2, 1.2]},
                legacy_engine=LegacyEngine,
            )

        self.assertEqual(result["actor"]["actor_guid"], "legacy-guid")
        self.assertEqual(legacy_calls[0][3]["scale"], [1.2, 1.2, 1.2])


if __name__ == "__main__":
    unittest.main()

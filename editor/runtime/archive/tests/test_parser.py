import tempfile
import unittest
from pathlib import Path


from runtime.archive.errors import ArchiveParseError
from runtime.archive.parser import parse_archive


class ArchiveParserTests(unittest.TestCase):
    def test_legacy_scene_owner_reuses_archive_parser_without_actor_instantiation(self):
        scene_source = (
            Path(__file__).resolve().parents[3]
            / "runtime"
            / "legacy"
            / "entities"
            / "scene.py"
        ).read_text(encoding="utf-8")
        self.assertIn("parse_archive(data_path)", scene_source)
        self.assertIn("self.archive_snapshot", scene_source)
        self.assertNotIn("self._actors.append(Actor", scene_source)

    def test_portable_scene_is_normalized_to_snapshot_v1(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            asset = root / "assets" / "chair.obj"
            asset.parent.mkdir()
            asset.write_text("o chair\n", encoding="utf-8")
            (root / "scene.ini").write_text(
                "\n".join(
                    [
                        "[format]",
                        "type = corona_scene_folder",
                        "version = 1",
                        "[scene]",
                        "name = Portable",
                        "core_version = 1.2",
                        "[actors]",
                        "chair.name = Chair",
                        "chair.actor_guid = chair-guid",
                        "chair.actor_type = actor",
                        "chair.route = assets/chair.obj",
                        "chair.runtime.entity_id = entity-chair",
                        "chair.runtime.asset_id = asset-chair",
                        "chair.runtime.model_ref = catalog/chair",
                        "chair.runtime.entity_type = prop",
                        "chair.runtime.semantic_role = seating",
                        "chair.runtime.source_plan_id = plan-1",
                        "chair.runtime.source_batch_id = batch-1",
                        "chair.runtime.source_scene_version = 3",
                        "chair.runtime.actor_version = 7",
                        "chair.geometry.position = 1, 2, 3",
                        "chair.optics.diffuse = 0.2, 0.3, 0.4",
                        "chair.optics.metallic = 0.5",
                        "chair.material.texture = assets/chair.png",
                        "chair.mechanics.physics_enabled = false",
                        "chair.mechanics.collision_type = mesh",
                        "[camera]",
                        "count = 1",
                        "active_id = main-camera",
                        "camera0.id = main-camera",
                        "camera0.name = Main",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            snapshot = parse_archive(str(root))

            self.assertEqual(snapshot["schema_version"], 1)
            self.assertEqual(snapshot["archive_type"], "portable_scene")
            self.assertEqual(snapshot["project_root"], str(root.resolve()))
            self.assertEqual(snapshot["scene"]["route"], "scene.ini")
            self.assertEqual(snapshot["scene"]["active_camera_id"], "main-camera")
            actor = snapshot["scene"]["actors"][0]
            self.assertEqual(actor["actor_guid"], "chair-guid")
            self.assertEqual(actor["runtime_entity_id"], "entity-chair")
            self.assertEqual(actor["asset_id"], "asset-chair")
            self.assertEqual(actor["model_ref"], "catalog/chair")
            self.assertEqual(actor["entity_type"], "prop")
            self.assertEqual(actor["semantic_role"], "seating")
            self.assertEqual(actor["source_plan_id"], "plan-1")
            self.assertEqual(actor["source_batch_id"], "batch-1")
            self.assertEqual(actor["source_scene_version"], 3)
            self.assertEqual(actor["actor_version"], 7)
            self.assertEqual(actor["transform"]["position"], [1.0, 2.0, 3.0])
            self.assertEqual(actor["transform"]["scale"], [1.0, 1.0, 1.0])
            self.assertEqual(actor["asset_path"], str(asset.resolve()))
            self.assertTrue(actor["visible"])
            self.assertFalse(actor["mechanics"]["physics_enabled"])
            self.assertEqual(actor["mechanics"]["collision_type"], "mesh")
            self.assertEqual(actor["optics"]["diffuse"], [0.2, 0.3, 0.4])
            self.assertEqual(actor["optics"]["metallic"], 0.5)
            self.assertEqual(actor["optics"]["texture"], "assets/chair.png")

    def test_portable_scene_rejects_resource_path_outside_project(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "scene"
            root.mkdir()
            (root / "scene.ini").write_text(
                "\n".join(
                    [
                        "[format]",
                        "type = corona_scene_folder",
                        "version = 1",
                        "[scene]",
                        "name = Unsafe",
                        "[actors]",
                        "bad.actor_guid = bad-guid",
                        "bad.route = ../outside.obj",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            with self.assertRaises(ArchiveParseError) as raised:
                parse_archive(str(root))

            self.assertEqual(raised.exception.code, "RESOURCE_PATH_OUTSIDE_PROJECT")
            self.assertFalse(raised.exception.recoverable)

    def test_duplicate_actor_guid_is_an_unrecoverable_archive_error(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "scene.ini").write_text(
                "\n".join(
                    [
                        "[format]",
                        "type = corona_scene_folder",
                        "version = 1",
                        "[scene]",
                        "name = Duplicate",
                        "[actors]",
                        "first.actor_guid = same-guid",
                        "first.route = first.obj",
                        "second.actor_guid = same-guid",
                        "second.route = second.obj",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            with self.assertRaises(ArchiveParseError) as raised:
                parse_archive(str(root))

            self.assertEqual(raised.exception.code, "DUPLICATE_ACTOR_GUID")

    def test_legacy_project_resolves_entrance_scene_and_reports_missing_resource(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            scene_dir = root / "Scene"
            scene_dir.mkdir()
            (root / "project.ini").write_text(
                "[Project]\nname = Legacy\nentrance_scene = Scene/default.scene\n",
                encoding="utf-8",
            )
            (scene_dir / "default.scene").write_text(
                "\n".join(
                    [
                        "[base]",
                        "name = Default",
                        "[actors]",
                        "missing.name = Missing",
                        "missing.actor_guid = missing-guid",
                        "missing.route = Model/missing.fbx",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            snapshot = parse_archive(str(root / "project.ini"))

            self.assertEqual(snapshot["archive_type"], "legacy_project")
            self.assertTrue(snapshot["project"]["legacy"])
            self.assertEqual(snapshot["scene"]["route"], "Scene/default.scene")
            self.assertEqual(snapshot["scene"]["name"], "Default")
            self.assertEqual(snapshot["diagnostics"][0]["code"], "RESOURCE_NOT_FOUND")
            self.assertTrue(snapshot["diagnostics"][0]["recoverable"])
            self.assertEqual(snapshot["diagnostics"][0]["actor_guid"], "missing-guid")

    def test_non_finite_transform_is_rejected_instead_of_defaulted(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "scene.ini").write_text(
                "\n".join(
                    [
                        "[format]",
                        "type = corona_scene_folder",
                        "version = 1",
                        "[scene]",
                        "name = Invalid",
                        "[actors]",
                        "bad.actor_guid = bad-guid",
                        "bad.route = bad.obj",
                        "bad.geometry.position = nan, 0, 0",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            with self.assertRaises(ArchiveParseError) as raised:
                parse_archive(str(root))

            self.assertEqual(raised.exception.code, "INVALID_VECTOR")

    def test_invalid_camera_number_is_reported_as_structured_parse_error(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "scene.ini").write_text(
                "\n".join(
                    [
                        "[format]",
                        "type = corona_scene_folder",
                        "version = 1",
                        "[scene]",
                        "name = InvalidCamera",
                        "[camera]",
                        "camera0.fov = nope",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            with self.assertRaises(ArchiveParseError) as raised:
                parse_archive(str(root))

            self.assertEqual(raised.exception.code, "INVALID_ARCHIVE_VALUE")

    def test_missing_texture_is_a_recoverable_actor_diagnostic(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            model = root / "model.obj"
            model.write_text("o model\n", encoding="utf-8")
            (root / "scene.ini").write_text(
                "\n".join(
                    [
                        "[format]",
                        "type = corona_scene_folder",
                        "version = 1",
                        "[scene]",
                        "name = MissingTexture",
                        "[actors]",
                        "model.actor_guid = model-guid",
                        "model.route = model.obj",
                        "model.material.texture = textures/missing.png",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            snapshot = parse_archive(str(root))

            diagnostic = snapshot["diagnostics"][0]
            self.assertEqual(diagnostic["code"], "ATTACHMENT_RESOURCE_NOT_FOUND")
            self.assertEqual(diagnostic["actor_guid"], "model-guid")
            self.assertTrue(diagnostic["recoverable"])


if __name__ == "__main__":
    unittest.main()

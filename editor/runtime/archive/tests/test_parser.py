import configparser
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


from runtime.archive.errors import ArchiveParseError
from runtime.archive.parser import parse_archive


class ArchiveParserTests(unittest.TestCase):
    def test_accumulation_migrates_legacy_mode_with_explicit_flag_priority(self):
        cases = [
            ("path_tracing", None, "true", "path_tracing", False, True),
            ("restir", "on", "false", "restir", True, False),
            ("restir", "off", "true", "restir", False, True),
            ("progressive_path_tracing", None, None, "path_tracing", True, False),
            ("PROGRESSIVE-PATH-TRACING", "false", "true", "path_tracing", False, True),
            ("progressive_path_tracing", "0", None, "path_tracing", False, False),
            ("svgf", None, None, "path_tracing", False, True),
            ("svgf", "yes", "false", "path_tracing", True, False),
        ]
        with tempfile.TemporaryDirectory() as temp_dir:
            scene = Path(temp_dir) / "scene.ini"
            for mode, accumulation, denoise, expected_mode, expected_accumulation, expected_denoise in cases:
                with self.subTest(mode=mode, accumulation=accumulation, denoise=denoise):
                    content = (
                        "[format]\ntype = corona_scene_folder\nversion = 1\n"
                        "[scene]\nname = Independent preferences\n"
                        "[camera]\ncount = 1\n"
                        f"camera0.vision_render_mode = {mode}\n"
                    )
                    if accumulation is not None:
                        content += f"camera0.vision_accumulation = {accumulation}\n"
                    if denoise is not None:
                        content += f"camera0.vision_denoise = {denoise}\n"
                    scene.write_text(content, encoding="utf-8")
                    camera = parse_archive(str(scene))["scene"]["cameras"][0]
                    self.assertEqual(camera["vision_render_mode"], expected_mode)
                    self.assertIs(camera.get("vision_accumulation"), expected_accumulation)
                    self.assertIs(camera["vision_denoise"], expected_denoise)

    def test_accumulation_defaults_to_false_for_default_camera(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            scene = Path(temp_dir) / "scene.ini"
            scene.write_text(
                "[format]\ntype = corona_scene_folder\nversion = 1\n[scene]\nname = Default\n",
                encoding="utf-8",
            )
            camera = parse_archive(str(scene))["scene"]["cameras"][0]
            self.assertIs(camera.get("vision_accumulation"), False)

    def test_vision_denoise_is_an_independent_boolean_and_migrates_legacy_svgf(self):
        cases = [
            ("path_tracing", None, "path_tracing", False),
            ("restir", "true", "restir", True),
            ("restir", "false", "restir", False),
            ("svgf", None, "path_tracing", True),
            ("VISION-SVGF", None, "path_tracing", True),
            ("svgf", "false", "path_tracing", False),
            ("svgf", "0", "path_tracing", False),
            ("path_tracing", "on", "path_tracing", True),
        ]
        with tempfile.TemporaryDirectory() as temp_dir:
            scene = Path(temp_dir) / "scene.ini"
            for mode, denoise, expected_mode, expected_enabled in cases:
                with self.subTest(mode=mode, denoise=denoise):
                    content = (
                        "[format]\ntype = corona_scene_folder\nversion = 1\n"
                        "[scene]\nname = Denoise preferences\n"
                        "[camera]\ncount = 1\n"
                        f"camera0.vision_render_mode = {mode}\n"
                    )
                    if denoise is not None:
                        content += f"camera0.vision_denoise = {denoise}\n"
                    scene.write_text(content, encoding="utf-8")
                    camera = parse_archive(str(scene))["scene"]["cameras"][0]
                    self.assertEqual(camera["vision_render_mode"], expected_mode)
                    self.assertIs(camera["vision_denoise"], expected_enabled)

    def test_blank_active_camera_uses_first_camera_but_unknown_id_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            scene = root / "scene.ini"
            content = (
                "[format]\ntype = corona_scene_folder\nversion = 1\n"
                "[scene]\nname = Vision camera\n"
                "[camera]\ncount = 1\nactive_id = \ncamera0.name = view1\n"
            )
            scene.write_text(content, encoding="utf-8")
            snapshot = parse_archive(str(root))
            self.assertEqual(snapshot["scene"]["active_camera_id"], "scene.ini#camera0")
            self.assertEqual(snapshot["scene"]["cameras"][0]["id"], "scene.ini#camera0")
            scene.write_text(content.replace("active_id = ", "active_id = unknown"), encoding="utf-8")
            with self.assertRaises(ArchiveParseError) as raised:
                parse_archive(str(root))
            self.assertEqual(raised.exception.code, "ACTIVE_CAMERA_NOT_FOUND")

    def test_actor_fields_are_resolved_once_instead_of_rescanning_for_each_actor(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            actor_count = 100
            lines = ["[format]", "type = corona_scene_folder", "version = 1",
                     "[scene]", "name = Large scene", "[actors]"]
            for index in range(actor_count):
                lines.extend([f"actor{index}.name = Item {index}",
                              f"actor{index}.actor_guid = guid-{index}",
                              f"actor{index}.custom.label = %(actor{index}.name)s"])
            (root / "scene.ini").write_text("\n".join(lines), encoding="utf-8")
            original_get = configparser.ConfigParser.get
            actor_reads = 0

            def count_get(parser, section, option, *args, **kwargs):
                nonlocal actor_reads
                if section == "actors":
                    actor_reads += 1
                return original_get(parser, section, option, *args, **kwargs)

            with patch.object(configparser.ConfigParser, "get", count_get):
                snapshot = parse_archive(str(root))
            actors = snapshot["scene"]["actors"]
            self.assertEqual(len(actors), actor_count)
            for actor in actors:
                index = actor["actor_guid"].removeprefix("guid-")
                self.assertEqual(actor["persisted_fields"], {
                    f"actor{index}.name": f"Item {index}",
                    f"actor{index}.actor_guid": f"guid-{index}",
                    f"actor{index}.custom.label": f"Item {index}",
                })
            self.assertLess(actor_reads, actor_count * 100,
                            "archive parsing must not resolve every actor's fields for each actor")

    def test_legacy_scene_owner_reuses_archive_parser_without_actor_instantiation(self):
        self.assertFalse(
            (Path(__file__).resolve().parents[3] / "runtime" / "legacy" / "entities" / "scene.py").is_file()
        )

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

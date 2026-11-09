import hashlib
import tempfile
import json
import unittest
from pathlib import Path
from unittest.mock import patch

from plugins.ProjectArchive.main import ProjectArchive


class ProjectArchiveServiceTests(unittest.TestCase):
    def test_parse_returns_invalid_archive_with_structured_diagnostic(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            scene = Path(temp_dir)
            (scene / "scene.ini").write_text(
                "[format]\ntype = corona_scene_folder\nversion = 99\n[scene]\nname = Future\n",
                encoding="utf-8",
            )

            result = ProjectArchive.parse({"path": str(scene)})

            self.assertFalse(result["ok"])
            self.assertEqual(result["status"], "invalid_archive")
            self.assertEqual(result["diagnostics"][0]["code"], "UNSUPPORTED_ARCHIVE_VERSION")

    def test_parse_requires_user_decision_when_snapshot_has_recoverable_errors(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            scene = Path(temp_dir)
            (scene / "scene.ini").write_text(
                "\n".join(
                    [
                        "[format]",
                        "type = corona_scene_folder",
                        "version = 1",
                        "[scene]",
                        "name = Missing",
                        "[actors]",
                        "missing.actor_guid = missing-guid",
                        "missing.route = Assets/Models/missing/missing.fbx",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            route = "Assets/Models/missing/missing.fbx"
            (scene / "assets.manifest.json").write_text(json.dumps({
                "format": "corona_scene_assets", "version": 1, "bundles": [{
                    "main": route, "files": [{"path": route, "size": 1}], "dependencies": [],
                }],
            }), encoding="utf-8")
            result = ProjectArchive.parse({"path": str(scene), "load_policy": "prompt"})

            self.assertTrue(result["ok"])
            self.assertEqual(result["status"], "decision_required")
            self.assertEqual(result["snapshot"]["schema_version"], 1)
            self.assertEqual(result["diagnostics"][0]["code"], "RESOURCE_NOT_FOUND")

            degraded = ProjectArchive.parse(
                {"path": str(scene), "load_policy": "degraded"}
            )
            self.assertEqual(degraded["status"], "ready_degraded")

    def test_mitsuba_material_approximations_keep_diagnostics_without_confirmation(self):
        materials = {
            "twosided": '<bsdf type="twosided"><bsdf type="diffuse"/></bsdf>',
            "mask": '<bsdf type="mask"><float name="opacity" value="0.4"/>'
                    '<bsdf type="diffuse"/></bsdf>',
            "thindielectric": '<bsdf type="thindielectric"/>',
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "mesh.obj").write_text(
                "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", encoding="utf-8"
            )
            for name, material in materials.items():
                scene = root / f"{name}.xml"
                scene.write_text(
                    '<scene version="3.0.0"><shape type="obj">'
                    '<string name="filename" value="mesh.obj"/>'
                    f'{material}</shape></scene>', encoding="utf-8"
                )
                for policy in ("prompt", "degraded"):
                    with self.subTest(material=name, policy=policy):
                        result = ProjectArchive.parse({"path": str(scene), "load_policy": policy})
                        self.assertTrue(result["ok"])
                        self.assertEqual(result["status"], "ready")
                        self.assertEqual(len(result["snapshot"]["scene"]["actors"]), 1)
                        self.assertTrue(result["diagnostics"])
                        self.assertEqual(result["diagnostics"], result["snapshot"]["diagnostics"])
                        for diagnostic in result["diagnostics"]:
                            self.assertIn(diagnostic["code"], {
                                "MATERIAL_APPROXIMATION", "MATERIAL_FEATURE_DEFERRED"
                            })
                            self.assertIs(diagnostic["requires_confirmation"], False)

    def test_missing_texture_warning_still_requires_confirmation(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            model = "Assets/Models/probe/model.obj"
            texture = "Assets/Images/probe/missing.png"
            (root / model).parent.mkdir(parents=True)
            content = b"o model\n"
            (root / model).write_bytes(content)
            (root / "assets.manifest.json").write_text(json.dumps({
                "format": "corona_scene_assets", "version": 1, "bundles": [
                    {"main": model, "sha256": hashlib.sha256(b"model.obj\0" + content).hexdigest(),
                     "files": [{"path": model, "size": len(content), "sha256": hashlib.sha256(content).hexdigest()}],
                     "dependencies": []},
                    {"main": texture, "files": [{"path": texture, "size": 1}], "dependencies": []},
                ],
            }))
            (root / "scene.ini").write_text(
                "[format]\ntype=corona_scene_folder\nversion=1\n"
                f"[actors]\nmodel.route={model}\nmodel.material.texture={texture}\n",
                encoding="utf-8",
            )
            for policy, status in (("prompt", "decision_required"), ("degraded", "ready_degraded")):
                with self.subTest(policy=policy):
                    result = ProjectArchive.parse({"path": str(root), "load_policy": policy})
                    self.assertEqual(result["status"], status)
                    self.assertEqual(result["diagnostics"][0]["severity"], "warning")
                    self.assertEqual(result["diagnostics"][0]["code"], "ATTACHMENT_RESOURCE_NOT_FOUND")

    def test_unsupported_shapes_still_require_confirmation_with_material_warnings(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "mesh.obj").write_text(
                "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", encoding="utf-8"
            )
            for mixed in (False, True):
                material_shape = (
                    '<shape type="obj"><string name="filename" value="mesh.obj"/>'
                    '<bsdf type="thindielectric"/></shape>' if mixed else ""
                )
                scene = root / "scene.xml"
                scene.write_text(
                    f'<scene version="3.0.0">{material_shape}<shape type="hair"/></scene>',
                    encoding="utf-8",
                )
                for policy, status in (("prompt", "decision_required"), ("degraded", "ready_degraded")):
                    with self.subTest(mixed=mixed, policy=policy):
                        result = ProjectArchive.parse({"path": str(scene), "load_policy": policy})
                        self.assertEqual(result["status"], status)
                        diagnostics = {item["code"]: item for item in result["diagnostics"]}
                        self.assertIs(diagnostics["UNSUPPORTED_SHAPE"]["requires_confirmation"], True)
                        if mixed:
                            self.assertIs(diagnostics["MATERIAL_APPROXIMATION"]["requires_confirmation"], False)

    def test_error_or_missing_severity_requires_confirmation_even_when_marked_otherwise(self):
        for diagnostic in (
            {"code": "RESOURCE_NOT_FOUND", "severity": "error", "requires_confirmation": False},
            {"code": "RESOURCE_NOT_FOUND", "requires_confirmation": False},
        ):
            with patch("plugins.ProjectArchive.main.parse_archive", return_value={"diagnostics": [diagnostic]}):
                for policy, status in (("prompt", "decision_required"), ("degraded", "ready_degraded")):
                    with self.subTest(diagnostic=diagnostic, policy=policy):
                        result = ProjectArchive.parse({"path": "unused", "load_policy": policy})
                        self.assertEqual(result["status"], status)
                        self.assertEqual(result["diagnostics"], [diagnostic])


if __name__ == "__main__":
    unittest.main()

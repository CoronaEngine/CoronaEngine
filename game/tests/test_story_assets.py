"""Shipped story assets/deployment checks, without loading any native engine."""
import contextlib
import importlib.util
import io
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock
from urllib.parse import unquote, urlparse
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
ART = ROOT / "game/art/models"
NS = {"c": "http://www.collada.org/2005/11/COLLADASchema"}


class StoryAssetTests(unittest.TestCase):
    def test_role_sources_are_animated_and_all_external_images_are_local(self):
        node = shutil.which("node")
        if not node:
            self.skipTest("Node.js is required to read the shared character configuration")
        result = subprocess.run([node, "--input-type=module", "-e",
            "import {STORY_CHARACTERS} from './game/frontend/storyCharacters.mjs'; "
            "console.log(JSON.stringify(STORY_CHARACTERS));"], cwd=ROOT,
            capture_output=True, text=True, encoding="utf-8", check=True, timeout=15)
        characters = json.loads(result.stdout)
        self.assertEqual([c["role"] for c in characters], ["player", "boss", "merchant", "prophet"])
        self.assertEqual(len({c["guid"] for c in characters}), 4)
        for character in characters:
            with self.subTest(role=character["role"]):
                model = (ART / character["asset"]).resolve()
                self.assertTrue(model.is_relative_to(ART))
                self.assertTrue(model.is_file(), str(model))
                document = ET.parse(model).getroot()
                self.assertTrue(document.findall("c:library_animations/c:animation", NS))
                for image in document.findall("c:library_images/c:image/c:init_from", NS):
                    image_uri = urlparse(image.text)
                    self.assertEqual(image_uri.scheme, "", image.text)
                    texture = (model.parent / unquote(image_uri.path)).resolve()
                    self.assertTrue(texture.is_relative_to(model.parent), str(texture))
                    self.assertTrue(texture.is_file(), str(texture))

    def test_models_and_supplied_sidecars_are_unchanged_copies_of_engine_assets(self):
        sources = {
            "player": ROOT / "assets/Maria WProp J J Ong",
            "boss": ROOT / "assets/dragon/dae",
            "merchant": ROOT / "assets/Maw J Laygo",
            "prophet": ROOT / "assets/model",
        }
        for role, source_root in sources.items():
            copied = [path for path in (ART / role).rglob("*") if path.is_file()]
            self.assertTrue(copied, role)
            self.assertEqual(len([path for path in copied if path.suffix == ".dae"]), 1)
            for path in copied:
                with self.subTest(file=str(path.relative_to(ART))):
                    original = source_root / path.relative_to(ART / role)
                    self.assertTrue(original.is_file(), str(original))
                    with path.open("rb") as current, original.open("rb") as source:
                        self.assertEqual(hashlib.file_digest(current, "sha256").digest(),
                                         hashlib.file_digest(source, "sha256").digest())

    def test_frontend_deployment_contains_every_module_and_cleans_stale_modules(self):
        spec = importlib.util.spec_from_file_location("story_asset_deployment", ROOT / "tools/build/editor_copy_and_build.py")
        deployment = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(deployment)
        with tempfile.TemporaryDirectory(prefix="story assets ") as temporary:
            root = Path(temporary)
            stale = root / "game/frontend/stale.mjs"
            stale.parent.mkdir(parents=True)
            stale.write_text("obsolete", encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()):
                deployment.copy_game_frontend(root / "CabbageEditor", ROOT)
            self.assertFalse(stale.exists())
            for source in (ROOT / "game/frontend").glob("*.mjs"):
                self.assertEqual(source.read_bytes(), (root / "game/frontend" / source.name).read_bytes())
            self.assertFalse((root / "game/tests").exists())


    def test_art_deployment_preserves_models_textures_and_attribution(self):
        spec = importlib.util.spec_from_file_location("story_art_deployment", ROOT / "tools/build/editor_copy_and_build.py")
        deployment = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(deployment)
        with tempfile.TemporaryDirectory(prefix="剧情美术 assets ") as temporary:
            root = Path(temporary)
            dest = root / "CabbageEditor"
            with contextlib.redirect_stdout(io.StringIO()):
                deployment.copy_game_art(dest, ROOT)
                deployment.copy_game_art(dest, ROOT)  # Incremental deployment is repeatable.
            expected = {p.relative_to(ROOT / "game/art") for p in (ROOT / "game/art").rglob("*") if p.is_file()}
            actual = {p.relative_to(root / "game/art") for p in (root / "game/art").rglob("*") if p.is_file()}
            self.assertEqual(actual, expected)
            for relative in expected:
                self.assertEqual((ROOT / "game/art" / relative).read_bytes(),
                                 (root / "game/art" / relative).read_bytes(), str(relative))
            self.assertFalse((root / "assets").exists())
            self.assertFalse((root / "game/tests").exists())
            with self.assertRaises(FileNotFoundError):
                deployment.copy_game_art(dest, root / "missing repository")
            with mock.patch.object(deployment.shutil, "copytree", side_effect=OSError("disk full")):
                with self.assertRaisesRegex(OSError, "disk full"), contextlib.redirect_stdout(io.StringIO()):
                    deployment.copy_game_art(dest, ROOT)


if __name__ == "__main__":
    unittest.main()

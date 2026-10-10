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

    # Every game-owned art file must be an untouched copy of an engine asset. `cube` is a
    # hand-authored primitive, so it is registered here too: the invariant is "copied, not
    # edited", not "downloaded". `fragment` was missing from this table before.
    ART_SOURCES = {
        "player": ROOT / "assets/Maria WProp J J Ong",
        "boss": ROOT / "assets/dragon/dae",
        "merchant": ROOT / "assets/Maw J Laygo",
        "prophet": ROOT / "assets/model",
        "fragment": ROOT / "assets/model",
        "cube": ROOT / "assets/model",
    }
    # A character folder carries one rigged model; props are plain meshes with no animation.
    CHARACTER_ART_ROLES = ("player", "boss", "merchant", "prophet")

    def test_models_and_supplied_sidecars_are_unchanged_copies_of_engine_assets(self):
        for role, source_root in self.ART_SOURCES.items():
            copied = [path for path in (ART / role).rglob("*") if path.is_file()]
            self.assertTrue(copied, role)
            if role in self.CHARACTER_ART_ROLES:
                self.assertEqual(len([path for path in copied if path.suffix == ".dae"]), 1, role)
            for path in copied:
                with self.subTest(file=str(path.relative_to(ART))):
                    original = source_root / path.relative_to(ART / role)
                    self.assertTrue(original.is_file(), str(original))
                    with path.open("rb") as current, original.open("rb") as source:
                        self.assertEqual(hashlib.file_digest(current, "sha256").digest(),
                                         hashlib.file_digest(source, "sha256").digest())
        # Every folder must be registered above, so a new one cannot quietly opt out.
        self.assertEqual({folder.name for folder in ART.iterdir() if folder.is_dir()},
                         set(self.ART_SOURCES))

    def test_cube_placement_and_fragment_catalogue_match_their_data_files(self):
        """The JSON is authoritative; the runtime projections must mirror it exactly."""
        node = shutil.which("node")
        if not node:
            self.skipTest("Node.js is required to read the runtime projections")
        result = subprocess.run([node, "--input-type=module", "-e",
            "import {CUBE_PLACEMENT} from './game/frontend/storyCube.mjs'; "
            "import {FRAGMENTS, WORLD_RULE_VERSION} from './game/frontend/storyWorldRules.mjs'; "
            "console.log(JSON.stringify({cube: CUBE_PLACEMENT, fragments: FRAGMENTS, "
            "ruleVersion: WORLD_RULE_VERSION}));"],
            cwd=ROOT, capture_output=True, text=True, encoding="utf-8", check=True, timeout=15)
        projection = json.loads(result.stdout)

        cube = json.loads((ROOT / "game/data/prophet.json").read_text(encoding="utf-8"))["cube"]
        for key, value in projection["cube"].items():
            self.assertIn(key, cube, key)
            self.assertEqual(value, cube[key], key)

        catalogue = json.loads((ROOT / "game/data/fragments.json").read_text(encoding="utf-8"))
        self.assertEqual(projection["ruleVersion"], catalogue["version"])
        self.assertEqual(len(projection["fragments"]), len(catalogue["fragments"]))
        for projected, declared in zip(projection["fragments"], catalogue["fragments"]):
            for key in ("id", "name", "initial", "description", "rule"):
                self.assertEqual(projected[key], declared[key], f'{declared["id"]}.{key}')
        # The requirement's initial fragment must be the left/right sway, excluding the player.
        self.assertEqual([f["id"] for f in catalogue["fragments"] if f["initial"]], ["sway"])
        rule = catalogue["fragments"][0]["rule"]
        self.assertEqual(rule["type"], "sway")
        self.assertEqual(rule["axis"], "x")
        self.assertIn("player", rule["excludeRoles"])
        self.assertGreater(rule["amplitude"], 0)
        self.assertGreater(rule["periodMs"], 0)

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


    def _prophet_data(self) -> dict:
        return json.loads((ROOT / "game/data/prophet.json").read_text(encoding="utf-8"))

    def _prophet_projection(self) -> dict:
        node = shutil.which("node")
        if not node:
            self.skipTest("Node.js is required to read the story dialogue projection")
        result = subprocess.run([node, "--input-type=module", "-e",
            "import {PROPHET_SCRIPT, PROPHET_PLACEMENT, describeProphetContract} "
            "from './game/frontend/storyDialogue.mjs'; "
            "console.log(JSON.stringify({script: PROPHET_SCRIPT, placement: PROPHET_PLACEMENT, "
            "contract: describeProphetContract()}));"],
            cwd=ROOT, capture_output=True, text=True, encoding="utf-8", check=True, timeout=15)
        return json.loads(result.stdout)

    def test_prophet_content_is_shared_between_the_data_file_and_the_runtime_projection(self):
        """game/data/prophet.json is authoritative; storyDialogue.mjs must mirror it exactly."""
        projection = self._prophet_projection()
        data = self._prophet_data()

        script, projected = data["script"], projection["script"]
        self.assertEqual(projected["lines"], script["lines"])
        self.assertEqual(projected["fragments"], script["fragments"])
        for key in ("id", "name", "title", "hint", "closing"):
            self.assertEqual(projected[key], script[key], key)
        # `lines` is the panel's wire format; `fragments` is the authoritative per-beat form.
        # Keeping them equal here is what makes a script rewrite a pure data edit.
        self.assertEqual(script["lines"], [fragment["text"] for fragment in script["fragments"]])
        self.assertEqual(len({fragment["id"] for fragment in script["fragments"]}),
                         len(script["fragments"]))
        self.assertTrue(all(fragment["id"] for fragment in script["fragments"]))

        # Every parameter the placer reads must come from here, not a second copy in code.
        placement = data["placement"]
        for key in ("model", "asset", "aheadDistance", "height", "yawOffset", "rotationPolicy"):
            self.assertEqual(projection["placement"][key], placement[key], key)

        contract, interaction = data["contract"], data["contract"]["interaction"]
        summary = projection["contract"]
        self.assertEqual(summary["version"], contract["version"])
        self.assertEqual(summary["worlds"], contract["worlds"])
        for key in ("key", "range", "prompt", "closeKey", "advanceKeys"):
            self.assertEqual(summary[key], interaction[key], key)
        self.assertEqual(summary["fragmentIds"], [f["id"] for f in script["fragments"]])
        self.assertEqual(summary["lineCount"], len(script["lines"]))

    def test_prophet_placement_matches_the_character_table(self):
        """storyCharacters.mjs must read the model and height the data file declares."""
        node = shutil.which("node")
        if not node:
            self.skipTest("Node.js is required to read the shared character configuration")
        result = subprocess.run([node, "--input-type=module", "-e",
            "import {STORY_CHARACTERS} from './game/frontend/storyCharacters.mjs'; "
            "console.log(JSON.stringify(STORY_CHARACTERS));"], cwd=ROOT,
            capture_output=True, text=True, encoding="utf-8", check=True, timeout=15)
        prophet = next(c for c in json.loads(result.stdout) if c["role"] == "prophet")
        placement = self._prophet_data()["placement"]
        self.assertEqual(prophet["asset"], placement["asset"])
        self.assertEqual(prophet["height"], placement["height"])

    def test_game_data_deployment_carries_the_story_content(self):
        spec = importlib.util.spec_from_file_location("story_data_deployment", ROOT / "tools/build/editor_copy_and_build.py")
        deployment = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(deployment)
        with tempfile.TemporaryDirectory(prefix="story data ") as temporary:
            root = Path(temporary)
            dest = root / "CabbageEditor"
            with contextlib.redirect_stdout(io.StringIO()):
                deployment.copy_game_data(dest, ROOT)
                deployment.copy_game_data(dest, ROOT)  # Incremental deployment is repeatable.
            expected = {path.relative_to(ROOT / "game/data")
                        for path in (ROOT / "game/data").rglob("*") if path.is_file()}
            actual = {path.relative_to(root / "game/data")
                      for path in (root / "game/data").rglob("*") if path.is_file()}
            self.assertEqual(actual, expected)
            self.assertIn(Path("prophet.json"), expected)
            for relative in expected:
                self.assertEqual((ROOT / "game/data" / relative).read_bytes(),
                                 (root / "game/data" / relative).read_bytes(), str(relative))
            with self.assertRaises(FileNotFoundError):
                deployment.copy_game_data(dest, root / "missing repository")


if __name__ == "__main__":
    unittest.main()

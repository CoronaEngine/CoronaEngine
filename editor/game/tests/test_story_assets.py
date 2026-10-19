"""Shipped story assets/deployment checks, without loading any native engine."""
import contextlib
import importlib.util
import io
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest import mock
from urllib.parse import unquote, urlparse
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
ART = ROOT / "editor/game/art/models"
NS = {"c": "http://www.collada.org/2005/11/COLLADASchema"}


class StoryAssetTests(unittest.TestCase):
    @staticmethod
    def _glb_json(document: Path) -> tuple[dict, int]:
        """The JSON chunk of a GLB and the byte offset of its BIN chunk.

        A `.glb` is a 12-byte header (magic, version, total) followed by length-prefixed
        chunks; JSON is 0x4E4F534A and BIN is 0x004E4942. The engine parses the whole file
        through Assimp, so this reads the same document it does, without a native build.
        """
        with document.open("rb") as stream:
            magic, version, total = struct.unpack("<4sII", stream.read(12))
            assert magic == b"glTF" and version == 2 and total == document.stat().st_size
            header = None
            while stream.tell() < total:
                length, kind = struct.unpack("<II", stream.read(8))
                if kind == 0x4E4F534A:
                    header = json.loads(stream.read(length).decode("utf-8"))
                    continue
                if kind == 0x004E4942 and header is not None:
                    return header, stream.tell()
                stream.seek(length, 1)
        raise AssertionError(f"{document} has no JSON chunk followed by a BIN chunk")

    def _assert_glb_character_art(self, model: Path):
        """A model the engine can animate, whose textures travel inside the file.

        Characters are imported through the same Assimp path as the map, so the
        requirements are the ones that path actually needs: a skin plus at least one
        animation clip (`read_animations` runs only when the scene has skinning, and the
        engine loops `animations[0]`), and every image embedded rather than referenced —
        the deployed CabbageEditor has no sidecar directory to resolve them against.
        """
        header, _binary = self._glb_json(model)
        self.assertTrue(header.get("animations"), "GLB character art must carry an animation")
        self.assertTrue(header.get("skins"), "GLB character art must be skinned")
        for index, image in enumerate(header.get("images", [])):
            self.assertNotIn("uri", image, f"image[{index}] must be embedded, not referenced")
            self.assertIn("bufferView", image, f"image[{index}] must carry a bufferView")
        names = [animation.get("name") for animation in header["animations"]]
        self.assertTrue(any(name for name in names), f"{model.name} needs a named clip: {names}")

    def _assert_dae_character_art(self, model: Path):
        """COLLADA: the rig carries animations and every image resolves next to the model."""
        document = ET.parse(model).getroot()
        self.assertTrue(document.findall("c:library_animations/c:animation", NS))
        for image in document.findall("c:library_images/c:image/c:init_from", NS):
            image_uri = urlparse(image.text)
            self.assertEqual(image_uri.scheme, "", image.text)
            texture = (model.parent / unquote(image_uri.path)).resolve()
            self.assertTrue(texture.is_relative_to(model.parent), str(texture))
            self.assertTrue(texture.is_file(), str(texture))

    def test_role_sources_are_animated_and_all_external_images_are_local(self):
        node = shutil.which("node")
        if not node:
            self.skipTest("Node.js is required to read the shared character configuration")
        result = subprocess.run([node, "--input-type=module", "-e",
            "import {STORY_CHARACTERS} from './editor/game/frontend/storyCharacters.mjs'; "
            "console.log(JSON.stringify(STORY_CHARACTERS));"], cwd=ROOT,
            capture_output=True, text=True, encoding="utf-8", check=True, timeout=15)
        characters = json.loads(result.stdout)
        roles = [c["role"] for c in characters]
        # The four inhabitants keep their positions: other modules address them by index.
        self.assertEqual(roles[:4], ["player", "boss", "merchant", "prophet"])
        self.assertEqual(len({c["guid"] for c in characters}), len(characters),
                         "every character needs its own stable actor identity")
        monsters = [c for c in characters if c["role"].startswith("monster-")]
        # A roster, not a specific size: the count is a content decision. What must hold is
        # that the main world has creatures at all...
        self.assertTrue(monsters, "the main world should carry a roster")
        # ... and that the shipped art and the roster agree exactly, both ways round. This is
        # what catches art on disk that no roster row uses (dead weight) and a roster row whose
        # art never reached the repo — neither of which the per-role loop below can see.
        shipped = sorted(p.name for p in (ART / "monster").rglob("*") if p.is_file())
        declared = sorted(Path(c["asset"]).name for c in monsters)
        self.assertEqual(declared, shipped, "monster art on disk and the roster must match exactly")
        for name in shipped:
            self.assertEqual(Path(name).suffix.lower(), ".glb",
                             f"{name}: creatures ship as a self-contained GLB")
        # `em001_04` is the boss. Listing it here too would put two roles on one asset and make
        # a future boss swap reach into this roster.
        self.assertNotIn("em001_04", [c["asset"] for c in monsters],
                         "the boss model must not be reused as a main-world monster")
        # A creature must not be rotated: an earlier boss build bought height by rolling a model
        # onto another axis and shipped it lying on its side. Identity is the rule.
        for monster in monsters:
            with self.subTest(role=monster["role"]):
                self.assertEqual(list(monster["rotation"]), [0, 0, 0],
                                 "main-world creatures keep their authored orientation")
                self.assertNotIn("height", monster,
                                 "creatures scale by `size`; `height` is for upright NPCs")
                self.assertGreater(monster["size"], 0)
                self.assertTrue(abs(monster["x"]) <= 32.4 and abs(monster["z"]) <= 45.0,
                                "the site must be inside the map footprint (58.7 x 82.0 m)")
        # The boss's generation marker lives on the table entry itself, because the replacement
        # path reads it straight off the character. The player and the prophet carry theirs as
        # module constants (`PLAYER_MODEL_REF` / `PROPHET_MODEL_REF`), which is why only one
        # entry declares `model_ref` here — this pins that, so a future role cannot quietly
        # adopt a second, competing source for the same key.
        markers = [c["model_ref"] for c in characters if c.get("model_ref")]
        self.assertEqual(markers, ["story.boss.em001.v1"],
                         "the boss alone declares its generation on the entry")
        for character in characters:
            with self.subTest(role=character["role"]):
                model = (ART / character["asset"]).resolve()
                self.assertTrue(model.is_relative_to(ART))
                self.assertTrue(model.is_file(), str(model))
                if model.suffix.lower() == ".glb":
                    self._assert_glb_character_art(model)
                else:
                    self._assert_dae_character_art(model)

    # Every game-owned art file must be an untouched copy of an engine asset. `cube` is a
    # hand-authored primitive, so it is registered here too: the invariant is "copied, not
    # edited", not "downloaded". `fragment` was missing from this table before.
    # `scene` is the story world map: its measurements are re-derived from the shipped GLB
    # below and compared with the placement the page actually runs.
    ART_SOURCES = {
        # The player is a supplied, self-contained GLB (`player_walk.glb`, a Max Biped rig with
        # two embedded PNGs), so there is no engine-asset directory to diff it against — the
        # `_assert_glb_character_art` requirements stand in for that comparison, the same way
        # they do for the boss. The previous player art is retired below, not deleted.
        "player": None,
        # The boss is the em001 lineage, shipped as one self-contained GLB: its textures
        # are the embedded PNGs Blender wrote, so there is no source directory to diff
        # against — `_assert_glb_character_art` is what stands in for that comparison.
        "boss": None,
        # The main-world roster ships as self-contained GLBs too: embedded textures, no
        # sidecars, and the same requirements checked by `_assert_glb_character_art`.
        "monster": None,
        "merchant": ROOT / "assets/Maw J Laygo",
        "prophet": ROOT / "assets/model",
        "fragment": ROOT / "assets/model",
        "cube": ROOT / "assets/model",
        "scene": ROOT / "assets/model",
    }
    # A character folder carries one rigged model; props are plain meshes with no animation.
    CHARACTER_ART_ROLES = ("player", "boss", "merchant", "prophet")
    # Retired art that is still on disk, so a swap can be reverted without a rebuild. Each entry
    # is an exact path, so this exemption can never widen into "this character folder is
    # unchecked"; delete the file (and its entry) to reclaim the space.
    #   boss/Dragon 2.5_dae.dae        replaced by boss/Em001_04.glb        (~9 MB)
    #   player/Maria WProp J J Ong.dae replaced by player/player_walk.glb   (~2.7 MB)
    # Maria's sidecar textures stay unchecked now that the player folder has no engine-asset
    # source; they are unreferenced, so deleting `player/textures/` reclaims a further ~14 MB.
    RETIRED_CHARACTER_ART = frozenset({
        "boss/Dragon 2.5_dae.dae",
        "player/Maria WProp J J Ong.dae",
    })

    # The story world map. `scale` is the map's largest world axis in metres: the engine
    # normalises every static model into a unit cube, so that is exactly the `geometry.scale`
    # an actor needs to appear at the shipped world size. `courtyardY` is where the walkable
    # courtyard sits in the model file, and the placement removes it so the surface lands on
    # the engine floor y = 0 that the story uses for feet.
    # Both numbers live in the `map` block of game/data/prophet.json; the tests below
    # re-derive them from the shipped GLB so neither side can drift away from the art.
    # The map was re-exported with a flat floor under this new name ("平地版" = flat-ground
    # edition), so the shipped route changed with the geometry, and the engine keys an imported
    # bundle by content hash. A world that already carries the map is *not* re-imported by the
    # engine's `skip_if_exists` repair branch (it returns before the route is read —
    # cef_editor_native_api_handlers.cpp:3410-3424 — and only rewrites the transform), so the
    # asset generation is bumped in the data instead: `modelRef` is the migration key, and the
    # page deletes and re-creates an actor whose generation is stale.
    # `test_a_stale_map_actor_is_replaced_not_repaired` guards that this stays wired.
    STORY_WORLD_MAP = "scene/01_山门神殿_平地版.glb"
    STORY_WORLD_MAP_SPAWN_SITES = (
        ("player", 0, 0), ("boss", 2, 5.5), ("merchant", -30, -15), ("prophet", 30, -15),
    )
    # "Open ground" for the 8 m-wide boss: the radius the dragon actually occupies (its wings
    # reach 4 m either side of its origin) and the height a surface may reach inside it before
    # the dragon is standing inside a wall, a step or a roof rather than on the courtyard.
    SITE_RADIUS = 4.0
    SITE_ROOF = 0.35

    def _map_placement(self) -> dict:
        """The authoritative `map` block of game/data/prophet.json."""
        return json.loads((ROOT / "editor/game/data/prophet.json").read_text(encoding="utf-8"))["map"]

    def _runtime_projection(self, spawn_sites: tuple = ()) -> dict:
        """The numbers the story frontend actually runs on, read from its modules through Node.

        The page mirrors `game/data/prophet.json` in code, so the only way to check the runtime
        rather than the data file is to import the module and ask it. `spawn_sites` (world x/z)
        are also run through the runtime ground lookup `heightAt`, which is the function
        `playerController` and `characterTransform` call — so the ground line the game uses is
        measured here rather than re-derived.
        """
        node = shutil.which("node")
        if not node:
            self.skipTest("Node.js is required to read the runtime projections")
        sites = json.dumps([{"role": role, "x": x, "z": z} for role, x, z in spawn_sites])
        script = (
            "import fs from 'node:fs'; "
            "import {CUBE_PLACEMENT} from './editor/game/frontend/storyCube.mjs'; "
            "import {MAP_PLACEMENT, MAP_MAX_AXIS, MAP_SURFACE_LIFT, mapOriginTransform} from './editor/game/frontend/storyMap.mjs'; "
            "import {MODEL_TO_WORLD, setHeightGrid, heightAt, heightGridInfo} from './editor/game/frontend/storyMapHeight.mjs'; "
            "import {FRAGMENTS, WORLD_RULE_VERSION} from './editor/game/frontend/storyWorldRules.mjs'; "
            "setHeightGrid(JSON.parse(fs.readFileSync('./editor/game/data/story-map-height.json', 'utf8'))); "
            f"const sites = {sites}; "
            "console.log(JSON.stringify({cube: CUBE_PLACEMENT, map: MAP_PLACEMENT, "
            "mapTransform: mapOriginTransform(), maxAxis: MAP_MAX_AXIS, modelToWorld: MODEL_TO_WORLD, "
            "surfaceLift: MAP_SURFACE_LIFT, fragments: FRAGMENTS, "
            "ruleVersion: WORLD_RULE_VERSION, grid: heightGridInfo(), "
            "spawnGround: sites.map(site => ({...site, y: heightAt(site.x, site.z)}))}));")
        result = subprocess.run([node, "--input-type=module", "-e", script],
                                cwd=ROOT, capture_output=True, text=True, encoding="utf-8", check=True, timeout=30)
        return json.loads(result.stdout)

    @staticmethod
    def _glb_json(document: Path) -> tuple[dict, int]:
        """Return the JSON chunk and the byte offset of the BIN payload that follows it."""
        with document.open("rb") as stream:
            magic, version, total = struct.unpack("<4sII", stream.read(12))
            assert magic == b"glTF" and version == 2 and total == document.stat().st_size
            header = None
            while stream.tell() < total:
                length, kind = struct.unpack("<II", stream.read(8))
                if kind == 0x4E4F534A:
                    header = json.loads(stream.read(length).decode("utf-8"))
                    continue
                if kind == 0x004E4942 and header is not None:
                    return header, stream.tell()
                stream.seek(length, 1)
        raise AssertionError(f"{document} has no JSON chunk followed by a BIN chunk")

    @staticmethod
    def _mat4_multiply(lhs: list[float], rhs: list[float]) -> list[float]:
        """Column-major 4x4 product, matching glTF's matrix layout."""
        return [sum(lhs[k * 4 + row] * rhs[column * 4 + k] for k in range(4))
                for column in range(4) for row in range(4)]

    @classmethod
    def _node_matrices(cls, header: dict) -> dict[int, list[float]]:
        """World matrix of every node that carries a mesh, as the engine sees it."""
        identity = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]
        nodes = header.get("nodes", [])
        local: dict[int, list[float]] = {}
        for index, node in enumerate(nodes):
            if "matrix" in node:
                raw = node["matrix"]
                local[index] = [raw[column * 4 + row] for column in range(4) for row in range(4)]
                continue
            tx, ty, tz = node.get("translation", [0.0, 0.0, 0.0])
            qx, qy, qz, qw = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
            sx, sy, sz = node.get("scale", [1.0, 1.0, 1.0])
            rotation = [1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy + qz * qw), 2 * (qx * qz - qy * qw), 0,
                        2 * (qx * qy - qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz + qx * qw), 0,
                        2 * (qx * qz + qy * qw), 2 * (qy * qz - qx * qw), 1 - 2 * (qx * qx + qy * qy), 0,
                        0, 0, 0, 1]
            # rotation is column-major: column c owns axis c, so scale column c by s[c].
            scales = (sx, sy, sz)
            column = [rotation[column_index * 4 + row] * (scales[column_index] if row < 3 and column_index < 3 else 1.0)
                      for column_index in range(4) for row in range(4)]
            column[12], column[13], column[14] = tx, ty, tz
            local[index] = column

        world: dict[int, list[float]] = {}
        scene = header.get("scenes", [{}])[header.get("scene", 0)]
        stack = [(index, identity) for index in scene.get("nodes", [])]
        while stack:
            index, parent = stack.pop()
            matrix = cls._mat4_multiply(parent, local[index])
            if "mesh" in nodes[index]:
                world[index] = matrix
            stack.extend((child, matrix) for child in nodes[index].get("children", []))
        return world

    @classmethod
    def _map_world_points(cls, document: Path) -> list[tuple[float, float, float]]:
        """Every vertex of the map in world space, through its full node hierarchy.

        The engine builds each static instance transform from the accumulated node
        matrix, so this is the geometry the collision mesh is actually built from.
        """
        header, binary = cls._glb_json(document)
        matrices = cls._node_matrices(header)
        mesh_matrices: dict[int, list[float]] = {}
        for index, node in enumerate(header.get("nodes", [])):
            if "mesh" in node and index in matrices:
                mesh_matrices[node["mesh"]] = matrices[index]
        points: list[tuple[float, float, float]] = []
        with document.open("rb") as stream:
            for mesh_index, mesh in enumerate(header.get("meshes", [])):
                matrix = mesh_matrices.get(mesh_index)
                for primitive in mesh.get("primitives", []):
                    accessor = header["accessors"][primitive["attributes"]["POSITION"]]
                    view = header["bufferViews"][accessor["bufferView"]]
                    stride = view.get("byteStride") or 12
                    start = binary + view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
                    stream.seek(start)
                    payload = stream.read((accessor["count"] - 1) * stride + 12)
                    for offset in range(0, len(payload), stride):
                        x, y, z = struct.unpack_from("<3f", payload, offset)
                        if matrix is None:
                            points.append((x, y, z))
                            continue
                        points.append(tuple(
                            matrix[0 * 4 + row] * x + matrix[4 + row] * y
                            + matrix[8 + row] * z + matrix[12 + row]
                            for row in range(3)))
        return points

    def test_story_world_map_measurements_match_what_the_runtime_places(self):
        """The map ships with the world, so its size and the line it is aligned to are facts
        the placement writes down: re-derive both from the shipped model here.

        The placement is `game/frontend/storyMap.mjs`, whose two measurements are the two
        numbers in the `map` block of `game/data/prophet.json`; this is what stops either
        side from drifting away from the model that is actually deployed.
        """
        placement = self._map_placement()
        projection = self._runtime_projection()
        document = ART / self.STORY_WORLD_MAP
        self.assertTrue(document.is_file(), str(document))
        copied = ROOT / "assets/model" / document.name
        self.assertTrue(copied.is_file(), str(copied))
        with document.open("rb") as current, copied.open("rb") as source:
            self.assertEqual(hashlib.file_digest(current, "sha256").digest(),
                             hashlib.file_digest(source, "sha256").digest())

        # The engine normalises every static model into a unit cube, so `geometry.scale`
        # must equal the model's largest world extent in metres for it to appear life-size.
        points = self._map_world_points(document)
        self.assertTrue(points, "the shipped map has no geometry")
        extent = [max(point[axis] for point in points) - min(point[axis] for point in points)
                  for axis in range(3)]
        # `scale` is the map's world depth in metres (z is this model's largest axis). It is
        # deliberately NOT "life size": at 1680 m the third-person camera sits inside the
        # building and the walkable area is far outside the engine's 50 m preload radius. What
        # still has to hold is the courtyard alignment below, plus the four spawn sites landing
        # on the map: the engine puts the model AABB centre on the world origin, so
        # `(high - centre) * scale / max_axis` is exactly `extent / 2 * scale / max_axis` and this
        # one half-width guard covers all four of them. The stronger statement — that there is
        # *surface* under each site — is measured through the runtime ground lookup in
        # `test_the_baked_height_grid_covers_the_shipped_map`.
        self.assertGreater(placement["scale"], 0)
        self.assertLessEqual(placement["scale"], 2000)
        self.assertGreaterEqual(placement["scale"] * extent[0] / max(extent) / 2, 30,
                                "地图世界半宽必须容得下站在 ±30 m 的先知与商人")

        # The engine floor is y = 0 and the story puts character feet there. Two facts have to
        # hold: the courtyard really is `courtyardY` metres up in the model file, and the
        # position the runtime sends puts it exactly on y = 0. The engine moves the model's
        # AABB centre to the origin and scales the *largest axis* to 1
        # (`finalize_normalization_params`, parse_common.h:53), so a model point at height y
        # lands at `position.y + (y - centre_y) * scale / max_axis`, with `position` in world
        # metres (`native_actor_world_aabb` is `position + local * scale`). `scale / max_axis` is
        # therefore the one factor that turns model metres into world metres, and the map is
        # 64.8 x 14.7 x 90 m at the shipped `scale = 90` exactly because of it.
        low = [min(point[axis] for point in points) for axis in range(3)]
        high = [max(point[axis] for point in points) for axis in range(3)]
        centre_y = (low[1] + high[1]) / 2
        # The AABB mid height is a measurement too, and the placement is derived from it; a
        # drifted copy here would move the map vertically with nothing else to notice.
        self.assertAlmostEqual(centre_y, placement["centreY"], delta=0.05,
                               msg="centreY 必须是模型世界包围盒的中高")
        # The walkable courtyard: the lowest surface inside a window around the model origin,
        # which is where the story authors its spawn sites. The window is 40 model metres wide
        # because this model's flat floor is a coarse plane (the mesh is authored at 4x the
        # previous unit scale, so its vertices are ~8 m apart and the old +/-3 m window can hold
        # no vertex at all); 20 m is still small enough to stay clear of the sunken pit at
        # (77, -58) and the walls, both of which sit further out.
        window = [point for point in points if abs(point[0]) <= 20 and abs(point[2]) <= 20]
        self.assertTrue(window, "庭院取样窗口里没有顶点")
        surface_y = min(point[1] for point in window)
        self.assertAlmostEqual(surface_y, placement["courtyardY"], delta=0.05,
                               msg="courtyardY 必须是可行走庭院在模型文件里的高度")
        # The runtime's own factor has to be this one, or the map and the baked height grid are
        # read through two different frames (that was the ~5 m float: dividing by `scale`).
        max_axis = max(extent)
        self.assertAlmostEqual(projection["maxAxis"], max_axis, delta=0.01,
                               msg="MAP_MAX_AXIS 必须是模型真实的最大轴")
        self.assertAlmostEqual(projection["modelToWorld"], placement["scale"] / max_axis, delta=1e-6,
                               msg="MODEL_TO_WORLD 必须是 scale / max_axis（MAP_MAX_AXIS 只写到小数点后四位）")
        scale_factor = placement["scale"] / max_axis
        position_y = (centre_y - placement["courtyardY"]) * scale_factor
        self.assertAlmostEqual(position_y + (surface_y - centre_y) * scale_factor, 0.0, delta=0.05,
                               msg="摆位必须把可行走庭院压到引擎地面 y = 0")
        # The transform the page actually sends must be that measurement, with no hand-tuned
        # lift on top: the ground line is the baked height grid now, and a lift would only push
        # the map (and the floor the props and spawn sites are authored on) off y = 0.
        self.assertAlmostEqual(projection["mapTransform"]["position"][1], position_y, delta=0.01)
        self.assertEqual(projection["surfaceLift"], 0.0,
                         "MAP_SURFACE_LIFT 必须回到 0：它不是手感旋钮，地面由高度网格给出")
        # Writing the metre offset itself (124.09) would be off by hundreds of metres;
        # `max_axis / 2` (840 m) mistakes the map's depth for its height.
        self.assertGreater(position_y, 0)
        self.assertLess(position_y, max_axis)

    def test_the_map_actor_is_phantom_so_it_builds_no_collision_mesh(self):
        """A solid body would make the engine build a triangle octree over ~1.64M triangles
        on every world. The map is scenery: characters stand on the courtyard because it is
        aligned to y = 0 and the world floor is `[physics] floor_y = 0`.

        The map is created by the page on every entry (`game/frontend/storyMap.mjs`), not
        seeded into new worlds by native creation code, so that is where the body type lives.
        """
        source = (ROOT / "editor/game/frontend/storyMap.mjs").read_text(encoding="utf-8")
        self.assertIn("body_type: 'phantom'", source)
        self.assertNotIn("body_type: 'static'", source)
        # The map must arrive with the world the player enters, not only with a new world.
        page = (ROOT / "editor/Frontend/src/views/layout/StoryWorld.vue").read_text(encoding="utf-8")
        self.assertIn("import { ensureStoryMap } from '../../../../game/frontend/storyMap.mjs';", page)
        self.assertIn("await ensureStoryMap({", page)

    def test_a_stale_map_actor_is_replaced_not_repaired(self):
        """A swapped map asset must reach a world that already has the map.

        The engine imports a model only when the actor is created: `create_actor`'s
        `skip_if_exists` branch (cef_editor_native_api_handlers.cpp:3410-3424) returns as soon
        as it matched the guid and only rewrites position/rotation/scale/follow_camera/body_type,
        never `actor.route`. So the map's asset generation has to be a data field the page can
        compare, and a stale actor has to be deleted and created again — repairing it in place
        would leave the old bundle on screen forever, exactly the bug this fixes.
        """
        source = (ROOT / "editor/game/frontend/storyMap.mjs").read_text(encoding="utf-8")
        # The migration key exists in the data and in the projection, so it can be compared.
        story_map = self._map_placement()
        self.assertIn("modelRef", story_map)
        # The shipped placement really carries the current generation (not the pre-swap one).
        marker = story_map["modelRef"]
        self.assertIn(marker, source, "storyMap.mjs 的 MAP_MODEL_REF 必须等于 prophet.json 的 modelRef")
        self.assertGreater(int(marker.rsplit(".v", 1)[1]), 1,
                           "换了地图资产就必须升版 modelRef，否则已有世界不会重建地图")
        # The page must delete the stale actor and create the current one, in that order.
        self.assertIn("api.sceneTools.removeActor(", source,
                      "旧代次的地图必须被删除后重建，否则引擎永远不会重新导入模型")
        removal = source.index("await removeMapActor(")
        # The delete-and-recreate branch is the one that follows the removal; the creation before
        # it is the "world has no map yet" branch, so the comparison has to start after the removal.
        self.assertIn("api.sceneTools.createActor(sceneId, route, 'model'", source[removal:],
                      "删除之后必须紧接着创建，否则旧 actor 会被 skip_if_exists 复用")
        # A current-generation actor with a stale transform is still repaired in place: that path
        # must stay `update_if_exists` and must not delete anything.
        self.assertIn("update_if_exists: true", source)
        # The generation marker is documented as the thing a future asset swap must bump.
        self.assertIn("MUST bump this string", source)
        self.assertTrue(marker.endswith(".v2"), "modelRef 必须落在换图之后的代次上")
        # The pre-swap GLB must not be the shipped asset either.
        self.assertNotIn("优化版", story_map["asset"])

    def test_the_story_page_does_not_feed_a_height_grid_to_the_characters(self):
        """Ground contact is native physics; the baked grid must not creep back into placement.

        The grid used to hand every character a ground line through `getGroundY`. The engine's
        triangle contacts answer for that now, so the page keeps the authored spawn y
        (`groundYAt` is a constant null) and neither the actor placement nor the player
        controller accepts a height hook. Re-adding one half of that wiring without the other
        is precisely the half-wired state this test exists to prevent.
        """
        page = (ROOT / "editor/Frontend/src/views/layout/StoryWorld.vue").read_text(encoding="utf-8")
        self.assertIn("const groundYAt = () => null;", page)
        self.assertNotIn("loadStoryMapHeight(", page)
        self.assertNotIn("heightAt(", page)
        controller = (ROOT / "editor/game/frontend/playerController.mjs").read_text(encoding="utf-8")
        self.assertNotIn("getGroundY", controller)
        actors = (ROOT / "editor/game/frontend/storyActors.mjs").read_text(encoding="utf-8")
        self.assertNotIn("heightAt", actors)
        characters = (ROOT / "editor/game/frontend/storyCharacters.mjs").read_text(encoding="utf-8")
        self.assertNotIn("groundYAt", characters)

    def test_the_baked_height_grid_covers_the_shipped_map(self):
        """The grid is a build artifact of the model: its frame has to be that model's own.

        `game/data/story-map-height.json` is read at runtime through `storyMapHeight.mjs`; these
        numbers are what make `world = position + (model - centre) * scale / max_axis` true for it.
        The grid is also where the ground line comes from, so the flat-floor re-export is checked
        here the way the game reads it: through `heightAt` at the four story spawn sites.
        """
        document = ROOT / "editor/game/data/story-map-height.json"
        self.assertTrue(document.is_file(), str(document))
        grid = json.loads(document.read_text(encoding="utf-8"))
        self.assertEqual(grid["version"], 1)
        self.assertEqual(grid["unit"], "cm")
        nx, nz = grid["res"]
        self.assertGreaterEqual(min(nx, nz), 32, "网格太粗，走上去会是台阶")
        self.assertEqual(len(grid["heights"]), nx * nz)
        self.assertTrue(all(isinstance(value, int) and 0 <= value <= 32767 for value in grid["heights"]),
                        "格值必须是 int16 厘米（空洞/越界值会被运行时拒绝）")

        points = self._map_world_points(ART / self.STORY_WORLD_MAP)
        low = [min(point[axis] for point in points) for axis in range(3)]
        high = [max(point[axis] for point in points) for axis in range(3)]
        centre = [(low[axis] + high[axis]) / 2 for axis in range(3)]
        self.assertAlmostEqual(grid["minY"], low[1], delta=0.001,
                               msg="minY 必须是模型包围盒的最低点（格值是相对它的厘米）")
        for axis, index in ((0, 0), (0, 1), (2, 2), (2, 3)):
            expected = (low[axis] if index % 2 == 0 else high[axis]) - centre[axis]
            self.assertAlmostEqual(grid["bounds"][index], expected, delta=0.001,
                                   msg=f"bounds[{index}] 必须是模型 x/z 足迹（相对 AABB 中心）")
        # The grid is walkable *ground*, not the topmost surface. It used to store the maximum y
        # per cell, so every cell a building or a wall stands on reported the wall top / eave —
        # 261 m of roof on a map whose courtyard is at 0 — and the player's ground lookup rode it
        # onto the roof ("每靠近一个建筑物就会像爬楼梯那样人物突然飞起来"). The band the bake
        # used is the invariant: no cell may rise above it, so a cell that contains a building can
        # never lift the character. A maximum-y bake fails this by 259 m.
        band = grid["band"]
        self.assertAlmostEqual(grid["courtyardY"], self._map_placement()["courtyardY"], delta=0.001,
                               msg="烘焙用的庭院高度必须就是摆位里的 courtyardY")
        ceiling = grid["courtyardY"] + band
        worst = grid["minY"] + max(grid["heights"]) / 100
        # The 1 cm of slack over the band's upper edge is the int16 centimetre quantisation of
        # the file itself (a band vertex at 2.0019 m is stored as 200 cm and read back at most
        # 2.005 m), not a licence for a cell to sit above the band.
        self.assertLessEqual(worst, ceiling + 0.01,
                             f"网格最高采样 {worst:.4f} m 高过可行走带上沿 {ceiling:.4f} m："
                             "建筑/墙顶又变成地面了")
        # Non-vacuity: the model really does have geometry far above the band, so the assertion
        # above is a statement about the grid and not about a flat model.
        self.assertGreater(high[1], ceiling + 100.0,
                           "模型本身必须远高于可行走带，否则上面那条断言是空转的")

        # The flat floor is the point of this re-export: the four story spawn sites have to read
        # the same world height through the runtime lookup the game actually calls (`heightAt`,
        # handed to `playerController` as `getGroundY`), or the characters are back on a slope.
        # `modelToWorld` is applied inside the module, so this is the world-metre ground line,
        # not a model-frame re-derivation of it.
        projection = self._runtime_projection(self.STORY_WORLD_MAP_SPAWN_SITES)
        ground = {item["role"]: item["y"] for item in projection["spawnGround"]}
        self.assertEqual(sorted(ground), ["boss", "merchant", "player", "prophet"])
        for role, value in ground.items():
            with self.subTest(role=role):
                self.assertIsNotNone(value, f"{role} 出生点没有地面")
                self.assertAlmostEqual(value, 0.0, delta=0.05,
                                       msg=f"{role} 出生点的地面必须落在引擎地面 y = 0 上")
        spread = max(ground.values()) - min(ground.values())
        self.assertLessEqual(spread, 0.05,
                             f"四个出生点的地面必须在 5 厘米以内齐平（实测 {spread:.4f} m）")

    def test_no_map_geometry_stands_over_the_boss_spawn_site(self):
        """The boss's site has to be open ground, in the placement the page actually runs.

        The dragon is 8 m of wingspan with its belly on the ground, so "is anything above it"
        is asked with the shipped model: every map vertex inside `SITE_RADIUS` world metres of
        the site is counted, and the highest of them must stay under `SITE_ROOF`. This is the
        reading that was missing when the boss was authored at (0, 12): the temple's main hall
        front bay rises 3.088 m within 4 m of that point, so the dragon stood inside the
        building and was invisible from the courtyard.

        World x/z and world y are the engine's own placement — the model AABB centre becomes
        the origin, the largest axis becomes 1 (`MAP_MAX_AXIS`), then the actor's transform is
        applied (`native_actor_world_aabb` is `position + local * scale`), so
        `world = position + (model - centre) * scale / max_axis` per axis. The radius is
        filtered in world space: 4 world metres is 74.7 model metres on this map, not 4.

        Non-vacuity: the same 40 m window is asserted to really hold a building metres above the
        ceiling, and the old (0, 12) site is measured inside the same 4 m radius and must fail
        it — so the ceiling below is a statement about this site, not about a flat map.
        """
        placement = self._map_placement()
        projection = self._runtime_projection(self.STORY_WORLD_MAP_SPAWN_SITES)
        document = ART / self.STORY_WORLD_MAP
        points = self._map_world_points(document)
        self.assertTrue(points, "the shipped map has no geometry")
        low = [min(point[axis] for point in points) for axis in range(3)]
        high = [max(point[axis] for point in points) for axis in range(3)]
        scale_factor = projection["modelToWorld"]
        centre_x = (low[0] + high[0]) / 2
        centre_z = (low[2] + high[2]) / 2
        origin_y = projection["mapTransform"]["position"][1]
        # The engine drops each vertex's y through `centreY`, not through the z/x centre.
        centre_y = placement["centreY"]
        world = [((point[0] - centre_x) * scale_factor,
                  origin_y + (point[1] - centre_y) * scale_factor,
                  (point[2] - centre_z) * scale_factor) for point in points]
        boss = [item for item in projection["spawnGround"] if item["role"] == "boss"]
        self.assertEqual(len(boss), 1, "STORY_WORLD_MAP_SPAWN_SITES 必须正好有一个 boss 出生点")
        site_x, site_z = boss[0]["x"], boss[0]["z"]

        def highest_within(x: float, z: float, radius: float) -> float:
            limit = radius ** 2
            return max((point[1] for point in world
                        if (point[0] - x) ** 2 + (point[2] - z) ** 2 <= limit), default=float("-inf"))

        # Non-vacuity of the measurement itself: the temple really does rise far above the
        # ceiling right where the boss stands, so "clear within 4 m" is a statement about this
        # site and not about a map with nothing on it. The window is 40 world metres — the
        # courtyard is only 64.8 x 90 m, so that covers the whole forecourt and the hall.
        self.assertGreater(highest_within(site_x, site_z, 40.0), self.SITE_ROOF + 2.0,
                           f"({site_x:g}, {site_z:g}) 40 m 内必须有远高于 {self.SITE_ROOF} m 的建筑，"
                           "否则「4 m 内净空」是空转的")
        # The 4 m radius is the boss's own half-width (its 8 m `size`), and it is also the
        # edge of the flat forecourt: at 5 m the temple front is already 1.825 m high and at
        # 6 m 2.141 m, so a larger boss would stand inside the hall. Pin the measured growth
        # rate here — raising `size` past 8 must fail loudly rather than quietly bury it.
        self.assertGreater(highest_within(site_x, site_z, 5.0), 1.5,
                           "5 m 处的寺庙前檐必须真的存在，否则「4 m 内净空」量的是平地")
        self.assertGreater(highest_within(site_x, site_z, 6.0), 2.0,
                           "6 m 处的寺庙前檐必须真的存在")
        self.assertGreater(highest_within(site_x, site_z, 10.0), 3.0,
                           "10 m 处的大殿必须真的存在")
        # The measured site, not a second copy of it: this test is about the shipped coordinate.
        # The clearance below is asserted first on purpose: a site that fails it must say *that*,
        # not "the pin changed", or a moved-and-buried boss reads as a stale test constant.
        self.assertIsNotNone(boss[0]["y"], "boss 出生点没有地面")
        self.assertAlmostEqual(boss[0]["y"], placement["courtyardY"], delta=0.05,
                               msg="boss 出生点的地面必须就是可行走庭院的高度")
        roof = highest_within(site_x, site_z, self.SITE_RADIUS)
        self.assertLess(roof, self.SITE_ROOF,
                        f"({site_x:g}, {site_z:g}) 周围 {self.SITE_RADIUS:g} m 内有地图几何高达 "
                        f"{roof:.3f} m，龙会被埋在里面")
        self.assertAlmostEqual(site_x, 2.0, delta=0.001, msg="boss 出生点 x 变了就要重新量净空")
        self.assertAlmostEqual(site_z, 5.5, delta=0.001, msg="boss 出生点 z 变了就要重新量净空")
        # Non-vacuity: the same measurement at the old site has to break the ceiling.
        old_roof = highest_within(0.0, 12.0, self.SITE_RADIUS)
        self.assertGreater(old_roof, self.SITE_ROOF,
                           "旧出生点 (0, 12) 必须真的被寺庙埋住，否则这条守卫什么都没证明")

    def test_models_and_supplied_sidecars_are_unchanged_copies_of_engine_assets(self):
        for role, source_root in self.ART_SOURCES.items():
            copied = [path for path in (ART / role).rglob("*") if path.is_file()]
            self.assertTrue(copied, role)
            if role in self.CHARACTER_ART_ROLES:
                # One rigged model per character folder, in whichever format it ships.
                # Retired generations are excluded by exact path and are still hash-checked
                # by the provenance loop below, so this cannot hide unverified art.
                # `as_posix` on both sides: on Windows `str(Path)` yields `boss\\x.dae`, so a
                # plain string comparison against a forward-slash literal passes on Linux and
                # fails here — exactly the kind of platform split this test exists to avoid.
                rigs = [path for path in copied
                        if path.suffix.lower() in (".dae", ".glb", ".fbx")
                        and path.relative_to(ART).as_posix() not in self.RETIRED_CHARACTER_ART]
                self.assertEqual(len(rigs), 1, f"{role}: {[str(p) for p in rigs]}")
            if source_root is None:
                continue
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
        projection = self._runtime_projection()

        cube = json.loads((ROOT / "editor/game/data/prophet.json").read_text(encoding="utf-8"))["cube"]
        for key, value in projection["cube"].items():
            self.assertIn(key, cube, key)
            self.assertEqual(value, cube[key], key)

        # The map is declared exactly like the cube, so it is mirrored the same way: every
        # field of the projection must exist in the data file with the same value.
        story_map = self._map_placement()
        self.assertEqual(projection["map"], {key: story_map[key] for key in projection["map"]},
                         "storyMap.mjs 的 MAP_PLACEMENT 必须逐字段等于 prophet.json 的 map 块")
        for key in ("asset", "guid", "modelRef", "scale", "courtyardY", "centreY"):
            self.assertIn(key, story_map, key)
        # The transform the page sends is derived from those measurements: the courtyard's
        # offset below the AABB mid height, converted into world metres by `scale / max_axis`,
        # with `MAP_SURFACE_LIFT` at 0 (the ground line is the baked height grid, so a lift has
        # nothing left to compensate). The frame itself is re-derived from the shipped GLB above.
        transform = projection["mapTransform"]
        expected_y = (story_map["centreY"] - story_map["courtyardY"]) \
            * story_map["scale"] / projection["maxAxis"]
        self.assertAlmostEqual(transform["position"][1], expected_y, delta=0.01)
        self.assertEqual(transform["position"][0], 0)
        self.assertEqual(transform["position"][2], 0)
        self.assertEqual(transform["scale"], [story_map["scale"]] * 3)
        self.assertEqual(transform["rotation"], [0, 0, 0])
        # Two classic slips are what the value guards: taking `max_axis / 2` as the mid height
        # (840 m instead of the measured 124.09 m) puts the map in orbit, and writing the metre
        # offset itself (124.09 m) misses by more than a hundred metres. `scale / max_axis` is the
        # only factor that agrees with the 64.8 x 14.7 x 90 m world size the placement documents.
        self.assertGreater(expected_y, 0.0)
        self.assertLess(expected_y, story_map["centreY"] - story_map["courtyardY"])

        # `follow_camera` is what decides world vs UI space in the engine
        # (`render_space = follow_camera ? "ui" : "scene"`,
        # cef_editor_native_api_handlers.cpp:2785): a map that followed the camera would be
        # drawn in UI space and appear to move with the player. `body_type: static` is what
        # builds the collision mesh the characters stand on. Both live on the map's actor data.
        module = (ROOT / "editor/game/frontend/storyMap.mjs").read_text(encoding="utf-8")
        declaration = module.split("MAP_ACTOR_DATA = Object.freeze({", 1)
        self.assertEqual(len(declaration), 2, "MAP_ACTOR_DATA 必须存在，地图的 actor 数据要能检查")
        actor_data = declaration[1].split("});", 1)[0]
        self.assertIn("follow_camera: false", actor_data)
        self.assertNotIn("follow_camera: true", module)
        self.assertIn("body_type: 'phantom'", actor_data)

        catalogue = json.loads((ROOT / "editor/game/data/fragments.json").read_text(encoding="utf-8"))
        self.assertEqual(projection["ruleVersion"], catalogue["version"])
        self.assertEqual(len(projection["fragments"]), len(catalogue["fragments"]))
        for projected, declared in zip(projection["fragments"], catalogue["fragments"]):
            for key in ("id", "name", "initial", "description", "rule"):
                self.assertEqual(projected[key], declared[key], f'{declared["id"]}.{key}')
        # The requirement's initial fragment must be the left/right sway, excluding the player.
        # `spin` (the z rotation) is initial too: no reward path exists yet that could grant a
        # fragment later, so it has to be owned from the start to be installable at all.
        self.assertEqual([f["id"] for f in catalogue["fragments"] if f["initial"]], ["sway", "spin"])
        rule = catalogue["fragments"][0]["rule"]
        self.assertEqual(rule["type"], "sway")
        self.assertEqual(rule["axis"], "x")
        self.assertIn("player", rule["excludeRoles"])
        self.assertGreater(rule["amplitude"], 0)
        self.assertGreater(rule["periodMs"], 0)
        # The second fragment turns objects around z, in degrees, and excludes the player too.
        spin = catalogue["fragments"][1]["rule"]
        self.assertEqual(spin["type"], "spin")
        self.assertEqual(spin["axis"], "z")
        self.assertIn("player", spin["excludeRoles"])
        self.assertGreater(spin["amplitude"], 0)
        self.assertLessEqual(spin["amplitude"], 100.0)
        self.assertGreater(spin["periodMs"], 0)

    def test_the_runtime_reserved_rule_targets_match_the_frontend_identities(self):
        """地图与先知不能成为碎片规则的目标：运行时那份冻结表必须与前端的身份同源。

        碎片规则现在可以点名一个物体（`target` = actor guid），而运行时是纯文件系统逻辑
        （不 import 前端、不读场景），所以它自带一份「保留身份」表。表只能靠这条用例保持
        同源：前端改 guid（storyMap.MAP_GUID / storyCharacters 里 role='prophet'）而运行时
        没跟上，这里就会红；反过来也一样。
        """
        from game.runtime import world_rules

        node = shutil.which("node")
        if not node:
            self.skipTest("Node.js is required to read the shared character configuration")
        script = (
            "import {MAP_GUID} from './game/frontend/storyMap.mjs'; "
            "import {PROPHET_GUID} from './game/frontend/storyCharacters.mjs'; "
            "import {RESERVED_TARGET_GUIDS, isReservedTargetGuid} from './game/frontend/storyWorldRules.mjs'; "
            "console.log(JSON.stringify({map: MAP_GUID, prophet: PROPHET_GUID, "
            "reserved: RESERVED_TARGET_GUIDS, "
            "refusesMap: isReservedTargetGuid(MAP_GUID), "
            "refusesProphet: isReservedTargetGuid(PROPHET_GUID)}));")
        result = subprocess.run([node, "--input-type=module", "-e", script], cwd=ROOT,
                                capture_output=True, text=True, encoding="utf-8", check=True, timeout=15)
        identities = json.loads(result.stdout)
        # 前端自己先得拒绝这两个身份。
        self.assertTrue(identities["refusesMap"])
        self.assertTrue(identities["refusesProphet"])
        self.assertEqual(identities["reserved"], [identities["map"], identities["prophet"]])
        # 运行时那份表必须与前端逐项相同。
        self.assertEqual(sorted(world_rules.RESERVED_TARGETS),
                         sorted(identities["reserved"]),
                         "world_rules.RESERVED_TARGETS 必须与前端保留身份一致")
        # 地图的身份最终来自数据文件：三处（数据、前端、运行时）必须同一个 guid。
        self.assertEqual(identities["map"], self._map_placement()["guid"])
        # 一个普通物体不是保留身份（非真空：表里不是「什么都拒绝」）。
        self.assertNotIn("story.placement.0003", world_rules.RESERVED_TARGETS)

    def test_frontend_deployment_contains_every_module_and_cleans_stale_modules(self):
        spec = importlib.util.spec_from_file_location("story_asset_deployment", ROOT / "tools/build/editor_copy_and_build.py")
        deployment = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(deployment)
        with tempfile.TemporaryDirectory(prefix="story assets ") as temporary:
            root = Path(temporary)
            stale = root / "CabbageEditor/game/frontend/stale.mjs"
            stale.parent.mkdir(parents=True)
            stale.write_text("obsolete", encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()):
                deployment.copy_game_frontend(root / "CabbageEditor", ROOT)
            self.assertFalse(stale.exists())
            for source in (ROOT / "editor/game/frontend").glob("*.mjs"):
                self.assertEqual(source.read_bytes(), (root / "CabbageEditor/game/frontend" / source.name).read_bytes())
            self.assertFalse((root / "CabbageEditor/game/tests").exists())


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
            expected = {p.relative_to(ROOT / "editor/game/art") for p in (ROOT / "editor/game/art").rglob("*") if p.is_file()}
            actual = {p.relative_to(root / "CabbageEditor/game/art") for p in (root / "CabbageEditor/game/art").rglob("*") if p.is_file()}
            self.assertEqual(actual, expected)
            for relative in expected:
                self.assertEqual((ROOT / "editor/game/art" / relative).read_bytes(),
                                 (root / "CabbageEditor/game/art" / relative).read_bytes(), str(relative))
            self.assertFalse((root / "assets").exists())
            self.assertFalse((root / "CabbageEditor/game/tests").exists())
            with self.assertRaises(FileNotFoundError):
                deployment.copy_game_art(dest, root / "missing repository")
            with mock.patch.object(deployment.shutil, "copytree", side_effect=OSError("disk full")):
                with self.assertRaisesRegex(OSError, "disk full"), contextlib.redirect_stdout(io.StringIO()):
                    deployment.copy_game_art(dest, ROOT)


    def _prophet_data(self) -> dict:
        return json.loads((ROOT / "editor/game/data/prophet.json").read_text(encoding="utf-8"))

    def _prophet_projection(self) -> dict:
        node = shutil.which("node")
        if not node:
            self.skipTest("Node.js is required to read the story dialogue projection")
        result = subprocess.run([node, "--input-type=module", "-e",
            "import {PROPHET_SCRIPT, PROPHET_PLACEMENT, describeProphetContract} "
            "from './editor/game/frontend/storyDialogue.mjs'; "
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
            "import {STORY_CHARACTERS} from './editor/game/frontend/storyCharacters.mjs'; "
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
            expected = {path.relative_to(ROOT / "editor/game/data")
                        for path in (ROOT / "editor/game/data").rglob("*") if path.is_file()}
            actual = {path.relative_to(root / "CabbageEditor/game/data")
                      for path in (root / "CabbageEditor/game/data").rglob("*") if path.is_file()}
            self.assertEqual(actual, expected)
            self.assertIn(Path("prophet.json"), expected)
            for relative in expected:
                self.assertEqual((ROOT / "editor/game/data" / relative).read_bytes(),
                                 (root / "CabbageEditor/game/data" / relative).read_bytes(), str(relative))
            with self.assertRaises(FileNotFoundError):
                deployment.copy_game_data(dest, root / "missing repository")


if __name__ == "__main__":
    unittest.main()

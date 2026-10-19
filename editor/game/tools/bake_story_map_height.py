"""Bake the story map's walkable height grid into ``editor/game/data/story-map-height.json``.

The map (``editor/game/art/models/scene/01_山门神殿_平地版.glb``) ships as scenery with no collision
(``body_type: 'phantom'``): the engine builds no triangle octree for it, so nothing in the
engine can answer "how high is the ground here?". This script answers that question offline
and once: it decodes every ``POSITION`` accessor of the GLB, rasterises the ~2M vertices onto a
256x256 grid over the model's x/z footprint, fills the empty cells by nearest-neighbour so the
grid has no holes, and writes the result as int16 centimetres.

The grid describes the **walkable ground**, not the topmost surface. Keeping the maximum y per
cell (what this script used to do) made every cell a building or a wall stands on report the
wall top or the eave — 274 m of roof on a map whose courtyard is at 0 — and the player's ground
lookup rode that straight up onto the roof: "walking near a building launches the character".
So per cell:

* vertices further than ``COURTYARD_BAND`` from ``COURTYARD_Y`` are not ground and are ignored;
* a cell with an in-band vertex takes that band's **surface**: the maximum *within the band*;
* a cell with no in-band vertex but whose **lowest** vertex is below the band (a ditch, the
  step at the foot of a wall, the courtyard's slope) takes that lowest vertex;
* a cell whose geometry only exists *above* the band (a solid wall or roof standing over a cell
  whose floor is a single coarse triangle vertex away) is ground at ``COURTYARD_Y`` — never
  lifted. This is the clamp that makes the invariant below true.
* a cell whose lowest vertex is below ``PIT_FLOOR`` is a hole in the map surface (the sunken pit
  at model (77.3, -57.8), 12 vertices at y = -12.908): it is left empty and filled from its
  neighbours, so a pit cannot punch a 12 m well into the walkable grid.

The invariant this buys: **no cell rises above the courtyard band**, so a cell that contains a
building can never lift the character — the bug this rewrite exists for.

``COURTYARD_Y`` is the walkable courtyard's height in the model frame, the same number
``editor/game/data/prophet.json`` calls ``map.courtyardY`` (0 for this flat re-export); the runtime
removes it from every sample (``storyMap.mjs``), so the grid and the placement stay in one frame.

Frame (see ``editor/game/frontend/storyMapHeight.mjs``, the only reader):

* ``bounds`` is the model's x/z footprint **translated so the model AABB centre is the origin**,
  in model metres — that is the frame the engine puts the mesh in before the actor's
  ``geometry.scale`` is applied (``normalize_vertices_with_global_params`` in
  ``parse_common.h``), so ``world_xz = position.xz + bounds_xz * scale / max_axis``.
* ``minY`` is the model's AABB minimum y in the *untranslated* model frame, because the
  placement measures ``centreY``/``courtyardY`` in that same frame.
* ``heights`` are int16 centimetres **relative to ``minY``**, row-major over z.

Re-run this whenever the map art or the placement's ``scale``/``centreY`` changes; the data
file is a build artifact of the model, not hand-authored content.

Usage::

    conda run -n coronaengine-dev python -B editor/game/tools/bake_story_map_height.py
"""

from __future__ import annotations

import json
import struct
import sys
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "editor/game/art/models/scene/01_山门神殿_平地版.glb"
OUTPUT = ROOT / "editor/game/data/story-map-height.json"

# 256x256 over the 64.8 x 90 m footprint is ~25 x 35 cm per cell: fine enough for a walkable
# surface, small enough (int16) to stay well under the ~600 KB the page is willing to parse.
RESOLUTION = 256

# The walkable ground, in the model frame. `COURTYARD_Y` is `map.courtyardY` from
# `editor/game/data/prophet.json` (0 on the flat-ground re-export); `COURTYARD_BAND` is how far either
# side of it still counts as ground rather than as a wall, a roof or scenery. Two metres covers
# this map's floor slabs and their coarse slopes while ignoring everything a character would
# have to climb onto. `PIT_FLOOR` is the deepest y that is still "ground": below it is a hole in
# the map surface (the pit at model (77.3, -57.8) reaches -12.908) that must be filled from its
# neighbours instead of being baked as a well.
COURTYARD_Y = 0.0
COURTYARD_BAND = 2.0
PIT_FLOOR = -5.0

# glTF constants
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942
FLOAT = 5126
IDENTITY = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]


def load_glb(path: Path):
    """Return (json chunk, absolute offset of the BIN payload, whole file bytes)."""
    raw = path.read_bytes()
    magic, version, total = struct.unpack_from("<4sII", raw, 0)
    if magic != b"glTF" or version != 2 or total != len(raw):
        raise SystemExit(f"{path} 不是有效的 glTF 2.0 二进制容器")
    header = None
    binary = None
    offset = 12
    while offset < total:
        length, kind = struct.unpack_from("<II", raw, offset)
        offset += 8
        if kind == CHUNK_JSON:
            header = json.loads(raw[offset:offset + length].decode("utf-8"))
        elif kind == CHUNK_BIN:
            binary = offset
        offset += length
    if header is None or binary is None:
        raise SystemExit(f"{path} 缺少 JSON 或 BIN 区块")
    return header, binary, raw


def multiply(lhs, rhs):
    """Column-major 4x4 product, matching glTF's matrix layout."""
    return [sum(lhs[k * 4 + row] * rhs[column * 4 + k] for k in range(4))
            for column in range(4) for row in range(4)]


def node_matrix(node, index):
    if "matrix" in node:
        raw = node["matrix"]
        return [raw[column * 4 + row] for column in range(4) for row in range(4)]
    tx, ty, tz = node.get("translation", [0.0, 0.0, 0.0])
    qx, qy, qz, qw = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    sx, sy, sz = node.get("scale", [1.0, 1.0, 1.0])
    rotation = [1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy + qz * qw), 2 * (qx * qz - qy * qw), 0,
                2 * (qx * qy - qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz + qx * qw), 0,
                2 * (qx * qz + qy * qw), 2 * (qy * qz - qx * qw), 1 - 2 * (qx * qx + qy * qy), 0,
                0, 0, 0, 1]
    scales = (sx, sy, sz)
    column = [rotation[column_index * 4 + row] * (scales[column_index] if row < 3 and column_index < 3 else 1.0)
              for column_index in range(4) for row in range(4)]
    column[12], column[13], column[14] = tx, ty, tz
    assert index >= 0
    return column


def mesh_matrices(header):
    """World matrix of every node that carries a mesh, keyed by mesh index."""
    nodes = header.get("nodes", [])
    local = {index: node_matrix(node, index) for index, node in enumerate(nodes)}
    matrices = {}
    scene = header.get("scenes", [{}])[header.get("scene", 0)]
    stack = [(index, IDENTITY) for index in scene.get("nodes", [])]
    while stack:
        index, parent = stack.pop()
        matrix = multiply(parent, local[index])
        if "mesh" in nodes[index]:
            matrices[nodes[index]["mesh"]] = matrix
        stack.extend((child, matrix) for child in nodes[index].get("children", []))
    return matrices


def positions(header, raw, binary, accessor_index):
    """Every vec3 of one POSITION accessor, honouring bufferView offsets and byteStride."""
    accessor = header["accessors"][accessor_index]
    if accessor["componentType"] != FLOAT or accessor["type"] != "VEC3":
        raise SystemExit(f"POSITION accessor {accessor_index} 不是 float 的 vec3")
    view = header["bufferViews"][accessor["bufferView"]]
    stride = view.get("byteStride") or 12
    start = binary + view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    count = accessor["count"]
    if stride == 12:
        return struct.iter_unpack("<3f", raw[start:start + count * 12])
    return (struct.unpack_from("<3f", raw, start + index * stride) for index in range(count))


def model_points(header, raw, binary):
    matrices = mesh_matrices(header)
    total = 0
    for mesh_index, mesh in enumerate(header.get("meshes", [])):
        matrix = matrices.get(mesh_index)
        for primitive in mesh.get("primitives", []):
            attribute = primitive.get("attributes", {}).get("POSITION")
            if attribute is None:
                continue
            for x, y, z in positions(header, raw, binary, attribute):
                total += 1
                if matrix is None or matrix == IDENTITY:
                    yield x, y, z
                else:
                    yield (matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12],
                           matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13],
                           matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14])
    if not total:
        raise SystemExit("地图模型没有任何顶点")


def main() -> int:
    if not SOURCE.is_file():
        raise SystemExit(f"找不到地图模型：{SOURCE}")
    header, binary, raw = load_glb(SOURCE)
    print(f"模型：{SOURCE.relative_to(ROOT)}（{SOURCE.stat().st_size} 字节）", flush=True)

    heights = [None] * (RESOLUTION * RESOLUTION)
    lowest = [float("inf")] * (RESOLUTION * RESOLUTION)
    in_band = [False] * (RESOLUTION * RESOLUTION)
    low = [float("inf")] * 3
    high = [float("-inf")] * 3
    vertices = 0
    for x, y, z in model_points(header, raw, binary):
        vertices += 1
        if x < low[0]: low[0] = x
        if y < low[1]: low[1] = y
        if z < low[2]: low[2] = z
        if x > high[0]: high[0] = x
        if y > high[1]: high[1] = y
        if z > high[2]: high[2] = z

    span_x = high[0] - low[0]
    span_z = high[2] - low[2]
    if span_x <= 0 or span_z <= 0:
        raise SystemExit("地图在 x/z 上没有面积，无法烘焙高度网格")
    centre = [(low[axis] + high[axis]) / 2 for axis in range(3)]
    cell_x = span_x / RESOLUTION
    cell_z = span_z / RESOLUTION

    # Second pass: the AABB is only known now, so re-decode and bin the vertices into the
    # walkable-ground grid (two streaming passes, because materialising 2M vertices costs more
    # memory than the whole rest of this script). A cell keeps three things: the lowest vertex
    # (the candidate ground for a ditch or a slope), the highest vertex still inside the
    # courtyard band (the real surface), and whether any vertex landed in the band at all. The
    # cell's *overall* maximum is deliberately not kept: it is the wall top / eave that used to
    # become the player's ground line.
    for x, y, z in model_points(header, raw, binary):
        ix = int((x - low[0]) / cell_x)
        iz = int((z - low[2]) / cell_z)
        if ix >= RESOLUTION:
            ix = RESOLUTION - 1
        if iz >= RESOLUTION:
            iz = RESOLUTION - 1
        index = iz * RESOLUTION + ix
        if y < lowest[index]:
            lowest[index] = y
        if abs(y - COURTYARD_Y) <= COURTYARD_BAND:
            in_band[index] = True
            if heights[index] is None or y > heights[index]:
                heights[index] = y

    pits = band = clamped = 0
    for index, value in enumerate(lowest):
        if in_band[index]:
            band += 1
            continue
        if value == float("inf"):
            heights[index] = None          # no geometry at all: nearest-neighbour fill
        elif value > COURTYARD_Y + COURTYARD_BAND:
            heights[index] = COURTYARD_Y   # geometry only above the band: it is not ground
            clamped += 1
        elif value < PIT_FLOOR:
            heights[index] = None          # a hole in the surface: fill from the neighbours
            pits += 1
        else:
            heights[index] = value

    holes = sum(1 for value in heights if value is None)
    filled = [value is not None for value in heights]
    queue = deque(index for index, value in enumerate(filled) if value)
    # Nearest-neighbour fill: expand the filled set outwards, so every empty cell adopts the
    # value of the closest real sample instead of leaving a hole the page would trip over.
    while queue:
        index = queue.popleft()
        iz, ix = divmod(index, RESOLUTION)
        for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1)):
            nx, nz = ix + dx, iz + dz
            if nx < 0 or nz < 0 or nx >= RESOLUTION or nz >= RESOLUTION:
                continue
            neighbour = nz * RESOLUTION + nx
            if filled[neighbour]:
                continue
            filled[neighbour] = True
            heights[neighbour] = heights[index]
            queue.append(neighbour)

    minimum = low[1]
    maximum = high[1]
    if any(value is None for value in heights):
        raise SystemExit("高度网格仍有空洞：最近邻填充没有覆盖整张网格")
    # The invariant that makes this grid a *ground* grid: nothing may rise above the walkable
    # band. If a cell did, a building could lift the character again — that was the bug.
    ceiling = COURTYARD_Y + COURTYARD_BAND
    if max(heights) > ceiling:
        raise SystemExit(f"高度网格有单元格高过可行走带上沿 {ceiling:.4f} 米：建筑又变成地面了")
    baked = [int(round((value - minimum) * 100)) for value in heights]
    if any(value < 0 or value > 32767 for value in baked):
        raise SystemExit("高度超出 int16 厘米范围")

    document = {
        "version": 1,
        "res": [RESOLUTION, RESOLUTION],
        "bounds": [round(low[0] - centre[0], 6), round(high[0] - centre[0], 6),
                   round(low[2] - centre[2], 6), round(high[2] - centre[2], 6)],
        "minY": round(minimum, 6),
        "unit": "cm",
        # The band this grid was baked with, so a reader can tell a ground grid from the old
        # maximum-y one without re-deriving it (they store the same version/unit otherwise).
        "courtyardY": COURTYARD_Y,
        "band": COURTYARD_BAND,
        "heights": baked,
    }
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(document, separators=(",", ":")), encoding="utf-8")

    def sample(model_x, model_z):
        ix = min(RESOLUTION - 1, max(0, int((model_x - low[0]) / cell_x)))
        iz = min(RESOLUTION - 1, max(0, int((model_z - low[2]) / cell_z)))
        return minimum + baked[iz * RESOLUTION + ix] / 100.0

    model_heights = [minimum + value / 100.0 for value in baked]
    grid_min = min(model_heights)
    grid_max = max(model_heights)
    mean = sum(model_heights) / len(model_heights)
    flat = sum(1 for value in model_heights if abs(value - COURTYARD_Y) <= 0.05)
    size = OUTPUT.stat().st_size
    print(f"顶点 {vertices}，模型 AABB x[{low[0]:.4f},{high[0]:.4f}] y[{low[1]:.4f},{high[1]:.4f}] "
          f"z[{low[2]:.4f},{high[2]:.4f}]")
    print(f"中心 ({centre[0]:.4f}, {centre[1]:.4f}, {centre[2]:.4f})，网格 {RESOLUTION}x{RESOLUTION}，"
          f"单元格 {cell_x:.4f} x {cell_z:.4f} 模型米")
    print(f"规则：庭院 y = {COURTYARD_Y:.4f} 米 ± {COURTYARD_BAND:.1f} 米的带内取表面；"
          f"带内无顶点时取格内最低点（低于 {PIT_FLOOR:.1f} 米的坑洞留给最近邻填充）")
    print(f"带内取表面 {band} 格，仅高处有几何降为庭院 {clamped} 格，坑洞 {pits} 格，"
          f"空单元格 {holes} 个（均已按最近邻补齐）")
    print(f"高度范围（模型 y）：{grid_min:.4f} .. {grid_max:.4f} 米，平均 {mean:.4f} 米，"
          f"|y| ≤ 5 厘米 {flat} 格（{100.0 * flat / len(baked):.1f}%），模型浮点最高 {maximum:.4f} 米")
    print(f"庭院模型坐标 (0,0) 采样：{sample(0.0, 0.0):.4f} 米")
    for name, (model_x, model_z) in (("player", (0.0, 0.0)), ("boss", (0.0, 12.0)),
                                     ("merchant", (-30.0, -15.0)), ("prophet", (30.0, -15.0))):
        print(f"  出生点 {name:<8} 模型 ({model_x:g}, {model_z:g})：{sample(model_x, model_z):.4f} 米")
    print(f"写出 {OUTPUT.relative_to(ROOT)}：{size} 字节（{size / 1024:.1f} KB）")
    return 0


if __name__ == "__main__":
    sys.exit(main())

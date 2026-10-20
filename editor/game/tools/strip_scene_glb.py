"""Strip a story-map GLB down to an allow-list of material slices and re-pack it.

The story map (``editor/game/art/models/scene/01_山门神殿_平地版.glb``) is a single
merged mesh: one node, one mesh, 43 material slices and ~2M vertices. The engine
imports the whole model, so scenery that has to disappear (buildings, vegetation,
rocks) cannot be removed at runtime -- the map is one mesh with no per-material
control. It has to be stripped out of the asset itself, which is what this tool
does: it keeps only the primitives whose material name is in the allow-list,
copies exactly the bytes those primitives reference into a fresh BIN chunk, and
writes a new GLB.

Usage::

    conda run -n coronaengine-dev python -B editor/game/tools/strip_scene_glb.py

The default allow-list keeps the flat ``Ground`` slice: the map becomes a plain
plane (28407 vertices, y = 0), which is the "only the ground remains" edition
used when buildings, vegetation and rocks are stripped out of the small world.
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "editor/game/art/models/scene/01_山门神殿_平地版.glb"
OUTPUT = ROOT / "editor/game/art/models/scene/01_山门神殿_平地版_仅地面.glb"
# Material slices that survive the strip. ``Ground`` is the flat y = 0 plane the
# characters stand on; every other slice (buildings, trees, grass, cliffs, rocks)
# is dropped.
KEEP_MATERIALS = frozenset({"Ground"})

# glTF constants
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942
_COMPONENT_BYTES = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
_COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


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


def accessor_size(accessor: dict) -> int:
    """Byte size of one accessor's payload, without its bufferView padding."""
    return (accessor["count"] * _COMPONENT_BYTES[accessor["componentType"]]
            * _COMPONENTS[accessor["type"]])


def pack(header: dict, binary: int, raw: bytes) -> bytes:
    """Re-pack the GLB: keep only allow-listed primitives and their bytes."""
    import copy
    keep = copy.deepcopy(header)
    materials = keep.get("materials", [])
    names = [m.get("name", "") for m in materials]
    meshes = keep.get("meshes", [])

    # Which material indexes survive, by name.
    surviving = [index for index, name in enumerate(names) if name in KEEP_MATERIALS]
    if not surviving:
        raise SystemExit(f"没有保留任何材质：{sorted(KEEP_MATERIALS)} 都不在模型里")

    used_accessors: set[int] = set()
    for mesh in meshes:
        kept_primitives = []
        for primitive in mesh.get("primitives", []):
            if primitive.get("material") not in surviving:
                continue
            kept_primitives.append(primitive)
            for accessor_index in primitive.get("attributes", {}).values():
                used_accessors.add(accessor_index)
            if "indices" in primitive:
                used_accessors.add(primitive["indices"])
        mesh["primitives"] = kept_primitives
        if not kept_primitives:
            raise SystemExit(f"网格 {mesh.get('name', '')} 没有保留任何 primitive")

    # Only the materials that are still referenced survive, re-indexed in order.
    material_map = {old: new for new, old in enumerate(surviving)}
    keep["materials"] = [materials[index] for index in surviving]
    for mesh in meshes:
        for primitive in mesh.get("primitives", []):
            primitive["material"] = material_map[primitive["material"]]

    # Copy exactly the bytes each surviving accessor references into a fresh BIN
    # chunk, one dedicated 4-byte-aligned bufferView per accessor. Traversing the
    # old accessor list in ascending order keeps blob order == accessor order.
    old_accessors = keep.get("accessors", [])
    views: list[dict] = []
    blob = bytearray()
    for old_index, accessor in enumerate(old_accessors):
        if old_index not in used_accessors:
            continue
        view = accessor.get("bufferView")
        if view is None:
            raise SystemExit(f"accessor {old_index} 没有 bufferView，无法裁剪")
        byte_offset = accessor.get("byteOffset", 0)
        byte_length = accessor_size(accessor)
        source_view = keep["bufferViews"][view]
        start = binary + source_view.get("byteOffset", 0) + byte_offset
        payload = raw[start:start + byte_length]
        if len(payload) != byte_length:
            raise SystemExit(f"accessor {old_index} 的字节越界")
        pad = (-len(blob)) % 4
        if pad:
            blob.extend(b"\x00" * pad)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": byte_length})
        blob.extend(payload)

    new_accessors = [dict(accessor) for old_index, accessor in enumerate(old_accessors)
                     if old_index in used_accessors]
    for index, accessor in enumerate(new_accessors):
        accessor["bufferView"] = index
        accessor.pop("byteOffset", None)

    keep["bufferViews"] = views
    keep["accessors"] = new_accessors
    keep["buffers"] = [{"byteLength": len(blob)}]

    # Re-map primitive attribute/indices references to the new accessor indexes.
    new_order = [old for old in range(len(old_accessors)) if old in used_accessors]
    accessor_map = {old: new for new, old in enumerate(new_order)}
    for mesh in meshes:
        for primitive in mesh.get("primitives", []):
            primitive["attributes"] = {
                name: accessor_map[index] for name, index in primitive["attributes"].items()}
            if "indices" in primitive:
                primitive["indices"] = accessor_map[primitive["indices"]]

    # Assemble the GLB: header + JSON chunk + BIN chunk.
    json_bytes = json.dumps(keep, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    json_bytes += b"\x20" * ((4 - len(json_bytes) % 4) % 4)
    bin_bytes = bytes(blob)
    bin_bytes += b"\x00" * ((4 - len(bin_bytes) % 4) % 4)
    total = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)
    return (struct.pack("<4sII", b"glTF", 2, total)
            + struct.pack("<II", len(json_bytes), CHUNK_JSON) + json_bytes
            + struct.pack("<II", len(bin_bytes), CHUNK_BIN) + bin_bytes)


def main() -> None:
    header, binary, raw = load_glb(SOURCE)
    blob = pack(header, binary, raw)
    OUTPUT.write_bytes(blob)
    kept = header["meshes"][0]["primitives"]
    before = SOURCE.stat().st_size
    print(f"{SOURCE.name}: {before / 1e6:.1f} MB -> {OUTPUT.name}: {len(blob) / 1e6:.1f} MB")
    print(f"materials kept: {sorted(KEEP_MATERIALS)}")


if __name__ == "__main__":
    sys.exit(main())

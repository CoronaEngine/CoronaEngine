"""Vision proxy OBJ generation and native model correction helpers."""

import logging
import os
import uuid

from .vision_geometry import (
    aabb_center_and_max_axis,
    clean_near_zero,
    vision_primitive_vertices,
)


logger = logging.getLogger(__name__)


def vision_model_native_local_correction(model_path: str):
    if not model_path or os.path.splitext(model_path)[1].lower() != ".obj":
        return None
    vertices = []
    try:
        with open(model_path, "r", encoding="utf-8", errors="ignore") as file:
            for line in file:
                stripped = line.strip()
                if not stripped.startswith("v "):
                    continue
                parts = stripped.split()
                if len(parts) < 4:
                    continue
                vertices.append([float(parts[1]), float(parts[2]), float(parts[3])])
    except (OSError, ValueError) as exc:
        logger.warning("Failed to parse Vision model bounds for %s: %s", model_path, exc)
        return None

    if not vertices:
        return None
    center, max_axis = aabb_center_and_max_axis(vertices)
    return {
        "native_local_correction_offset": [
            clean_near_zero(center[0]),
            clean_near_zero(center[1]),
            clean_near_zero(-center[2]),
        ],
        "native_local_correction_scale": clean_near_zero(max_axis),
    }


def write_vision_primitive_proxy(
    project_root: str,
    shape: dict,
    shape_type: str,
    json_path: str,
    shape_guid: str,
):
    if not project_root:
        return "", ""

    local_vertices, faces = vision_primitive_vertices(shape, shape_type)
    if not local_vertices or not faces:
        return "", ""

    vertices = [
        [clean_near_zero(vertex[0]), clean_near_zero(vertex[1]), clean_near_zero(-vertex[2])]
        for vertex in local_vertices
    ]

    raw_name = str(shape.get("name") or shape_type).strip()
    stem = "".join(char if char.isalnum() or char in "_.-" else "_" for char in raw_name)
    stem = stem.strip("._") or "vision_shape"
    filename = f"{stem}_{uuid.uuid5(uuid.NAMESPACE_URL, shape_guid).hex[:12]}.obj"
    proxy_dir = os.path.join(project_root, "Resource", "vision_proxies")
    os.makedirs(proxy_dir, exist_ok=True)
    abs_path = os.path.join(proxy_dir, filename)
    rel_path = os.path.join("Resource", "vision_proxies", filename).replace("\\", "/")

    lines = [f"# Corona external_live proxy for {shape_type} {json_path}"]
    for x, y, z in vertices:
        lines.append(f"v {x:.17g} {y:.17g} {z:.17g}")
    for face in faces:
        lines.append("f " + " ".join(str(index) for index in face))
    with open(abs_path, "w", encoding="utf-8") as file:
        file.write("\n".join(lines) + "\n")

    return rel_path, abs_path

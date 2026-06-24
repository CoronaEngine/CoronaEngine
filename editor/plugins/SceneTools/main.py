import json
import math
import os
import re
import uuid
import copy

from CoronaCore.core.components import Optics
from CoronaCore.core import network_sync_policy
from CoronaCore.core.corona_editor import CoronaEditor
from CoronaPlugin.core.corona_plugin_base import PluginBase
from CoronaCore.core.entities import Actor
from CoronaCore.core.entities.camera import Camera
from CoronaCore.core.managers import scene_manager
from CoronaCore.utils.file_handler import FileHandler

import logging

logger = logging.getLogger(__name__)

_SUPPORTED_VISION_PRIMITIVES = {"quad", "cube", "sphere"}


def _active_project_path():
    try:
        from utils.settings import settings_manager
        if settings_manager.active_project_path:
            return settings_manager.active_project_path
    except Exception:
        pass
    return getattr(CoronaEditor.CoronaEngine, "active_project_path", None)


def _as_float3(value):
    if not isinstance(value, (list, tuple)) or len(value) < 3:
        return None
    try:
        return [float(value[0]), float(value[1]), float(value[2])]
    except (TypeError, ValueError):
        return None


def _normalize_vec3(value):
    vec = _as_float3(value)
    if vec is None:
        return None
    length = math.sqrt(vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2])
    if length <= 1e-8:
        return None
    return [vec[0] / length, vec[1] / length, vec[2] / length]


def _vision_vec_to_corona(value):
    vec = _as_float3(value)
    if vec is None:
        return None
    return [vec[0], vec[1], -vec[2]]


def _extract_vision_camera_pose(document: dict):
    scene_data = document.get("scene", document) if isinstance(document, dict) else {}
    camera = None
    cameras = scene_data.get("cameras")
    if isinstance(cameras, list) and cameras:
        camera = cameras[0]
    if camera is None:
        camera = scene_data.get("camera") or document.get("camera")
    if not isinstance(camera, dict):
        return None

    params = camera.get("param") if isinstance(camera.get("param"), dict) else camera
    transform = params.get("transform") if isinstance(params.get("transform"), dict) else {}
    transform_params = (
        transform.get("param") if isinstance(transform.get("param"), dict) else transform
    )

    position = (
        _vision_vec_to_corona(transform_params.get("position"))
        or _vision_vec_to_corona(params.get("position"))
        or _vision_vec_to_corona(camera.get("position"))
    )
    up = (
        _normalize_vec3(_vision_vec_to_corona(transform_params.get("up")))
        or _normalize_vec3(_vision_vec_to_corona(params.get("up")))
        or _normalize_vec3(_vision_vec_to_corona(params.get("world_up")))
        or [0.0, 1.0, 0.0]
    )
    forward = (
        _normalize_vec3(_vision_vec_to_corona(transform_params.get("forward")))
        or _normalize_vec3(_vision_vec_to_corona(transform_params.get("direction")))
        or _normalize_vec3(_vision_vec_to_corona(params.get("forward")))
        or _normalize_vec3(_vision_vec_to_corona(params.get("direction")))
    )
    target = _vision_vec_to_corona(transform_params.get("target_pos") or transform_params.get("target"))
    if forward is None and position is not None and target is not None:
        forward = _normalize_vec3([
            target[0] - position[0],
            target[1] - position[1],
            target[2] - position[2],
        ])

    fov = (
        params.get("fov_y")
        or params.get("fov")
        or params.get("vfov")
        or camera.get("fov")
        or 45.0
    )
    try:
        fov = float(fov)
    except (TypeError, ValueError):
        fov = 45.0
    if 0.0 < fov <= math.pi:
        fov = math.degrees(fov)

    if position is None or forward is None:
        return None

    return {
        "name": str(params.get("name") or camera.get("name") or "VisionCamera"),
        "position": position,
        "forward": forward,
        "world_up": up,
        "fov": fov,
    }


def _infer_vision_render_mode(document: dict) -> str:
    if not isinstance(document, dict):
        return "path_tracing"

    output = document.get("output")
    output_denoise = (
        bool(output.get("denoise"))
        if isinstance(output, dict) and "denoise" in output
        else False
    )
    render = document.get("render")
    integrator = render.get("integrator") if isinstance(render, dict) else {}
    integrator_param = (
        integrator.get("param") if isinstance(integrator, dict) else {}
    )
    denoiser = (
        integrator_param.get("denoiser")
        if isinstance(integrator_param, dict)
        else {}
    )
    denoiser_type = (
        str(denoiser.get("type") or "").strip().lower()
        if isinstance(denoiser, dict)
        else ""
    )

    pipeline = document.get("pipeline")
    pipeline_param = pipeline.get("param") if isinstance(pipeline, dict) else {}
    frame_buffer = (
        pipeline_param.get("frame_buffer")
        if isinstance(pipeline_param, dict)
        else {}
    )
    frame_buffer_type = (
        str(frame_buffer.get("type") or "").strip().lower()
        if isinstance(frame_buffer, dict)
        else ""
    )

    if frame_buffer_type == "lightfield" or denoiser_type == "ssat":
        return "ssat"
    if output_denoise and denoiser_type == "svgf":
        return "svgf"
    return "path_tracing"


def _vision_scene_data(document: dict) -> dict:
    return document.get("scene", document) if isinstance(document, dict) else {}


def _iter_vision_shapes(document: dict):
    scene_data = _vision_scene_data(document)
    shapes = scene_data.get("shapes", [])
    if isinstance(shapes, list):
        for index, shape in enumerate(shapes):
            if isinstance(shape, dict):
                yield index, f"/scene/shapes/{index}", shape
    elif isinstance(shapes, dict):
        for index, (key, shape) in enumerate(shapes.items()):
            if isinstance(shape, dict):
                yield index, f"/scene/shapes/{key}", shape


def _vision_shape_params(shape: dict) -> dict:
    params = shape.get("param")
    return params if isinstance(params, dict) else {}


def _vision_shape_type(shape: dict) -> str:
    return str(shape.get("type") or shape.get("shape_type") or "").strip().lower()


def _vision_proxy_name(shape: dict, shape_type: str, shape_index: int) -> str:
    raw = shape.get("name")
    if isinstance(raw, str) and raw.strip():
        return raw.strip()
    return f"vision_{shape_type or 'shape'}_{shape_index}"


def _vision_shape_name(shape: dict, model_path: str, shape_index: int) -> str:
    raw = shape.get("name")
    if isinstance(raw, str) and raw.strip():
        return raw.strip()
    stem = os.path.splitext(os.path.basename(model_path))[0]
    return stem or f"vision_shape_{shape_index}"


def _vision_shape_guid(shape: dict, json_path: str) -> str:
    declared_guid = _vision_shape_declared_guid(shape)
    if declared_guid:
        return declared_guid
    return f"vision-shape-{uuid.uuid5(uuid.NAMESPACE_URL, json_path).hex}"


def _vision_shape_declared_guid(shape: dict) -> str:
    for key in ("shape_guid", "guid", "id"):
        value = shape.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return ""


def _json_identity(value) -> str:
    try:
        return json.dumps(value, sort_keys=True, separators=(",", ":"))
    except TypeError:
        return str(value)


def _vision_shape_identity_key(scene_path: str, shape: dict, json_path: str) -> str:
    declared_guid = _vision_shape_declared_guid(shape)
    if declared_guid:
        return f"guid:{declared_guid}"

    shape_type = _vision_shape_type(shape)
    if shape_type == "model":
        model_path = _resolve_vision_model_path(scene_path, shape)
        if model_path:
            return f"model:{os.path.normcase(os.path.abspath(model_path))}"

    params = dict(_vision_shape_params(shape))
    params.pop("transform", None)
    payload = {
        "type": shape_type,
        "params": params,
        "fn": shape.get("fn"),
        "path": shape.get("path"),
    }
    return f"shape:{_json_identity(payload)}"


def _resolve_vision_model_path(scene_path: str, shape: dict) -> str:
    params = _vision_shape_params(shape)
    model_path = params.get("fn") or shape.get("fn") or params.get("path") or shape.get("path")
    if not isinstance(model_path, str) or not model_path.strip():
        return ""
    model_path = model_path.strip()
    if os.path.isabs(model_path):
        return os.path.abspath(model_path)
    return os.path.abspath(os.path.join(os.path.dirname(scene_path), model_path))


def _vision_model_native_local_correction(model_path: str):
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
    center, max_axis = _aabb_center_and_max_axis(vertices)
    return {
        "native_local_correction_offset": [
            _clean_near_zero(center[0]),
            _clean_near_zero(center[1]),
            _clean_near_zero(-center[2]),
        ],
        "native_local_correction_scale": _clean_near_zero(max_axis),
    }


def _flatten_matrix4x4(value):
    if isinstance(value, list) and len(value) == 16:
        try:
            return [float(item) for item in value]
        except (TypeError, ValueError):
            return None
    if isinstance(value, list) and len(value) == 4 and all(
            isinstance(row, list) and len(row) == 4 for row in value):
        try:
            return [float(item) for row in value for item in row]
        except (TypeError, ValueError):
            return None
    return None


def _vector_length(vec):
    return math.sqrt(sum(component * component for component in vec))


def _clean_near_zero(value):
    return 0.0 if abs(value) < 1e-9 else value


def _aabb_center_and_max_axis(vertices):
    if not vertices:
        return [0.0, 0.0, 0.0], 1.0
    mins = [min(vertex[i] for vertex in vertices) for i in range(3)]
    maxs = [max(vertex[i] for vertex in vertices) for i in range(3)]
    center = [(mins[i] + maxs[i]) * 0.5 for i in range(3)]
    max_axis = max(maxs[i] - mins[i] for i in range(3))
    return center, max_axis if max_axis > 1e-8 else 1.0


def _matrix4x4_to_corona_trs(matrix):
    # Vision and Corona use opposite Z handedness. Convert object matrices with
    # F * M * F, matching the C++ built-in Vision geometry adapter.
    position = [matrix[12], matrix[13], -matrix[14]]
    columns = [
        [matrix[0], matrix[1], -matrix[2]],
        [matrix[4], matrix[5], -matrix[6]],
        [-matrix[8], -matrix[9], matrix[10]],
    ]
    scale = [_vector_length(column) for column in columns]
    if any(component <= 1e-8 for component in scale):
        return {"position": position, "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]}

    r00, r10, r20 = [columns[0][i] / scale[0] for i in range(3)]
    r01, r11, r21 = [columns[1][i] / scale[1] for i in range(3)]
    r02, r12, r22 = [columns[2][i] / scale[2] for i in range(3)]

    sin_y = max(-1.0, min(1.0, -r20))
    y = math.asin(sin_y)
    cos_y = math.cos(y)
    if abs(cos_y) > 1e-6:
        x = math.atan2(r21, r22)
        z = math.atan2(r10, r00)
    else:
        x = math.atan2(-r12, r11)
        z = 0.0

    return {"position": position, "rotation": [x, y, z], "scale": scale}


def _vision_transform_matrix(shape: dict):
    params = _vision_shape_params(shape)
    transform = params.get("transform") if isinstance(params.get("transform"), dict) else {}
    transform_params = transform.get("param") if isinstance(transform.get("param"), dict) else transform
    transform_type = str(transform.get("type") or "matrix4x4").lower()
    if transform_type != "matrix4x4":
        return None
    return _flatten_matrix4x4(transform_params.get("matrix4x4"))


def _apply_vision_matrix_to_corona(matrix, point):
    if not matrix:
        x, y, z = point
    else:
        px, py, pz = point
        x = px * matrix[0] + py * matrix[4] + pz * matrix[8] + matrix[12]
        y = px * matrix[1] + py * matrix[5] + pz * matrix[9] + matrix[13]
        z = px * matrix[2] + py * matrix[6] + pz * matrix[10] + matrix[14]
    return [x, y, -z]


def _apply_corona_trs_to_point(transform: dict, point):
    sx, sy, sz = transform.get("scale", [1.0, 1.0, 1.0])
    rx, ry, rz = transform.get("rotation", [0.0, 0.0, 0.0])
    tx, ty, tz = transform.get("position", [0.0, 0.0, 0.0])
    x = point[0] * sx
    y = point[1] * sy
    z = point[2] * sz

    cos_x = math.cos(rx)
    sin_x = math.sin(rx)
    y, z = y * cos_x - z * sin_x, y * sin_x + z * cos_x

    cos_y = math.cos(ry)
    sin_y = math.sin(ry)
    x, z = x * cos_y + z * sin_y, -x * sin_y + z * cos_y

    cos_z = math.cos(rz)
    sin_z = math.sin(rz)
    x, y = x * cos_z - y * sin_z, x * sin_z + y * cos_z

    return [x + tx, y + ty, z + tz]


def _extract_vision_primitive_proxy_transform(shape: dict) -> dict:
    return _extract_vision_shape_transform(shape)


def _extract_vision_shape_transform(shape: dict) -> dict:
    params = _vision_shape_params(shape)
    transform = params.get("transform") if isinstance(params.get("transform"), dict) else {}
    transform_params = transform.get("param") if isinstance(transform.get("param"), dict) else transform
    transform_type = str(transform.get("type") or "matrix4x4").lower()

    position = [0.0, 0.0, 0.0]
    rotation = [0.0, 0.0, 0.0]
    scale = [1.0, 1.0, 1.0]

    if transform_type == "matrix4x4":
        matrix = _flatten_matrix4x4(transform_params.get("matrix4x4"))
        if matrix:
            return _matrix4x4_to_corona_trs(matrix)
    elif transform_type == "trs":
        t = _as_float3(transform_params.get("t"))
        s = _as_float3(transform_params.get("s"))
        if t:
            position = [t[0], t[1], -t[2]]
        if s:
            scale = s
    elif transform_type == "euler":
        t = _as_float3(transform_params.get("position"))
        if t:
            position = [t[0], t[1], -t[2]]
        try:
            rotation = [
                float(transform_params.get("pitch", 0.0)),
                -float(transform_params.get("yaw", 0.0)),
                -float(transform_params.get("roll", 0.0)),
            ]
        except (TypeError, ValueError):
            rotation = [0.0, 0.0, 0.0]

    return {"position": position, "rotation": rotation, "scale": scale}


def _project_root_for_scene(scene):
    project_root = _active_project_path()
    if project_root:
        return os.path.abspath(project_root)
    scene_route = getattr(scene, "route", "")
    if os.path.isabs(scene_route):
        parent = os.path.dirname(os.path.dirname(scene_route))
        if parent:
            return os.path.abspath(parent)
    return ""


def _safe_filename_stem(value: str) -> str:
    stem = re.sub(r"[^0-9A-Za-z_.-]+", "_", value.strip())
    return stem.strip("._") or "vision_shape"


def _vision_primitive_vertices(shape: dict, shape_type: str):
    params = _vision_shape_params(shape)
    if shape_type == "quad":
        width = float(params.get("width", 1.0))
        height = float(params.get("height", 1.0))
        hw = width * 0.5
        hh = height * 0.5
        return [
            [hw, 0.0, hh],
            [hw, 0.0, -hh],
            [-hw, 0.0, hh],
            [-hw, 0.0, -hh],
        ], [[1, 2, 3], [3, 2, 4]]
    if shape_type == "cube":
        x = float(params.get("x", params.get("width", 1.0)))
        y = float(params.get("y", params.get("height", 1.0)))
        z = float(params.get("z", params.get("depth", 1.0)))
        y = x if y == 0.0 else y
        z = y if z == 0.0 else z
        sx = x * 0.5
        sy = y * 0.5
        sz = z * 0.5
        return [
            [-sx, -sy, -sz], [sx, -sy, -sz], [sx, sy, -sz], [-sx, sy, -sz],
            [-sx, -sy, sz], [sx, -sy, sz], [sx, sy, sz], [-sx, sy, sz],
        ], [
            [1, 2, 3, 4],
            [5, 8, 7, 6],
            [1, 5, 6, 2],
            [2, 6, 7, 3],
            [3, 7, 8, 4],
            [4, 8, 5, 1],
        ]
    if shape_type == "sphere":
        radius = float(params.get("radius", 1.0))
        theta_div = max(3, int(params.get("sub_div", 60)))
        phi_div = 2 * theta_div
        vertices = [[0.0, radius, 0.0]]
        for i in range(1, theta_div):
            v = float(i) / theta_div
            theta = math.pi * v
            y = radius * math.cos(theta)
            ring_radius = radius * math.sin(theta)
            for j in range(phi_div):
                u = float(j) / phi_div
                phi = u * math.tau
                vertices.append([
                    math.cos(phi) * ring_radius,
                    y,
                    math.sin(phi) * ring_radius,
                ])
        vertices.append([0.0, -radius, 0.0])

        faces = []
        for i in range(phi_div):
            faces.append([1, ((i + 1) % phi_div) + 2, i + 2])

        for i in range(theta_div - 2):
            vert_start = 2 + i * phi_div
            for j in range(phi_div):
                current = vert_start + j
                next_vertex = vert_start + ((j + 1) % phi_div)
                below = current + phi_div
                below_next = next_vertex + phi_div
                faces.append([current, next_vertex, below])
                faces.append([next_vertex, below_next, below])

        bottom = len(vertices)
        last_ring = 2 + (theta_div - 2) * phi_div
        for i in range(phi_div):
            current = last_ring + i
            next_vertex = last_ring + ((i + 1) % phi_div)
            faces.append([bottom, next_vertex, current])
        return vertices, faces
    return [], []


def _vision_primitive_world_vertices(shape: dict, local_vertices):
    matrix = _vision_transform_matrix(shape)
    if matrix:
        return [_apply_vision_matrix_to_corona(matrix, vertex) for vertex in local_vertices]

    transform = _extract_vision_shape_transform(shape)
    return [
        _apply_corona_trs_to_point(transform, [vertex[0], vertex[1], -vertex[2]])
        for vertex in local_vertices
    ]


def _ensure_vision_primitive_proxy(scene, shape: dict, shape_type: str, json_path: str):
    project_root = _project_root_for_scene(scene)
    if not project_root:
        return "", ""

    local_vertices, faces = _vision_primitive_vertices(shape, shape_type)
    if not local_vertices or not faces:
        return "", ""

    vertices = [
        [_clean_near_zero(vertex[0]), _clean_near_zero(vertex[1]), _clean_near_zero(-vertex[2])]
        for vertex in local_vertices
    ]

    shape_guid = _vision_shape_guid(shape, json_path)
    stem = _safe_filename_stem(str(shape.get("name") or shape_type))
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


def _actor_transform(actor):
    def _read_vec(method_name, attr_name, fallback):
        method = getattr(actor, method_name, None)
        if callable(method):
            try:
                value = method()
            except Exception:
                value = None
        else:
            value = getattr(actor, attr_name, None)
        vec = _as_float3(value)
        return vec if vec is not None else list(fallback)

    return {
        "position": _read_vec("get_position", "position", [0.0, 0.0, 0.0]),
        "rotation": _read_vec("get_rotation", "rotation", [0.0, 0.0, 0.0]),
        "scale": _read_vec("get_scale", "scale", [1.0, 1.0, 1.0]),
    }


def _rotate_corona_vector(vec, rotation):
    x, y, z = vec
    rx, ry, rz = rotation

    cos_x = math.cos(rx)
    sin_x = math.sin(rx)
    y, z = y * cos_x - z * sin_x, y * sin_x + z * cos_x

    cos_y = math.cos(ry)
    sin_y = math.sin(ry)
    x, z = x * cos_y + z * sin_y, -x * sin_y + z * cos_y

    cos_z = math.cos(rz)
    sin_z = math.sin(rz)
    x, y = x * cos_z - y * sin_z, x * sin_z + y * cos_z
    return [x, y, z]


def _corona_trs_to_vision_matrix4x4(transform: dict):
    position = transform.get("position", [0.0, 0.0, 0.0])
    rotation = transform.get("rotation", [0.0, 0.0, 0.0])
    scale = transform.get("scale", [1.0, 1.0, 1.0])

    corona_columns = [
        _rotate_corona_vector([scale[0], 0.0, 0.0], rotation) + [0.0],
        _rotate_corona_vector([0.0, scale[1], 0.0], rotation) + [0.0],
        _rotate_corona_vector([0.0, 0.0, scale[2]], rotation) + [0.0],
        [position[0], position[1], position[2], 1.0],
    ]

    vision_columns = []
    for col_index, column in enumerate(corona_columns):
        vision_column = []
        for row_index, value in enumerate(column):
            if row_index == 2:
                value = -value
            if col_index == 2:
                value = -value
            vision_column.append(_clean_near_zero(float(value)))
        vision_columns.append(vision_column)
    return vision_columns


def _shape_collection(document: dict):
    scene_data = _vision_scene_data(document)
    shapes = scene_data.get("shapes", [])
    return shapes if isinstance(shapes, (list, dict)) else []


def _remove_shape_at_json_path(document: dict, json_path: str) -> bool:
    shapes = _shape_collection(document)
    if not json_path.startswith("/scene/shapes/"):
        return False
    key = json_path[len("/scene/shapes/"):]
    if isinstance(shapes, list):
        try:
            index = int(key)
        except ValueError:
            return False
        if 0 <= index < len(shapes):
            shapes[index] = None
            return True
        return False
    if isinstance(shapes, dict) and key in shapes:
        del shapes[key]
        return True
    return False


def _shape_at_json_path(document: dict, json_path: str):
    shapes = _shape_collection(document)
    if not json_path.startswith("/scene/shapes/"):
        return None
    key = json_path[len("/scene/shapes/"):]
    if isinstance(shapes, list):
        try:
            index = int(key)
        except ValueError:
            return None
        if 0 <= index < len(shapes) and isinstance(shapes[index], dict):
            return shapes[index]
        return None
    if isinstance(shapes, dict) and isinstance(shapes.get(key), dict):
        return shapes[key]
    return None


def _compact_removed_shapes(document: dict) -> None:
    scene_data = _vision_scene_data(document)
    shapes = scene_data.get("shapes", [])
    if isinstance(shapes, list):
        scene_data["shapes"] = [shape for shape in shapes if shape is not None]


def _derived_vision_scene_path(source_path: str, scene) -> str:
    source_dir = os.path.dirname(os.path.abspath(source_path))
    source_stem, source_ext = os.path.splitext(os.path.basename(source_path))
    scene_stem = _safe_filename_stem(getattr(scene, "name", "") or "scene")
    key = f"{os.path.abspath(source_path)}|{getattr(scene, 'route', '')}"
    suffix = uuid.uuid5(uuid.NAMESPACE_URL, key).hex[:12]
    return os.path.join(source_dir, f"{source_stem}.corona_{scene_stem}_{suffix}{source_ext or '.json'}")


def _atomic_write_json(path: str, document: dict) -> None:
    directory = os.path.dirname(os.path.abspath(path))
    temp_path = os.path.join(directory, f".{os.path.basename(path)}.{uuid.uuid4().hex}.tmp")
    try:
        with open(temp_path, "w", encoding="utf-8") as file:
            json.dump(document, file, ensure_ascii=False, indent=2)
            file.write("\n")
            file.flush()
            os.fsync(file.fileno())
        os.replace(temp_path, path)
    except Exception:
        try:
            if os.path.exists(temp_path):
                os.remove(temp_path)
        except OSError:
            logger.warning("Failed to remove temporary Vision scene file: %s", temp_path)
        raise


def _write_derived_external_live_scene(scene, source_path: str) -> str:
    if not source_path or not os.path.isfile(source_path):
        return source_path

    with open(source_path, "r", encoding="utf-8") as file:
        document = json.load(file)

    derived = copy.deepcopy(document)
    bindings = list(getattr(scene, "vision_bindings", []))
    for binding in bindings:
        json_path = binding.get("json_path", "")
        actor = _find_actor_by_guid(scene, binding.get("actor_guid", ""))
        if actor is None:
            _remove_shape_at_json_path(derived, json_path)
            continue

        shape = _shape_at_json_path(derived, json_path)
        if shape is None:
            continue

        shape_type = (binding.get("shape_type") or _vision_shape_type(shape)).lower()
        if shape_type != "model" and shape_type not in _SUPPORTED_VISION_PRIMITIVES:
            continue

        params = shape.setdefault("param", {})
        if not isinstance(params, dict):
            params = {}
            shape["param"] = params
        params["transform"] = {
            "type": "matrix4x4",
            "param": {
                "matrix4x4": _corona_trs_to_vision_matrix4x4(_actor_transform(actor)),
            },
        }

    _compact_removed_shapes(derived)
    path = _derived_vision_scene_path(source_path, scene)
    _atomic_write_json(path, derived)
    return path


def _sync_external_live_binding_source_path(scene, runtime_path: str) -> None:
    if not runtime_path:
        return
    for binding in list(getattr(scene, "vision_bindings", [])):
        actor = _find_actor_by_guid(scene, binding.get("actor_guid", ""))
        if actor is None or not hasattr(actor, "set_external_vision_binding"):
            continue
        runtime_binding = dict(binding)
        runtime_binding["source_path"] = runtime_path
        actor.set_external_vision_binding(runtime_binding)


def prepare_external_live_vision_scene(scene) -> str:
    source_path = getattr(scene, "vision_source_path", "") or ""
    if not source_path or getattr(scene, "vision_import_mode", "") != "external_live":
        return source_path
    source_path = os.path.abspath(source_path)
    runtime_path = source_path
    if getattr(scene, "vision_bindings", []):
        try:
            runtime_path = _write_derived_external_live_scene(scene, source_path)
        except Exception as exc:
            logger.exception("Failed to write derived external_live Vision scene: %s", exc)
            runtime_path = source_path
    _sync_external_live_binding_source_path(scene, runtime_path)
    return runtime_path


def _find_actor_by_guid(scene, actor_guid: str):
    if not actor_guid:
        return None
    for actor in scene.get_actors():
        if getattr(actor, "actor_guid", "") == actor_guid:
            return actor
    return None


def _binding_is_compatible(binding: dict, shape_type: str, model_path: str,
                           identity_key: str) -> bool:
    if binding.get("shape_type") and binding.get("shape_type") != shape_type:
        return False
    if model_path and binding.get("model_path") and (
            os.path.normcase(os.path.abspath(binding.get("model_path"))) !=
            os.path.normcase(os.path.abspath(model_path))):
        return False
    if binding.get("shape_identity_key"):
        return binding.get("shape_identity_key") == identity_key
    return True


def _find_previous_binding(bindings, used_binding_indices: set, shape: dict, shape_type: str,
                           json_path: str, scene_path: str, model_path: str):
    current_guid = _vision_shape_guid(shape, json_path)
    current_identity_key = _vision_shape_identity_key(scene_path, shape, json_path)
    declared_guid = _vision_shape_declared_guid(shape)

    if declared_guid:
        for index, binding in enumerate(bindings or []):
            if index in used_binding_indices:
                continue
            if binding.get("shape_guid") == current_guid:
                used_binding_indices.add(index)
                return binding

    identity_matches = []
    for index, binding in enumerate(bindings or []):
        if index in used_binding_indices:
            continue
        if binding.get("shape_identity_key") == current_identity_key:
            identity_matches.append((index, binding))
    if len(identity_matches) == 1:
        index, binding = identity_matches[0]
        used_binding_indices.add(index)
        return binding

    for index, binding in enumerate(bindings or []):
        if index in used_binding_indices:
            continue
        if binding.get("json_path") != json_path:
            continue
        if not _binding_is_compatible(binding, shape_type, model_path, current_identity_key):
            continue
        used_binding_indices.add(index)
        return binding
    return None


def _vision_import_summary(import_mode: str, source_path: str, bindings, unsupported_shapes) -> dict:
    unsupported_by_reason = {}
    unsupported_by_type = {}
    for shape in unsupported_shapes or []:
        reason = shape.get("reason") or "unknown"
        shape_type = shape.get("type") or "unknown"
        unsupported_by_reason[reason] = unsupported_by_reason.get(reason, 0) + 1
        unsupported_by_type[shape_type] = unsupported_by_type.get(shape_type, 0) + 1
    return {
        "import_mode": import_mode,
        "source_path": source_path,
        "binding_count": len(bindings or []),
        "unsupported_count": len(unsupported_shapes or []),
        "unsupported_by_reason": unsupported_by_reason,
        "unsupported_by_type": unsupported_by_type,
        "unsupported_shapes": list(unsupported_shapes or []),
    }


def _remove_stale_vision_proxy_actors(scene, previous_bindings, active_actor_guids) -> int:
    removed = 0
    for binding in previous_bindings or []:
        actor_guid = binding.get("actor_guid", "")
        if not actor_guid or actor_guid in active_actor_guids:
            continue
        actor = _find_actor_by_guid(scene, actor_guid)
        if actor is None:
            continue
        try:
            if hasattr(actor, "clear_external_vision_binding"):
                actor.clear_external_vision_binding()
            if hasattr(scene, "remove_actor"):
                scene.remove_actor(actor)
                removed += 1
        except Exception as exc:
            logger.warning("Failed to remove stale Vision proxy actor %s: %s",
                           getattr(actor, "name", actor_guid), exc)
    return removed


@PluginBase.register_web("SceneTools")
class SceneTools(PluginBase):
    @staticmethod
    def _find_actor_by_guid(scene, actor_guid: str):
        if scene is None or not actor_guid:
            return None
        try:
            for candidate in scene.get_actors():
                if getattr(candidate, "actor_guid", "") == actor_guid:
                    return candidate
        except Exception:
            pass
        try:
            return scene.find_actor(actor_guid)
        except Exception:
            return None

    @staticmethod
    def _actor_sync_state(actor) -> dict:
        try:
            data = actor.to_dict()
        except Exception:
            data = {}
        data.setdefault("name", getattr(actor, "name", ""))
        data.setdefault("actor_guid", getattr(actor, "actor_guid", ""))
        data.setdefault("path", getattr(actor, "route", ""))
        data.setdefault("model", getattr(actor, "model_path", data.get("path", "")))
        data.setdefault("model_dependencies", list(getattr(actor, "model_dependencies", []) or []))
        data.setdefault("actor_type", getattr(actor, "actor_type", "model"))
        data.setdefault("visible", SceneTools._safe_actor_call(actor, "get_visible", True))
        data.setdefault("follow_camera", SceneTools._safe_actor_call(actor, "get_follow_camera", False))
        geometry = data.setdefault("geometry", {})
        geometry.setdefault("position", SceneTools._safe_actor_call(actor, "get_position", [0.0, 0.0, 0.0]))
        geometry.setdefault("rotation", SceneTools._safe_actor_call(actor, "get_rotation", [0.0, 0.0, 0.0]))
        geometry.setdefault("scale", SceneTools._safe_actor_call(actor, "get_scale", [1.0, 1.0, 1.0]))
        return {
            "actor_guid": data.get("actor_guid", ""),
            "name": data.get("name", ""),
            "actor_type": data.get("actor_type", "model"),
            "path": data.get("path") or data.get("model") or "",
            "model": data.get("model") or data.get("path") or "",
            "model_dependencies": data.get("model_dependencies", []),
            "visible": data.get("visible", True),
            "follow_camera": data.get("follow_camera", False),
            "geometry": data.get("geometry", {}),
        }

    @staticmethod
    def _canonical_actor_sync_state(actor_data) -> dict:
        data = actor_data if isinstance(actor_data, dict) else {}
        geometry = data.get("geometry") if isinstance(data.get("geometry"), dict) else {}

        def list_or_default(value, default):
            return list(value) if value is not None else list(default)

        return {
            "actor_guid": data.get("actor_guid", ""),
            "name": data.get("name", ""),
            "actor_type": data.get("actor_type", "model"),
            "path": data.get("path") or data.get("model") or "",
            "model": data.get("model") or data.get("path") or "",
            "model_dependencies": list(data.get("model_dependencies") or []),
            "visible": data.get("visible", True),
            "follow_camera": data.get("follow_camera", False),
            "geometry": {
                "position": list_or_default(geometry.get("position"), [0.0, 0.0, 0.0]),
                "rotation": list_or_default(geometry.get("rotation"), [0.0, 0.0, 0.0]),
                "scale": list_or_default(geometry.get("scale"), [1.0, 1.0, 1.0]),
            },
        }

    @staticmethod
    def _actor_sync_states_equal(local_actor, remote_actor_data) -> bool:
        local_state = SceneTools._canonical_actor_sync_state(
            SceneTools._actor_sync_state(local_actor))
        remote_state = SceneTools._canonical_actor_sync_state(remote_actor_data)
        return local_state == remote_state

    @staticmethod
    def _actor_snapshot_block_reason(actor_data) -> str | None:
        if not isinstance(actor_data, dict):
            return network_sync_policy.actor_data_sync_block_reason(actor_data)
        policy_data = dict(actor_data)
        # The receiver adds this flag to prevent rebroadcast loops; it should not
        # make an otherwise valid host snapshot actor ineligible for apply.
        policy_data.pop("_suppress_network_broadcast", None)
        return network_sync_policy.actor_data_sync_block_reason(policy_data)

    @staticmethod
    def _safe_actor_call(actor, method_name: str, default=None):
        if actor is None or not hasattr(actor, method_name):
            return default
        try:
            return getattr(actor, method_name)()
        except Exception:
            return default

    @staticmethod
    def _camera_view_payload(scene, camera) -> dict:
        payload = camera.to_dict()
        payload["scene_id"] = scene.route
        return payload

    @staticmethod
    def create_actor(scene_name: str, asset_path: str, actor_type: str = 'model', actor_data=None) -> dict:
        # B-1 + 模型导入修复:场景不存在时不再崩溃,
        # 改为通过 get_or_create 自动补建并返回明确错误信息,避免前端静默失败
        return SceneTools._create_actor_impl(scene_name, asset_path, actor_type, actor_data)

    @staticmethod
    def create_actor_internal(scene_name: str, asset_path: str, actor_type: str = 'model', actor_data=None) -> dict:
        """纯后端 Actor 创建（不发 JS 回调，避免远程同步时触发前端死循环）。
        文件传输完成后由 C++ CEF bridge 调用此方法。"""
        return SceneTools._create_actor_impl(scene_name, asset_path, actor_type, actor_data,
                                             notify_frontend=False)

    @staticmethod
    def get_actor_sync_snapshot(scene_name: str) -> dict:
        try:
            scene = scene_manager.get(scene_name)
            if scene is None:
                return {"status": "error",
                        "message": f"Scene '{scene_name}' not found",
                        "code": "scene_not_found"}
            actors = []
            for actor in scene.get_actors():
                actor_state = SceneTools._actor_sync_state(actor)
                actor_state["scene"] = actor_state.get("scene") or scene_name
                if not actor_state.get("actor_guid"):
                    continue
                if SceneTools._actor_snapshot_block_reason(actor_state) is not None:
                    continue
                actors.append(actor_state)
            return {"status": "success", "scene": scene_name, "actors": actors}
        except Exception as exc:
            logger.exception("get_actor_sync_snapshot failed")
            return {"status": "error", "message": str(exc), "code": "internal_error"}

    @staticmethod
    def apply_actor_state_internal(scene_name: str, actor_guid: str, actor_data=None) -> dict:
        """Apply remote actor metadata and transform by actor_guid without rebroadcasting."""
        try:
            scene = scene_manager.get(scene_name)
            if scene is None:
                return {"status": "error",
                        "message": f"Scene '{scene_name}' not found",
                        "code": "scene_not_found"}
            actor = SceneTools._find_actor_by_guid(scene, actor_guid)
            if actor is None:
                return {"status": "warning",
                        "message": f"Actor guid '{actor_guid}' not found",
                        "code": "actor_not_found",
                        "actor_guid": actor_guid}

            actor_data = actor_data or {}
            previous_network_remote = getattr(actor, "network_remote", False)
            previous_suppress = getattr(actor, "_suppress_network_broadcast", False)
            actor.network_remote = True
            actor._suppress_network_broadcast = True
            try:
                if actor_data.get("name"):
                    actor.name = str(actor_data["name"])
                geometry = actor_data.get("geometry") or {}
                if "position" in geometry and hasattr(actor, "set_position"):
                    actor.set_position(geometry["position"], if_init=True)
                if "rotation" in geometry and hasattr(actor, "set_rotation"):
                    actor.set_rotation(geometry["rotation"], if_init=True)
                if "scale" in geometry and hasattr(actor, "set_scale"):
                    actor.set_scale(geometry["scale"], if_init=True)
                if "visible" in actor_data and hasattr(actor, "set_visible"):
                    actor.set_visible(actor_data["visible"])
                if "follow_camera" in actor_data and hasattr(actor, "set_follow_camera"):
                    actor.set_follow_camera(actor_data["follow_camera"], if_init=True)
            finally:
                actor.network_remote = previous_network_remote
                actor._suppress_network_broadcast = previous_suppress
            try:
                scene.save_data()
                if hasattr(scene, "_notify_scene_tree_changed"):
                    scene._notify_scene_tree_changed()
            except Exception:
                logger.debug("apply_actor_state_internal: save/notify failed", exc_info=True)
            return {"status": "success", "scene": scene_name, "actor": actor.to_dict()}
        except Exception as exc:
            logger.exception("apply_actor_state_internal failed")
            return {"status": "error", "message": str(exc), "code": "internal_error"}

    @staticmethod
    def apply_actor_sync_snapshot_internal(scene_name: str, snapshot=None) -> dict:
        """Apply a host actor snapshot. Missing local actors are created; absent host actors are never deleted."""
        try:
            scene = scene_manager.get(scene_name)
            if scene is None:
                return {"status": "error",
                        "message": f"Scene '{scene_name}' not found",
                        "code": "scene_not_found"}
            actors = snapshot.get("actors") if isinstance(snapshot, dict) else snapshot
            if not isinstance(actors, list):
                actors = []
            created = []
            updated = []
            unchanged = []
            warnings = []
            for actor_data in actors:
                if not isinstance(actor_data, dict):
                    continue
                actor_guid = actor_data.get("actor_guid", "")
                if not actor_guid:
                    continue
                block_reason = SceneTools._actor_snapshot_block_reason(actor_data)
                if block_reason is not None:
                    warnings.append({"status": "warning",
                                     "code": block_reason,
                                     "actor_guid": actor_guid,
                                     "actor": actor_data.get("name", "")})
                    continue
                existing = SceneTools._find_actor_by_guid(scene, actor_guid)
                if existing is not None:
                    if SceneTools._actor_sync_states_equal(existing, actor_data):
                        unchanged.append(SceneTools._actor_sync_state(existing))
                        continue
                    result = SceneTools.apply_actor_state_internal(
                        scene_name, actor_guid, actor_data)
                    if result.get("status") == "success":
                        updated.append(result.get("actor", {}))
                    else:
                        warnings.append(result)
                    continue

                asset_path = actor_data.get("path") or actor_data.get("model") or ""
                if not asset_path:
                    warnings.append({"status": "warning",
                                     "code": "missing_model_path",
                                     "actor_guid": actor_guid})
                    continue
                create_data = dict(actor_data)
                create_data["_suppress_network_broadcast"] = True
                try:
                    result = SceneTools.create_actor_internal(
                        scene_name,
                        asset_path,
                        create_data.get("actor_type", "model"),
                        create_data,
                    )
                except FileNotFoundError as exc:
                    logger.info(
                        "apply_actor_sync_snapshot_internal: skip actor with missing asset "
                        "scene=%s actor=%s guid=%s path=%s error=%s",
                        scene_name,
                        actor_data.get("name", ""),
                        actor_guid,
                        asset_path,
                        exc,
                    )
                    warnings.append({
                        "status": "warning",
                        "code": "missing_asset",
                        "actor_guid": actor_guid,
                        "actor": actor_data.get("name", ""),
                        "path": asset_path,
                        "message": str(exc),
                    })
                    continue
                actor_result = result.get("actor") if isinstance(result, dict) else None
                if actor_result:
                    created.append(actor_result)
                else:
                    warnings.append(result)
            return {
                "status": "success",
                "scene": scene_name,
                "created": created,
                "updated": updated,
                "unchanged": unchanged,
                "warnings": warnings,
            }
        except Exception as exc:
            logger.exception("apply_actor_sync_snapshot_internal failed")
            return {"status": "error", "message": str(exc), "code": "internal_error"}

    @staticmethod
    def apply_actor_transform_internal(scene_name: str, actor_guid: str, actor_data=None) -> dict:
        """Apply a remote actor transform without re-broadcasting it."""
        try:
            scene = scene_manager.get(scene_name)
            if scene is None:
                return {"status": "error",
                        "message": f"Scene '{scene_name}' not found",
                        "code": "scene_not_found"}

            actor = SceneTools._find_actor_by_guid(scene, actor_guid)
            if actor is None:
                return {"status": "error",
                        "message": f"Actor guid '{actor_guid}' not found",
                        "code": "actor_not_found"}

            geometry = (actor_data or {}).get("geometry") or {}
            if "position" in geometry:
                actor.set_position(geometry["position"], if_init=True)
            if "rotation" in geometry:
                actor.set_rotation(geometry["rotation"], if_init=True)
            if "scale" in geometry:
                actor.set_scale(geometry["scale"], if_init=True)
            try:
                scene.save_data()
            except Exception:
                logger.debug("apply_actor_transform_internal: save_data failed", exc_info=True)
            return {"status": "success", "scene": scene_name, "actor": actor.to_dict()}
        except Exception as exc:
            logger.exception("apply_actor_transform_internal failed")
            return {"status": "error", "message": str(exc), "code": "internal_error"}

    @staticmethod
    def _create_actor_impl(scene_name: str, asset_path: str, actor_type: str = 'model',
                           actor_data=None, notify_frontend: bool = True) -> dict:
        scene = scene_manager.get(scene_name)
        if scene is None:
            try:
                scene = scene_manager.get_or_create(scene_name)
            except Exception as exc:
                logger.error("create_actor: 场景 '%s' 不存在且无法创建: %s", scene_name, exc)
                return {"status": "error",
                        "message": f"Scene '{scene_name}' not found",
                        "code": "scene_not_found"}
        if scene is None:
            logger.error("create_actor: scene '%s' still None after get_or_create", scene_name)
            return {"status": "error",
                    "message": f"Scene '{scene_name}' not found",
                    "code": "scene_not_found"}

        existing_count = 0
        try:
            existing_count = sum(1 for a in scene._actors if a.route == asset_path)
        except Exception as exc:
            logger.warning("create_actor: 统计同路径 actor 失败 (%s),按 0 处理: %s", scene_name, exc)
            existing_count = 0

        actor_data = actor_data or {}
        actor_guid = actor_data.get("actor_guid", "") if isinstance(actor_data, dict) else ""
        existing_actor = SceneTools._find_actor_by_guid(scene, actor_guid)
        if existing_actor is not None:
            applied = SceneTools.apply_actor_state_internal(scene_name, actor_guid, actor_data)
            if applied.get("status") == "success":
                return {"scene": scene_name, "actor": applied.get("actor")}
            return applied

        actor = Actor(route=asset_path,
                      source_index=existing_count,
                      actor_type=actor_type,
                      parent_scene=scene,
                      actor_data=actor_data)
        scene.add_actor(actor)
        if actor_data.get("_suppress_network_broadcast"):
            actor.network_remote = False
            actor._suppress_network_broadcast = False
        logger.info("Actor %s added to %s type %s", actor.name, scene_name, actor_type)
        if notify_frontend:
            CoronaEditor.js_call_func("import-asset-complete", actor.to_dict())
        return {"scene": scene_name, "actor": actor.to_dict()}

    @staticmethod
    def create_scene(scene_name: str) -> dict:
        if not scene_name:
            raise ValueError("sceneName is required")
        scene = scene_manager.create(scene_name)
        scene.ensure_default_camera()
        actors = scene.get_actors()
        for actor in actors:
            actor._optics = Optics(actor._geometry)
            scene.add_actor(actor, True)
        return scene.to_dict()

    @staticmethod
    def remove_actor(scene_name: str, actor_name: str) -> dict:
        """从场景移除 Actor

        B-1 修复:不再 raise ValueError,改为返回 error dict
        与同模块其他方法(focus_actor / camera_move 等)保持一致,
        前端可统一通过 success===false / status==='error' 判定失败。
        """
        try:
            scene = scene_manager.get(scene_name)
            if scene is None:
                return {"status": "error",
                        "message": f"Scene '{scene_name}' not found",
                        "code": "scene_not_found"}
            actor = scene.find_actor(actor_name)
            if actor is None:
                return {"status": "error",
                        "message": f"Actor '{actor_name}' not found",
                        "code": "actor_not_found"}
            scene.remove_actor(actor)
            logger.info("Actor %s removed from %s", actor_name, scene_name)
            return {"status": "success", "scene": scene_name, "actor": actor_name}
        except Exception as exc:
            logger.exception("remove_actor 失败")
            return {"status": "error", "message": str(exc), "code": "internal_error"}

    @staticmethod
    def rename_actor(scene_name: str, actor_name: str, new_name: str) -> dict:
        try:
            scene = scene_manager.get(scene_name)
            if scene is None:
                raise ValueError(f"Scene '{scene_name}' not found")
            actor = scene.find_actor(actor_name)
            if actor is None:
                raise ValueError(f"Actor '{actor_name}' not found")
            normalized_name = str(new_name or "").strip()
            if not normalized_name:
                raise ValueError("Actor name cannot be empty")
            if any(other is not actor and other.name == normalized_name
                   for other in scene.get_actors()):
                raise ValueError(f"Actor name '{normalized_name}' already exists")

            old_name = actor.name
            actor.name = normalized_name
            scene.save_data()
            scene._notify_scene_tree_changed()
            try:
                if not getattr(actor, "_suppress_network_broadcast", False) and not getattr(actor, "network_remote", False):
                    CoronaEditor.js_call_func("actor-state-sync-broadcast", [actor.to_dict()])
            except Exception:
                logger.debug("rename_actor: state broadcast failed", exc_info=True)
            return {
                "status": "success",
                "scene": scene_name,
                "actor": actor.to_dict(),
                "old_name": old_name,
                "new_name": normalized_name,
            }
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def remove_actor_internal(scene_name: str, actor_guid: str = "", actor_name: str = "") -> dict:
        """Apply a remote actor deletion without re-broadcasting it."""
        try:
            scene = scene_manager.get(scene_name)
            if scene is None:
                return {"status": "error",
                        "message": f"Scene '{scene_name}' not found",
                        "code": "scene_not_found"}

            actor = None
            if actor_guid:
                for candidate in scene.get_actors():
                    if getattr(candidate, "actor_guid", "") == actor_guid:
                        actor = candidate
                        break
            if actor is None and actor_name:
                actor = scene.find_actor(actor_name)
            if actor is None and actor_guid:
                actor = scene.find_actor(actor_guid)
            if actor is None:
                return {"status": "warning",
                        "message": f"Actor '{actor_guid or actor_name}' not found",
                        "code": "actor_not_found",
                        "actor_guid": actor_guid,
                        "actor": actor_name}

            actor.network_remote = True
            actor._suppress_network_broadcast = True
            scene.remove_actor(actor)
            logger.info("Remote actor %s/%s removed from %s",
                        actor_guid, actor_name, scene_name)
            return {"status": "success",
                    "scene": scene_name,
                    "actor_guid": actor_guid,
                    "actor": actor_name or getattr(actor, "name", "")}
        except Exception as exc:
            logger.exception("remove_actor_internal failed")
            return {"status": "error", "message": str(exc), "code": "internal_error"}

    @staticmethod
    def sun_direction(scene_name: str, if_enable: bool, direction: list[float]) -> dict:
        try:
            scene = scene_manager.get(scene_name)
            scene.set_sun_direction(direction)
            logger.info("Sun direction set for %s", scene_name)
            return {"status": "success"}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def floor_grid(scene_name: str, enabled: bool) -> dict:
        try:
            scene = scene_manager.get(scene_name)
            scene.set_floor_grid(enabled)
            logger.info("Floor grid set for %s: %s", scene_name, enabled)
            return {"status": "success"}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def set_physics_params(scene_name: str, gravity: list = None, floor_y: float = None,
                           floor_restitution: float = None, fixed_dt: float = None) -> dict:
        """设置场景物理参数"""
        try:
            scene = scene_manager.get(scene_name)
            if gravity is not None:
                scene.set_gravity(gravity)
            if floor_y is not None:
                scene.set_floor_y(floor_y)
            if floor_restitution is not None:
                scene.set_floor_restitution(floor_restitution)
            if fixed_dt is not None:
                scene.set_fixed_dt(fixed_dt)
            logger.info("Physics params set for %s", scene_name)
            return {"status": "success"}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def get_physics_params(scene_name: str) -> dict:
        """获取场景物理参数"""
        try:
            scene = scene_manager.get(scene_name)
            return {
                "status": "success",
                "gravity": scene.get_gravity(),
                "floor_y": scene.get_floor_y(),
                "floor_restitution": scene.get_floor_restitution(),
                "fixed_dt": scene.get_fixed_dt(),
            }
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def save_screenshot(scene_name: str, path: str, camera_name: str = None) -> dict:
        try:
            scene = scene_manager.get(scene_name)
            if scene is None:
                raise ValueError(f"Scene '{scene_name}' not found")
            camera = scene.find_camera(camera_name)
            if camera is None:
                raise ValueError(f"Camera '{camera_name}' not found in scene '{scene_name}'")
            camera.save_screenshot(path)
            logger.info("Screenshot saved for scene %s camera %s to %s",
                        scene_name, getattr(camera, 'name', camera_name), path)
            return {"status": "success", "path": path}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def set_output_mode(scene_name: str, camera_name: str = None, mode: str = "final_color") -> dict:
        try:
            scene = scene_manager.get(scene_name)
            if scene is None:
                raise ValueError(f"Scene '{scene_name}' not found")
            camera = scene.find_camera(camera_name)
            if camera is None:
                raise ValueError(f"Camera '{camera_name}' not found in scene '{scene_name}'")
            camera.set_output_mode(mode)
            logger.info("Output mode set to '%s' for scene %s camera %s",
                        mode, scene_name, getattr(camera, 'name', camera_name))
            return {"status": "success", "mode": mode}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def get_output_mode(scene_name: str, camera_name: str = None) -> dict:
        try:
            scene = scene_manager.get(scene_name)
            if scene is None:
                raise ValueError(f"Scene '{scene_name}' not found")
            camera = scene.find_camera(camera_name)
            if camera is None:
                raise ValueError(f"Camera '{camera_name}' not found in scene '{scene_name}'")
            mode = camera.get_output_mode()
            return {"status": "success", "mode": mode}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def is_vision_available() -> dict:
        try:
            available = bool(CoronaEditor.CoronaEngine.is_vision_available())
            return {"status": "success", "available": available}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def set_render_backend(mode: str = "native", scene_name: str = None,
                           camera_name: str = None) -> dict:
        try:
            if scene_name:
                scene = scene_manager.get(scene_name)
                if scene is None:
                    raise ValueError(f"Scene '{scene_name}' not found")
                camera = scene.find_camera(camera_name)
                if camera is None:
                    raise ValueError(f"Camera '{camera_name}' not found")
                camera.set_render_backend(mode)
                actual = camera.get_render_backend()
            else:
                CoronaEditor.CoronaEngine.set_render_backend(mode)
                actual = CoronaEditor.CoronaEngine.get_render_backend()
            return {
                "status": "success",
                "mode": actual,
                "fallback": mode == "vision" and actual != "vision",
            }
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def get_render_backend(scene_name: str = None, camera_name: str = None) -> dict:
        try:
            if scene_name:
                scene = scene_manager.get(scene_name)
                if scene is None:
                    raise ValueError(f"Scene '{scene_name}' not found")
                camera = scene.find_camera(camera_name)
                if camera is None:
                    raise ValueError(f"Camera '{camera_name}' not found")
                mode = camera.get_render_backend()
            else:
                mode = CoronaEditor.CoronaEngine.get_render_backend()
            return {"status": "success", "mode": mode}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def set_vision_render_mode(scene_name: str, camera_name: str = None,
                               mode: str = "path_tracing") -> dict:
        try:
            scene = scene_manager.get(scene_name)
            if scene is None:
                raise ValueError(f"Scene '{scene_name}' not found")
            camera = scene.find_camera(camera_name)
            if camera is None:
                raise ValueError(f"Camera '{camera_name}' not found")
            camera.set_vision_render_mode(mode)
            actual = camera.get_vision_render_mode()
            logger.info("Vision render mode set to '%s' for scene %s camera %s",
                        actual, scene_name, getattr(camera, 'name', camera_name))
            return {"status": "success", "mode": actual}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def get_vision_render_mode(scene_name: str, camera_name: str = None) -> dict:
        try:
            scene = scene_manager.get(scene_name)
            if scene is None:
                raise ValueError(f"Scene '{scene_name}' not found")
            camera = scene.find_camera(camera_name)
            if camera is None:
                raise ValueError(f"Camera '{camera_name}' not found")
            return {"status": "success", "mode": camera.get_vision_render_mode()}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def prepare_external_live_vision_scene(scene) -> str:
        return prepare_external_live_vision_scene(scene)

    @staticmethod
    def create_camera_view(scene_name: str, name: str = None) -> dict:
        try:
            scene = scene_manager.get(scene_name)
            if scene is None:
                raise ValueError(f"Scene '{scene_name}' not found")
            source = scene.get_active_camera()
            if source is None:
                raise ValueError("No source camera is available")

            existing_names = {camera.name for camera in scene.get_cameras()}
            base_name = name or "Camera"
            candidate = base_name
            suffix = 1
            while candidate in existing_names:
                candidate = f"{base_name}_{suffix}"
                suffix += 1

            index = len(scene.get_cameras())
            camera = Camera(
                position=list(source.get_position()),
                forward=list(source.get_forward()),
                world_up=list(source.get_world_up()),
                fov=float(source.get_fov()),
                name=candidate,
                width=source.width,
                height=source.height,
                render_backend=source.get_render_backend(),
                output_mode=source.get_output_mode(),
                vision_render_mode=(
                    source.get_vision_render_mode()
                    if hasattr(source, "get_vision_render_mode")
                    else getattr(source, "vision_render_mode", "path_tracing")
                ),
                move_speed=source.move_speed,
                view_open=True,
                view_x=120 + index * 36,
                view_y=120 + index * 36,
                view_width=960,
                view_height=540,
            )
            camera.set_surface(0)
            scene.add_camera_to_scene(camera)
            scene._notify_scene_tree_changed()
            return {"status": "success", "camera": SceneTools._camera_view_payload(scene, camera)}
        except Exception as exc:
            logger.exception("create_camera_view failed")
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def open_camera_view(scene_name: str, camera_name: str) -> dict:
        try:
            scene = scene_manager.get(scene_name)
            camera = scene.find_camera(camera_name) if scene else None
            if camera is None:
                raise ValueError(f"Camera '{camera_name}' not found")
            camera.set_view_state(
                True, camera.view_x, camera.view_y,
                camera.view_width, camera.view_height, camera.move_speed)
            scene.save_data()
            return {"status": "success", "camera": SceneTools._camera_view_payload(scene, camera)}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def close_camera_view(scene_name: str, camera_name: str) -> dict:
        try:
            scene = scene_manager.get(scene_name)
            camera = scene.find_camera(camera_name) if scene else None
            if camera is None:
                raise ValueError(f"Camera '{camera_name}' not found")
            camera.refresh_view_state()
            camera.set_view_state(
                False, camera.view_x, camera.view_y,
                camera.view_width, camera.view_height, camera.move_speed)
            camera.set_surface(0)
            scene.save_data()
            return {"status": "success", "camera": SceneTools._camera_view_payload(scene, camera)}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def rename_camera_view(scene_name: str, camera_name: str, new_name: str) -> dict:
        try:
            scene = scene_manager.get(scene_name)
            camera = scene.find_camera(camera_name) if scene else None
            if camera is None:
                raise ValueError(f"Camera '{camera_name}' not found")
            if not new_name.strip():
                raise ValueError("Camera name cannot be empty")
            if any(other is not camera and other.name == new_name.strip()
                   for other in scene.get_cameras()):
                raise ValueError(f"Camera name '{new_name}' already exists")
            camera.name = new_name.strip()
            scene.save_data()
            scene._notify_scene_tree_changed()
            return {"status": "success", "camera": SceneTools._camera_view_payload(scene, camera)}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def list_camera_views(scene_name: str) -> dict:
        try:
            scene = scene_manager.get(scene_name)
            if scene is None:
                raise ValueError(f"Scene '{scene_name}' not found")
            return {
                "status": "success",
                "cameras": [
                    SceneTools._camera_view_payload(scene, camera)
                    for camera in scene.get_cameras()
                ],
            }
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def update_camera_view(scene_name: str, camera_name: str, state: dict) -> dict:
        try:
            scene = scene_manager.get(scene_name)
            camera = scene.find_camera(camera_name) if scene else None
            if camera is None:
                raise ValueError(f"Camera '{camera_name}' not found")
            camera.set_view_state(
                bool(state.get("view_open", camera.view_open)),
                int(state.get("view_x", camera.view_x)),
                int(state.get("view_y", camera.view_y)),
                int(state.get("view_width", camera.view_width)),
                int(state.get("view_height", camera.view_height)),
                float(state.get("move_speed", camera.move_speed)),
            )
            if "width" in state or "height" in state:
                camera.set_size(
                    int(state.get("width", camera.width)),
                    int(state.get("height", camera.height)))
            scene.save_data()
            return {"status": "success", "camera": SceneTools._camera_view_payload(scene, camera)}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def delete_camera(scene_name: str, camera_name: str) -> dict:
        try:
            scene = scene_manager.get(scene_name)
            camera = scene.find_camera(camera_name) if scene else None
            if camera is None:
                raise ValueError(f"Camera '{camera_name}' not found")
            if len(scene.get_cameras()) <= 1:
                raise ValueError("A scene must keep at least one camera")
            if not getattr(camera, 'deletable', True):
                raise ValueError("The main camera cannot be deleted")
            camera.set_surface(0)
            scene.remove_camera_from_scene(camera)
            scene._notify_scene_tree_changed()
            return {"status": "success", "camera_id": camera.camera_id}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def load_vision_scene(path: str = "") -> dict:
        try:
            if not CoronaEditor.CoronaEngine.is_vision_available():
                return {"status": "error", "message": "Vision backend is not available in this build"}
            CoronaEditor.CoronaEngine.load_vision_scene(path)
            logger.info("Vision scene load requested: %s", path or "<unload>")
            return {"status": "success", "path": path}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def import_vision_scene_into_current_scene(scene_name: str, path: str) -> dict:
        try:
            if not scene_name:
                return {"status": "error", "message": "scene_name is required"}
            if not path:
                return {"status": "error", "message": "Vision scene path is required"}
            if not CoronaEditor.CoronaEngine.is_vision_available():
                return {"status": "error", "message": "Vision backend is not available in this build"}

            abs_path = os.path.abspath(path)
            if not os.path.isfile(abs_path):
                return {"status": "error", "message": f"Vision scene file not found: {abs_path}"}

            with open(abs_path, "r", encoding="utf-8") as f:
                document = json.load(f)

            scene = scene_manager.get(scene_name)
            if scene is None:
                return {"status": "error", "message": f"Scene '{scene_name}' not found"}

            camera_pose = _extract_vision_camera_pose(document)
            scene.ensure_default_camera()
            active_camera = scene.get_active_camera()
            camera_imported = False
            if camera_pose is not None and active_camera is not None:
                active_camera.name = camera_pose["name"] or active_camera.name
                scene.set_camera(
                    camera_pose["position"],
                    camera_pose["forward"],
                    camera_pose["world_up"],
                    camera_pose["fov"],
                    active_camera.camera_id,
                )
                camera_imported = True

            active_camera = scene.get_active_camera()

            imported_vision_render_mode = _infer_vision_render_mode(document)
            if active_camera is not None and hasattr(active_camera, "set_vision_render_mode"):
                active_camera.set_vision_render_mode(imported_vision_render_mode)

            if "vision" not in scene.file_data:
                scene.file_data["vision"] = {}
            scene.vision_source_path = abs_path
            scene.vision_import_mode = "external_live"
            scene.file_data["vision"]["source_path"] = abs_path
            scene.file_data["vision"]["import_mode"] = "external_live"

            previous_bindings = list(getattr(scene, "vision_bindings", []))
            new_bindings = []
            unsupported_shapes = []
            created_proxy_count = 0
            reused_proxy_count = 0
            used_binding_indices = set()

            for shape_index, json_path, shape in _iter_vision_shapes(document):
                shape_type = _vision_shape_type(shape)
                model_path = ""
                actor_route = ""
                if shape_type == "model":
                    model_path = _resolve_vision_model_path(abs_path, shape)
                    actor_route = model_path
                    if not model_path or not os.path.isfile(model_path):
                        unsupported_shapes.append({
                            "shape_index": shape_index,
                            "json_path": json_path,
                            "type": shape_type,
                            "reason": "model_file_not_found",
                            "model_path": model_path,
                        })
                        continue
                elif shape_type in _SUPPORTED_VISION_PRIMITIVES:
                    actor_route, model_path = _ensure_vision_primitive_proxy(
                        scene, shape, shape_type, json_path)
                    if not actor_route:
                        unsupported_shapes.append({
                            "shape_index": shape_index,
                            "json_path": json_path,
                            "type": shape_type,
                            "reason": "primitive_proxy_generation_failed",
                        })
                        continue
                else:
                    unsupported_shapes.append({
                        "shape_index": shape_index,
                        "json_path": json_path,
                        "type": shape_type or "unknown",
                        "reason": "unsupported_shape_type",
                    })
                    continue

                previous_binding = _find_previous_binding(
                    previous_bindings,
                    used_binding_indices,
                    shape,
                    shape_type,
                    json_path,
                    abs_path,
                    model_path,
                )
                actor = _find_actor_by_guid(
                    scene, previous_binding.get("actor_guid", "") if previous_binding else "")
                transform = (
                    _extract_vision_primitive_proxy_transform(shape)
                    if shape_type in _SUPPORTED_VISION_PRIMITIVES
                    else _extract_vision_shape_transform(shape)
                )
                if actor is None:
                    actor_guid = (
                        previous_binding.get("actor_guid", "") if previous_binding else ""
                    ) or f"actor-{uuid.uuid4().hex}"
                    actor_data = {
                        "actor_guid": actor_guid,
                        "_suppress_network_broadcast": True,
                        "geometry": transform,
                    }
                    actor = Actor(
                        name=(_vision_proxy_name(shape, shape_type, shape_index)
                              if shape_type in _SUPPORTED_VISION_PRIMITIVES
                              else _vision_shape_name(shape, model_path, shape_index)),
                        route=actor_route,
                        actor_type="model",
                        parent_scene=scene,
                        actor_data=actor_data,
                    )
                    scene.add_actor(actor)
                    if hasattr(actor, "set_physics_enabled"):
                        try:
                            actor.set_physics_enabled(False)
                        except Exception as exc:
                            logger.warning("Failed to disable physics for Vision proxy %s: %s",
                                           getattr(actor, "name", actor_guid), exc)
                    created_proxy_count += 1
                else:
                    if hasattr(actor, "set_position"):
                        actor.set_position(transform["position"], True)
                    if hasattr(actor, "set_rotation"):
                        actor.set_rotation(transform["rotation"], True)
                    if hasattr(actor, "set_scale"):
                        actor.set_scale(transform["scale"], True)
                    if hasattr(actor, "set_physics_enabled"):
                        try:
                            actor.set_physics_enabled(False)
                        except Exception as exc:
                            logger.warning("Failed to disable physics for Vision proxy %s: %s",
                                           getattr(actor, "name", ""), exc)
                    reused_proxy_count += 1

                binding = {
                    "actor_guid": getattr(actor, "actor_guid", ""),
                    "actor_name": getattr(actor, "name", ""),
                    "shape_guid": _vision_shape_guid(shape, json_path),
                    "shape_index": shape_index,
                    "json_path": json_path,
                    "shape_type": shape_type,
                    "shape_identity_key": _vision_shape_identity_key(abs_path, shape, json_path),
                    "model_path": model_path,
                    "source_path": abs_path,
                }
                if shape_type == "model":
                    native_correction = _vision_model_native_local_correction(model_path)
                    if native_correction:
                        binding.update(native_correction)
                if hasattr(actor, "set_external_vision_binding"):
                    actor.set_external_vision_binding(binding)
                new_bindings.append(binding)

            active_actor_guids = {binding.get("actor_guid", "") for binding in new_bindings}
            removed_proxy_count = _remove_stale_vision_proxy_actors(
                scene, previous_bindings, active_actor_guids)
            scene.vision_bindings = new_bindings
            scene.vision_unsupported_shapes = unsupported_shapes
            scene.save_data()

            runtime_path = prepare_external_live_vision_scene(scene)
            CoronaEditor.CoronaEngine.load_vision_scene(runtime_path or abs_path)
            if active_camera is not None:
                active_camera.set_render_backend("vision")
                if hasattr(scene.engine_scene, "set_active_camera"):
                    scene.engine_scene.set_active_camera(getattr(active_camera, "engine_obj", active_camera))
                scene.save_data()
            scene._notify_scene_tree_changed()
            logger.info(
                "Vision scene imported into current scene %s: %s (created proxies=%d, reused=%d, removed=%d, unsupported=%d)",
                scene_name, abs_path, created_proxy_count, reused_proxy_count,
                removed_proxy_count, len(unsupported_shapes))
            vision_summary = _vision_import_summary(
                "external_live", abs_path, new_bindings, unsupported_shapes)
            return {
                "status": "success",
                "scene": scene_name,
                "path": abs_path,
                "runtime_path": runtime_path or abs_path,
                "import_mode": "external_live",
                "vision_render_mode": imported_vision_render_mode,
                "camera_imported": camera_imported,
                "camera": active_camera.to_dict() if active_camera is not None else None,
                "proxy_actors_created": created_proxy_count,
                "proxy_actors_reused": reused_proxy_count,
                "proxy_actors_removed": removed_proxy_count,
                "bindings": new_bindings,
                "unsupported_shapes": unsupported_shapes,
                "vision": vision_summary,
            }
        except json.JSONDecodeError as exc:
            return {"status": "error", "message": f"Invalid Vision JSON: {exc}"}
        except Exception as exc:
            logger.exception("import_vision_scene_into_current_scene failed")
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def select_vision_scene_path() -> dict:
        try:
            _content, path = FileHandler.open_file(
                caption="打开 Vision 场景",
                file_types="Vision 场景 (*.json)",
                read_content=False,
                return_relative_path=False,
            )
            if not path:
                return {"status": "canceled", "path": ""}
            return {"status": "success", "path": path}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def select_screenshot_path(scene_name: str, camera_name: str = None) -> dict:
        try:
            init_path = _active_project_path()
            import datetime
            timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            default_filename = f"screenshot_{timestamp}.png"

            path = FileHandler.choose_save_path(
                caption="保存截图",
                file_types="PNG 图片 (*.png)",
                default_dir=init_path,
                default_filename=default_filename,
                return_relative_path=False,
            )

            if not path:
                return {"status": "canceled", "path": ""}

            return {"status": "success", "path": path, "camera_name": camera_name}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def list_actor_tree(scene_name) -> list:
        scene = scene_manager.get(scene_name)
        final_list = []
        for actor in scene.get_actors():
            final_list.append({
                "name": actor.name,
                "path": actor.route,
                "type": actor.actor_type,
            })
        return final_list

    @staticmethod
    def list_scene_tree(scene_name: str) -> dict:
        scene = scene_manager.get(scene_name)
        if scene is None:
            raise ValueError(f"Scene '{scene_name}' not found")

        bindings = list(getattr(scene, "vision_bindings", []))
        unsupported_shapes = list(getattr(scene, "vision_unsupported_shapes", []))
        binding_by_actor_guid = {
            binding.get("actor_guid"): binding
            for binding in bindings
            if binding.get("actor_guid")
        }
        actors = []
        for actor in scene.get_actors():
            actor_guid = getattr(actor, "actor_guid", "")
            actor_info = {
                "name": actor.name,
                "path": actor.route,
                "type": actor.actor_type,
                "visible": actor.get_visible(),
                "handle": int(getattr(actor, "handle", 0) or 0),
                "actor_guid": actor_guid,
                "vision_proxy": actor_guid in binding_by_actor_guid,
            }
            if actor_guid in binding_by_actor_guid:
                actor_info["vision_binding"] = binding_by_actor_guid[actor_guid]
            actors.append(actor_info)

        cameras = []
        for cam in scene.get_cameras():
            camera_info = None
            if cam is not None and hasattr(cam, 'to_dict'):
                camera_info = cam.to_dict()
            elif cam is not None:
                camera_info = {"name": getattr(cam, 'name', 'Unknown')}
            cameras.append(camera_info)

        return {
            "actors": actors,
            "cameras": cameras,
            "vision": _vision_import_summary(
                getattr(scene, "vision_import_mode", ""),
                getattr(scene, "vision_source_path", ""),
                bindings,
                unsupported_shapes,
            ),
        }

    @staticmethod
    def focus_actor(scene_name: str, actor_name: str, camera_name: str = None) -> dict:
        """将摄像头聚焦到指定 Actor 上（基于 AABB 中心和尺寸）"""
        try:
            scene = scene_manager.get(scene_name)
            if scene is None:
                raise ValueError(f"Scene '{scene_name}' not found")

            actor = scene.find_actor(actor_name)
            if actor is None:
                raise ValueError(f"Actor '{actor_name}' not found in scene '{scene_name}'")

            camera = scene.find_camera(camera_name)
            if camera is None:
                raise ValueError(f"No camera available in scene '{scene_name}'")

            # 获取 actor AABB
            if not hasattr(actor, '_geometry') or actor._geometry is None:
                raise ValueError(f"Actor '{actor_name}' has no geometry")

            aabb = actor._geometry.get_aabb()  # [min_x, min_y, min_z, max_x, max_y, max_z] (模型空间)

            # 获取 Actor 世界变换
            actor_pos = actor.get_position()   # 世界位置
            actor_scale = actor.get_scale()    # 缩放

            # 将模型空间 AABB 中心转换到世界空间（忽略旋转的近似值）
            model_center = [
                (aabb[0] + aabb[3]) / 2.0,
                (aabb[1] + aabb[4]) / 2.0,
                (aabb[2] + aabb[5]) / 2.0,
            ]
            center = [
                actor_pos[0] + model_center[0] * actor_scale[0],
                actor_pos[1] + model_center[1] * actor_scale[1],
                actor_pos[2] + model_center[2] * actor_scale[2],
            ]

            # 计算世界空间 AABB 对角线长度
            dx = (aabb[3] - aabb[0]) * actor_scale[0]
            dy = (aabb[4] - aabb[1]) * actor_scale[1]
            dz = (aabb[5] - aabb[2]) * actor_scale[2]
            diagonal = math.sqrt(dx * dx + dy * dy + dz * dz)

            # 摄像头距离：对角线的 2 倍，最小为 1.0
            distance = max(diagonal * 2.0, 1.0)


            # 摄像头放在物体中心的 -Z 方向，朝向 +Z（看向物体中心）
            forward = [0.0, 0.0, 1.0]

            # 新摄像头位置 = 中心 - forward * 距离（即 center_z - distance）
            position = [
                center[0],
                center[1],
                center[2] - distance,
            ]

            up = [0.0, 1.0, 0.0]
            fov = camera.get_fov()

            camera.set(position, forward, up, fov)

            logger.info(
                "Camera focused on actor '%s': aabb=%s center=%s distance=%.2f pos=%s fwd=%s up=%s",
                actor_name, aabb, center, distance, position, forward, up,
            )
            return {"status": "success", "center": center, "distance": distance}
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def open_actor(scene_name: str, actor_name: str):
        try:
            scene = scene_manager.get(scene_name)
            if scene is None:
                logger.error(f"open_actor: scene '{scene_name}' not found")
                return False
            actor = scene.get_actor(actor_name)
            if actor is None:
                logger.error(f"open_actor: actor '{actor_name}' not found in scene '{scene_name}'")
                return False
            CoronaEditor.js_call_func("actor-change", [actor.actor_type, scene_name, actor_name])
            return True
        except Exception as e:
            logger.error(f"open actor error: {e}")
            return False

    @staticmethod
    def pick_actor_at_pixel(scene_name: str, x: float, y: float,
                            vp_width: float, vp_height: float) -> dict:
        """
        鼠标在3D视口中拾取物体。

        引擎的 pick_actor_at_pixel 是异步的：第一次调用设置GPU拾取请求并返回0，
        需要等待一帧（约16ms）后再次调用才能获取拾取结果。
        前端应在第一次调用后等待约50ms再重试。

        Args:
            scene_name: 场景名称
            x, y: 浏览器视口中的鼠标坐标 (event.clientX, event.clientY)
            vp_width, vp_height: 浏览器视口尺寸 (window.innerWidth, innerHeight)
        Returns:
            {"status": "success", "actor": {...}}  拾取成功
            {"status": "miss"}                      该位置没有物体
            {"status": "pending"}                   结果尚未就绪，需重试
            {"status": "error", "message": "..."}   出错
        """
        try:
            # 获取当前场景和活动摄像机
            scene = scene_manager.get(scene_name)
            if scene is None:
                return {"status": "error", "message": f"场景 '{scene_name}' 未找到"}

            camera = scene.get_active_camera()
            if camera is None:
                return {"status": "error", "message": "没有可用的摄像机"}

            # 坐标缩放：浏览器视口坐标 -> 摄像机渲染分辨率坐标
            cam_w = camera.width
            cam_h = camera.height
            if vp_width <= 0 or vp_height <= 0:
                return {"status": "error", "message": "无效的视口尺寸"}
            pick_x = int(x * cam_w / vp_width)
            pick_y = int(y * cam_h / vp_height)

            # 边界检查
            if pick_x < 0 or pick_x >= cam_w or pick_y < 0 or pick_y >= cam_h:
                return {"status": "miss"}

            # 调用引擎拾取API（第一次调用设置拾取请求，返回0或缓存的命中结果）
            handle = camera.pick_actor_at_pixel(pick_x, pick_y)

            if handle != 0:
                # 命中物体：通过handle查找对应的Python Actor对象
                from CoronaCore.core.entities.actor import _handle_to_actor
                actor = _handle_to_actor.get(handle)
                if actor is not None:
                    # 设置选中状态并通知前端更新属性面板
                    CoronaEditor._selected_scene = scene_name
                    CoronaEditor._selected_actor = actor.name
                    CoronaEditor.js_call_func(
                        "actor-change",
                        [actor.actor_type, scene_name, actor.name]
                    )
                    logger.info(
                        "Viewport pick hit: actor='%s' at pixel (%d, %d)",
                        actor.name, pick_x, pick_y
                    )
                    return {"status": "success", "actor": actor.to_dict()}
                else:
                    # handle存在但Python对象已被GC回收
                    logger.warning(
                        "Viewport pick: handle %d 未找到对应的 Python Actor", handle
                    )
                    return {"status": "miss"}

            # handle == 0：无缓存结果，拾取请求已提交，需等待下一帧
            return {"status": "pending"}

        except Exception as e:
            logger.error("pick_actor_at_pixel error: %s", e)
            return {"status": "error", "message": str(e)}

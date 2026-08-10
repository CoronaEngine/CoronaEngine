"""Pure Vision document parsing and coordinate conversion helpers.

This module contains no Scene, Actor, C++ binding, or persistence access. The
SceneTools aggregate handler owns orchestration; this module owns only the
shape/camera/document value conversion used by that orchestration.
"""

import copy
import base64
import json
import math
import os
import zlib


_VISION_RESOURCE_PATH_KEYS = {
    "fn",
    "path",
    "file",
    "filename",
    "texture",
    "image",
}

VISION_DOCUMENT_VERSION = "1"
VISION_DOCUMENT_ENCODING = "zlib_base64_json"


def encode_vision_document(document: dict) -> str:
    payload = json.dumps(document, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    compressor = zlib.compressobj(level=0)
    return base64.b64encode(compressor.compress(payload) + compressor.flush()).decode("ascii")


def decode_vision_document(data: str) -> dict:
    payload = zlib.decompress(base64.b64decode(data.encode("ascii"))).decode("utf-8")
    document = json.loads(payload)
    if not isinstance(document, dict):
        raise ValueError("Vision document must decode to a JSON object")
    return document


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


def vision_document_for_embedded_storage(document: dict, source_path: str) -> dict:
    base_dir = os.path.dirname(os.path.abspath(source_path)) if source_path else ""
    embedded = copy.deepcopy(document)

    def absolutize(value):
        if isinstance(value, dict):
            for key, child in list(value.items()):
                if (key or "").lower() in _VISION_RESOURCE_PATH_KEYS and isinstance(child, str):
                    text = child.strip()
                    if base_dir and text and not os.path.isabs(text) and "://" not in text and not text.startswith("data:"):
                        value[key] = os.path.abspath(os.path.join(base_dir, text))
                    else:
                        value[key] = child
                else:
                    absolutize(child)
        elif isinstance(value, list):
            for item in value:
                absolutize(item)

    absolutize(embedded)
    return embedded


def extract_vision_camera_pose(document: dict):
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


def infer_vision_render_mode(document: dict) -> str:
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
    integrator_param = integrator.get("param") if isinstance(integrator, dict) else {}
    denoiser = integrator_param.get("denoiser") if isinstance(integrator_param, dict) else {}
    denoiser_type = (
        str(denoiser.get("type") or "").strip().lower()
        if isinstance(denoiser, dict)
        else ""
    )

    pipeline = document.get("pipeline")
    pipeline_param = pipeline.get("param") if isinstance(pipeline, dict) else {}
    frame_buffer = pipeline_param.get("frame_buffer") if isinstance(pipeline_param, dict) else {}
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


def vision_scene_data(document: dict) -> dict:
    return document.get("scene", document) if isinstance(document, dict) else {}


def iter_vision_shapes(document: dict):
    scene_data = vision_scene_data(document)
    shapes = scene_data.get("shapes", [])
    if isinstance(shapes, list):
        for index, shape in enumerate(shapes):
            if isinstance(shape, dict):
                yield index, f"/scene/shapes/{index}", shape
    elif isinstance(shapes, dict):
        for index, (key, shape) in enumerate(shapes.items()):
            if isinstance(shape, dict):
                yield index, f"/scene/shapes/{key}", shape


def vision_shape_params(shape: dict) -> dict:
    params = shape.get("param")
    return params if isinstance(params, dict) else {}


def vision_shape_type(shape: dict) -> str:
    return str(shape.get("type") or shape.get("shape_type") or "").strip().lower()


def vision_shape_name(shape: dict, model_path: str, shape_index: int) -> str:
    raw = shape.get("name")
    if isinstance(raw, str) and raw.strip():
        return raw.strip()
    stem = os.path.splitext(os.path.basename(model_path))[0]
    return stem or f"vision_shape_{shape_index}"


def resolve_vision_model_path(scene_path: str, shape: dict) -> str:
    params = vision_shape_params(shape)
    model_path = params.get("fn") or shape.get("fn") or params.get("path") or shape.get("path")
    if not isinstance(model_path, str) or not model_path.strip():
        return ""
    model_path = model_path.strip()
    if os.path.isabs(model_path):
        return os.path.abspath(model_path)
    return os.path.abspath(os.path.join(os.path.dirname(scene_path), model_path))


def shape_collection(document: dict):
    scene_data = vision_scene_data(document)
    shapes = scene_data.get("shapes", [])
    return shapes if isinstance(shapes, (list, dict)) else []


def remove_shape_at_json_path(document: dict, json_path: str) -> bool:
    shapes = shape_collection(document)
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


def shape_at_json_path(document: dict, json_path: str):
    shapes = shape_collection(document)
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


def compact_removed_shapes(document: dict) -> None:
    scene_data = vision_scene_data(document)
    shapes = scene_data.get("shapes", [])
    if isinstance(shapes, list):
        scene_data["shapes"] = [shape for shape in shapes if shape is not None]

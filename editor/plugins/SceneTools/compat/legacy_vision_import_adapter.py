"""Compatibility owner for importing Vision documents into legacy Python Scenes.

New editor code must use the native Scene/Vision contracts.  This module keeps
the old Web API behavior available for hosts that still provide Python Scene
objects, while keeping that dependency out of the active SceneTools facade.
"""

import copy
import json
import logging
import os
import uuid

from api.editor_api import CoronaEditorApi, get_active_project_path
from plugins.SceneTools.compat.legacy_vision_scene_adapter import get_legacy_vision_scene
from plugins.SceneTools.vision_document import (
    extract_vision_camera_pose as _extract_vision_camera_pose,
    infer_vision_render_mode as _infer_vision_render_mode,
    iter_vision_shapes as _iter_vision_shapes,
    resolve_vision_model_path as _resolve_vision_model_path,
    vision_document_for_embedded_storage as _vision_document_for_embedded_storage,
    compact_removed_shapes as _compact_removed_shapes,
    remove_shape_at_json_path as _remove_shape_at_json_path,
    shape_at_json_path as _shape_at_json_path,
    vision_shape_name as _vision_shape_name,
    vision_shape_type as _vision_shape_type,
)
from plugins.SceneTools.vision_geometry import (
    corona_trs_to_vision_matrix4x4 as _corona_trs_to_vision_matrix4x4,
    extract_vision_shape_transform as _extract_vision_shape_transform,
)
from plugins.SceneTools.vision_storage import (
    atomic_write_json as _atomic_write_json,
    derived_vision_scene_path as _derived_vision_scene_path,
    runtime_vision_scene_path as _runtime_vision_scene_path,
)
from plugins.SceneTools.vision_proxy import (
    vision_model_native_local_correction as _vision_model_native_local_correction,
    write_vision_primitive_proxy,
)
from plugins.SceneTools.vision_bindings import (
    find_previous_binding as _find_previous_binding,
    vision_import_summary as _vision_import_summary,
    vision_shape_guid as _vision_shape_guid,
    vision_shape_identity_key as _vision_shape_identity_key,
)
from plugins.SceneTools.compat.legacy_vision_binding_sync import (
    find_actor_by_guid as _find_actor_by_guid,
    remove_stale_vision_proxy_actors as _remove_stale_vision_proxy_actors,
    sync_external_live_binding_source_path as _sync_external_live_binding_source_path,
)

logger = logging.getLogger(__name__)
_SUPPORTED_VISION_PRIMITIVES = {"quad", "cube", "sphere"}


def _active_project_path():
    try:
        return get_active_project_path()
    except Exception:
        return ""


def _as_float3(value):
    if not isinstance(value, (list, tuple)) or len(value) < 3:
        return None
    try:
        return [float(value[0]), float(value[1]), float(value[2])]
    except (TypeError, ValueError):
        return None


def _vision_proxy_name(shape: dict, shape_type: str, shape_index: int) -> str:
    raw = shape.get("name")
    if isinstance(raw, str) and raw.strip():
        return raw.strip()
    return f"vision_{shape_type or 'shape'}_{shape_index}"


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


def _ensure_vision_primitive_proxy(scene, shape: dict, shape_type: str, json_path: str):
    project_root = _project_root_for_scene(scene)
    if not project_root:
        return "", ""
    shape_guid = _vision_shape_guid(shape, json_path)
    return write_vision_primitive_proxy(
        project_root, shape, shape_type, json_path, shape_guid
    )


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


def _write_derived_external_live_scene(scene, source_path: str) -> str:
    embedded_document = getattr(scene, "vision_document", None)
    if isinstance(embedded_document, dict):
        document = embedded_document
        output_path = _runtime_vision_scene_path(scene, _active_project_path())
    else:
        if not source_path or not os.path.isfile(source_path):
            return source_path
        with open(source_path, "r", encoding="utf-8") as file:
            document = json.load(file)
        output_path = _derived_vision_scene_path(source_path, scene)

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
            "param": {"matrix4x4": _corona_trs_to_vision_matrix4x4(_actor_transform(actor))},
        }

    _compact_removed_shapes(derived)
    _atomic_write_json(output_path, derived)
    return output_path


def prepare_external_live_vision_scene(scene) -> str:
    if isinstance(getattr(scene, "vision_document", None), dict):
        try:
            runtime_path = _write_derived_external_live_scene(scene, "")
        except Exception as exc:
            logger.exception("Failed to write embedded Vision runtime scene: %s", exc)
            return ""
        _sync_external_live_binding_source_path(
            scene, runtime_path, getattr(scene, "vision_bindings", [])
        )
        return runtime_path

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
    _sync_external_live_binding_source_path(
        scene, runtime_path, getattr(scene, "vision_bindings", [])
    )
    return runtime_path


def import_vision_scene_into_current_scene(scene_name: str, path: str) -> dict:
    try:
        if not scene_name:
            return {"status": "error", "message": "scene_name is required"}
        if not path:
            return {"status": "error", "message": "Vision scene path is required"}
        vision_status = CoronaEditorApi.scene_tools.is_vision_available()
        if not isinstance(vision_status, dict) or not vision_status.get("available"):
            return {"status": "error", "message": "Vision backend is not available in this build"}

        abs_path = os.path.abspath(path)
        if not os.path.isfile(abs_path):
            return {"status": "error", "message": f"Vision scene file not found: {abs_path}"}
        with open(abs_path, "r", encoding="utf-8") as file:
            document = json.load(file)

        scene = get_legacy_vision_scene(scene_name)
        if scene is None:
            return {"status": "error", "message": f"Scene '{scene_name}' not found"}

        camera_pose = _extract_vision_camera_pose(document)
        scene.ensure_default_camera()
        active_camera = scene.get_active_camera()
        camera_imported = False
        if camera_pose is not None and active_camera is not None:
            active_camera.name = camera_pose["name"] or active_camera.name
            scene.set_camera(
                camera_pose["position"], camera_pose["forward"], camera_pose["world_up"],
                camera_pose["fov"], active_camera.camera_id,
            )
            camera_imported = True

        active_camera = scene.get_active_camera()
        imported_vision_render_mode = _infer_vision_render_mode(document)
        if active_camera is not None and hasattr(active_camera, "set_vision_render_mode"):
            active_camera.set_vision_render_mode(imported_vision_render_mode)

        scene.vision_document = _vision_document_for_embedded_storage(document, abs_path)
        scene.vision_source_path = ""
        scene.vision_import_mode = ""
        if "vision" in scene.file_data:
            scene.file_data.remove_section("vision")

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
                    unsupported_shapes.append({"shape_index": shape_index, "json_path": json_path,
                                               "type": shape_type, "reason": "model_file_not_found",
                                               "model_path": model_path})
                    continue
            elif shape_type in _SUPPORTED_VISION_PRIMITIVES:
                actor_route, model_path = _ensure_vision_primitive_proxy(scene, shape, shape_type, json_path)
                if not actor_route:
                    unsupported_shapes.append({"shape_index": shape_index, "json_path": json_path,
                                               "type": shape_type, "reason": "primitive_proxy_generation_failed"})
                    continue
            else:
                unsupported_shapes.append({"shape_index": shape_index, "json_path": json_path,
                                           "type": shape_type or "unknown", "reason": "unsupported_shape_type"})
                continue

            previous_binding = _find_previous_binding(
                previous_bindings, used_binding_indices, shape, shape_type,
                json_path, abs_path, model_path,
            )
            transform = _extract_vision_shape_transform(shape)
            actor_guid = (previous_binding.get("actor_guid", "") if previous_binding else "") or f"actor-{uuid.uuid4().hex}"
            actor_name = _vision_proxy_name(shape, shape_type, shape_index) if shape_type in _SUPPORTED_VISION_PRIMITIVES else _vision_shape_name(shape, model_path, shape_index)
            actor_data = {
                "actor_guid": actor_guid, "actor_name": actor_name, "name": actor_name,
                "geometry": transform, "physics_enabled": False,
                "skip_if_exists": True, "update_if_exists": True,
            }
            native_result = CoronaEditorApi.scene_tools.create_actor(
                getattr(scene, "route", scene_name), actor_route, "model", actor_data
            )
            if not isinstance(native_result, dict) or native_result.get("status") not in ("success", "ok"):
                raise RuntimeError(str(native_result.get("message") if isinstance(native_result, dict) else native_result))
            native_actor = native_result.get("actor") or {}
            if native_result.get("existed"):
                reused_proxy_count += 1
            else:
                created_proxy_count += 1

            binding = {
                "actor_guid": native_actor.get("actor_guid", actor_guid),
                "actor_name": native_actor.get("name", actor_name),
                "shape_guid": _vision_shape_guid(shape, json_path),
                "shape_index": shape_index, "json_path": json_path, "shape_type": shape_type,
                "shape_identity_key": _vision_shape_identity_key(abs_path, shape, json_path),
                "model_path": model_path,
            }
            if shape_type == "model":
                native_correction = _vision_model_native_local_correction(model_path)
                if native_correction:
                    binding.update(native_correction)
            new_bindings.append(binding)

        active_actor_guids = {binding.get("actor_guid", "") for binding in new_bindings}
        removed_proxy_count = _remove_stale_vision_proxy_actors(scene, previous_bindings, active_actor_guids)
        scene.vision_bindings = new_bindings
        scene.vision_unsupported_shapes = unsupported_shapes
        scene.save_data()

        runtime_path = prepare_external_live_vision_scene(scene)
        CoronaEditorApi.scene_tools.load_vision_scene(runtime_path or abs_path)
        if active_camera is not None:
            CoronaEditorApi.scene_tools.set_render_backend("vision", scene_name, getattr(active_camera, "name", ""))
            scene.save_data()
        scene._notify_scene_tree_changed()
        logger.info(
            "Vision scene imported into current scene %s: %s (created proxies=%d, reused=%d, removed=%d, unsupported=%d)",
            scene_name, abs_path, created_proxy_count, reused_proxy_count,
            removed_proxy_count, len(unsupported_shapes),
        )
        vision_summary = _vision_import_summary("embedded", new_bindings, unsupported_shapes)
        return {
            "status": "success", "scene": scene_name, "path": abs_path,
            "runtime_path": runtime_path or abs_path, "storage": "embedded",
            "vision_render_mode": imported_vision_render_mode, "camera_imported": camera_imported,
            "camera": active_camera.to_dict() if active_camera is not None else None,
            "proxy_actors_created": created_proxy_count, "proxy_actors_reused": reused_proxy_count,
            "proxy_actors_removed": removed_proxy_count, "bindings": new_bindings,
            "unsupported_shapes": unsupported_shapes, "vision": vision_summary,
        }
    except json.JSONDecodeError as exc:
        return {"status": "error", "message": f"Invalid Vision JSON: {exc}"}
    except Exception as exc:
        logger.exception("import_vision_scene_into_current_scene failed")
        return {"status": "error", "message": str(exc)}


def import_embedded_vision_scene_into_current_scene(scene_name: str) -> dict:
    try:
        scene = get_legacy_vision_scene(scene_name)
        if scene is None:
            return {"status": "error", "message": f"Scene '{scene_name}' not found"}
        if not isinstance(getattr(scene, "vision_document", None), dict):
            return {"status": "error", "message": "Scene has no embedded Vision document"}
        runtime_path = prepare_external_live_vision_scene(scene)
        if not runtime_path:
            return {"status": "error", "message": "Failed to prepare embedded Vision runtime scene"}
        return import_vision_scene_into_current_scene(scene_name, runtime_path)
    except Exception as exc:
        logger.exception("import_embedded_vision_scene_into_current_scene failed")
        return {"status": "error", "message": str(exc)}

"""Native SceneTools owner for importing Vision documents.

The importer uses native scene snapshots, aggregate mutations and scene_save;
it does not resolve or mutate legacy Python Scene objects.
"""

import copy
import json
import logging
import os

from api.editor_api import CoronaEditorApi, get_active_project_path
from plugins.SceneTools.vision_document import (
    extract_vision_camera_pose as _extract_vision_camera_pose,
    infer_vision_render_mode as _infer_vision_render_mode,
    iter_vision_shapes as _iter_vision_shapes,
    resolve_vision_model_path as _resolve_vision_model_path,
    vision_document_for_embedded_storage as _vision_document_for_embedded_storage,
    encode_vision_document as _encode_vision_document,
    decode_vision_document as _decode_vision_document,
    VISION_DOCUMENT_ENCODING as _VISION_DOCUMENT_ENCODING,
    VISION_DOCUMENT_VERSION as _VISION_DOCUMENT_VERSION,
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
from plugins.SceneTools.vision_binding_sync import (
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


class _NativeVisionCamera:
    def __init__(self, scene, data: dict):
        self._scene = scene
        self._data = dict(data or {})
        self.name = self._data.get("name", "")
        self.camera_id = self._data.get("camera_id", self.name)

    def set_vision_render_mode(self, mode: str):
        return CoronaEditorApi.scene_tools.set_vision_render_mode(
            self._scene.route, self.name, mode
        )

    def set_camera(self, position, forward, world_up, fov):
        return CoronaEditorApi.viewport.set_camera_pose(
            self._scene.route,
            self.name,
            {
                "position": list(position),
                "forward": list(forward),
                "world_up": list(world_up),
                "fov": float(fov),
                "persist": True,
            },
        )

    def to_dict(self):
        return dict(self._data)


class _NativeVisionActor:
    def __init__(self, data: dict):
        self._data = dict(data or {})
        self.actor_guid = self._data.get("actor_guid", "")
        self.name = self._data.get("name", "")
        self.route = self._data.get("route", self._data.get("path", ""))

    def get_position(self):
        return list((self._data.get("geometry") or {}).get("position", [0.0, 0.0, 0.0]))

    def get_rotation(self):
        return list((self._data.get("geometry") or {}).get("rotation", [0.0, 0.0, 0.0]))

    def get_scale(self):
        return list((self._data.get("geometry") or {}).get("scale", [1.0, 1.0, 1.0]))

    # Native C++ owns external Vision binding metadata. These methods exist
    # only so the pure derived-scene writer can consume a value object.
    def set_external_vision_binding(self, binding):
        return None

    def clear_external_vision_binding(self):
        return None


class _NativeVisionScene:
    """Small native snapshot/value facade used by Vision orchestration."""

    def __init__(self, snapshot: dict):
        self.route = str(snapshot.get("scene") or snapshot.get("scene_id") or "")
        self.name = str(snapshot.get("scene_name") or self.route)
        self._snapshot = snapshot
        self._vision = snapshot.get("vision") if isinstance(snapshot.get("vision"), dict) else {}
        self.vision_storage = self._vision.get("storage", "")
        self.vision_source_id = self._vision.get("source_id", "")
        self.vision_source_path = self._vision.get("source_path", "")
        self.vision_import_mode = self._vision.get("import_mode", "")
        self.vision_document_asset_root = self._vision.get("document_asset_root", "")
        encoded = self._vision.get("document_data", "")
        try:
            self.vision_document = _decode_vision_document(encoded) if encoded else None
        except Exception:
            self.vision_document = None
        self.vision_bindings = []
        self.vision_unsupported_shapes = []
        self.file_data = {}

    def ensure_default_camera(self):
        return None

    def get_active_camera(self):
        camera = self._snapshot.get("camera")
        return _NativeVisionCamera(self, camera) if isinstance(camera, dict) else None

    def set_camera(self, position, forward, world_up, fov, camera_id=""):
        camera = self.get_active_camera()
        return camera.set_camera(position, forward, world_up, fov) if camera else None

    def get_actors(self):
        actors = self._snapshot.get("actors")
        return [_NativeVisionActor(actor) for actor in actors or [] if isinstance(actor, dict)]

    def find_actor(self, actor_name):
        return next((actor for actor in self.get_actors() if actor.name == actor_name or actor.actor_guid == actor_name), None)

    def remove_actor(self, actor):
        return CoronaEditorApi.scene_tools.remove_actor(self.route, actor.actor_guid or actor.name)

    def save_data(self):
        document = self.vision_document if isinstance(self.vision_document, dict) else {}
        return CoronaEditorApi.main.scene_save(
            self.route,
            {
                "vision": {
                    "storage": self.vision_storage,
                    "source_id": self.vision_source_id,
                    "import_mode": self.vision_import_mode,
                },
                "vision_document": {
                    "version": _VISION_DOCUMENT_VERSION,
                    "encoding": _VISION_DOCUMENT_ENCODING,
                    "asset_root": self.vision_document_asset_root,
                    "data": _encode_vision_document(document),
                },
            },
        )

    def _notify_scene_tree_changed(self):
        return None


def _native_scene(scene_name: str) -> _NativeVisionScene | None:
    raw = CoronaEditorApi.scene.get_snapshot(scene_name)
    snapshot = json.loads(raw) if isinstance(raw, str) else raw
    if not isinstance(snapshot, dict) or snapshot.get("status") in ("error", "failed"):
        return None
    return _NativeVisionScene(snapshot)


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

        scene = _native_scene(scene_name)
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
        scene.vision_storage = "embedded"
        if "vision" in scene.file_data:
            scene.file_data.remove_section("vision")

        # Publish the document through the native scene aggregate before
        # creating actors. Native C++ then owns persistence and binding sync.
        scene.save_data()

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
            actor_guid = (previous_binding.get("actor_guid", "") if previous_binding else "") or _vision_shape_guid(shape, json_path)
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
        scene = _native_scene(scene_name)
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

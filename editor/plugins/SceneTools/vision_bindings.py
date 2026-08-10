"""Vision shape identity, binding matching and import summaries."""

import json
import os

from .vision_document import (
    resolve_vision_model_path,
    vision_shape_params,
    vision_shape_type,
)


def vision_shape_declared_guid(shape: dict) -> str:
    for key in ("shape_guid", "guid", "id"):
        value = shape.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return ""


def json_identity(value) -> str:
    try:
        return json.dumps(value, sort_keys=True, separators=(",", ":"))
    except TypeError:
        return str(value)


def vision_shape_guid(shape: dict, json_path: str) -> str:
    declared_guid = vision_shape_declared_guid(shape)
    if declared_guid:
        return declared_guid
    import uuid

    return f"vision-shape-{uuid.uuid5(uuid.NAMESPACE_URL, json_path).hex}"


def vision_shape_identity_key(scene_path: str, shape: dict, json_path: str) -> str:
    declared_guid = vision_shape_declared_guid(shape)
    if declared_guid:
        return f"guid:{declared_guid}"

    shape_type = vision_shape_type(shape)
    if shape_type == "model":
        model_path = resolve_vision_model_path(scene_path, shape)
        if model_path:
            return f"model:{os.path.normcase(os.path.abspath(model_path))}"

    params = dict(vision_shape_params(shape))
    params.pop("transform", None)
    payload = {
        "type": shape_type,
        "params": params,
        "fn": shape.get("fn"),
        "path": shape.get("path"),
    }
    return f"shape:{json_identity(payload)}"


def binding_is_compatible(
    binding: dict, shape_type: str, model_path: str, identity_key: str
) -> bool:
    if binding.get("shape_type") and binding.get("shape_type") != shape_type:
        return False
    if model_path and binding.get("model_path") and (
        os.path.normcase(os.path.abspath(binding.get("model_path")))
        != os.path.normcase(os.path.abspath(model_path))
    ):
        return False
    if binding.get("shape_identity_key"):
        return binding.get("shape_identity_key") == identity_key
    return True


def find_previous_binding(
    bindings,
    used_binding_indices: set,
    shape: dict,
    shape_type: str,
    json_path: str,
    scene_path: str,
    model_path: str,
):
    current_guid = vision_shape_guid(shape, json_path)
    current_identity_key = vision_shape_identity_key(scene_path, shape, json_path)
    declared_guid = vision_shape_declared_guid(shape)

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
        if not binding_is_compatible(binding, shape_type, model_path, current_identity_key):
            continue
        used_binding_indices.add(index)
        return binding
    return None


def vision_import_summary(storage: str, bindings, unsupported_shapes) -> dict:
    unsupported_by_reason = {}
    unsupported_by_type = {}
    for shape in unsupported_shapes or []:
        reason = shape.get("reason") or "unknown"
        shape_type = shape.get("type") or "unknown"
        unsupported_by_reason[reason] = unsupported_by_reason.get(reason, 0) + 1
        unsupported_by_type[shape_type] = unsupported_by_type.get(shape_type, 0) + 1
    return {
        "storage": storage,
        "embedded": storage == "embedded",
        "binding_count": len(bindings or []),
        "unsupported_count": len(unsupported_shapes or []),
        "unsupported_by_reason": unsupported_by_reason,
        "unsupported_by_type": unsupported_by_type,
        "unsupported_shapes": list(unsupported_shapes or []),
    }

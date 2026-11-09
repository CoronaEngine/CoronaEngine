"""Write live editor values into a portable scene without touching unrelated files."""

import json
import re
from pathlib import Path

from .parser import _actor_fields, _resolve_source
from .portable import recover_scene, validate_assets, write_ini_sections


def _value(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (list, dict)):
        return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
    return str(value)


def _vector(values):
    x, y, z = values
    return ", ".join(format(float(v), ".9g") for v in (x, y, z))


def _actor_section(actors, original):
    old = _actor_fields(original)
    by_guid = {fields.get(key + ".actor_guid"): key for key, fields in old.items()
               if fields.get(key + ".actor_guid")}
    used = set()
    section = {}
    for index, actor in enumerate(actors):
        name = actor["name"]
        old_key = by_guid.get(actor["actor_guid"])
        if old_key is None:
            # Legacy entries have no GUID. A replacement Actor with a new GUID
            # must not inherit extension fields from a removed namesake.
            old_key = next((k for k, f in old.items() if not f.get(k + ".actor_guid")
                            and f.get(k + ".name", k) == name), None)
        key = old_key or name
        if not key or any(c in key for c in ".=:\r\n[]#;") or key.casefold() in used:
            key = f"actor{index + 1}"
            while key.casefold() in used or (key in old and key != old_key):
                key += "_"
        used.add(key.casefold())
        # Preserve extension fields from the previous actor, including across renames.
        fields = {k.partition(".")[2]: v for k, v in old.get(old_key, {}).items()}
        fields.update({k: actor[k] for k in ("actor_guid", "actor_type", "name", "route", "follow_camera")})
        for k in ("entity_id", "asset_id", "model_ref", "entity_type", "semantic_role", "source_plan_id",
                  "source_batch_id", "source_scene_version", "actor_version"):
            fields["runtime." + k] = actor[k]
        for k in ("position", "rotation", "scale"):
            fields["geometry." + k] = _vector(actor["geometry"][k])
        lock = actor["camera_lock"]
        fields["camera_lock.enabled"] = lock["enabled"]
        for k in ("position_offset", "rotation_offset"):
            fields["camera_lock." + k] = _vector(lock[k])
        if actor.get("body_type"):
            fields["mechanics.body_type"] = actor["body_type"]
        fields["ik.chains"] = actor["ik_chains"]
        fields["animation.sources"] = actor["animation_sources"]
        for k in ("node_overrides", "node_resource", "node_unmatched"):
            fields["model." + k] = actor["model_" + k]
        fields["optics.visible"] = actor["visible"]
        for k in ("diffuse", "emission", "metallic", "roughness", "specular", "shininess"):
            fields.pop("optics." + k, None)
        for k, value in actor["authored_optics"].items():
            fields["optics." + k] = _vector(value) if isinstance(value, list) else value
        fields["material.source"] = "model"
        fields["material.overrides"] = actor["material"]["overrides"]
        fields.pop("material.texture", None)
        if actor["material"].get("texture"):
            fields["material.texture"] = actor["material"]["texture"]
        if actor["actor_type"] == "audio":
            fields["audio_resource_id"] = actor["audio_resource_id"]
        section.update({f"{key}.{k}": _value(v) for k, v in fields.items()})
    return section


def _camera_section(state, previous):
    # Indexes change on reorder/delete. Only a matching camera ID inherits its extension fields.
    old = {}
    section = {}
    for key, value in previous.items():
        match = re.fullmatch(r"camera(\d+)\.(.+)", key)
        if match:
            old.setdefault(int(match[1]), {})[match[2]] = value
        elif key not in {"count", "active_id"}:
            section[key] = value
    by_id = {fields.get("id", f'{state["id"]}#camera{index}'): fields for index, fields in old.items()}
    section.update(count=len(state["cameras"]), active_id=state["active_camera_id"])
    for index, camera in enumerate(state["cameras"]):
        for suffix, value in by_id.get(camera["id"], {}).items():
            section[f"camera{index}.{suffix}"] = value
        for key in ("id", "name", "deletable", "position", "forward", "world_up", "fov", "output_mode",
                    "render_backend", "vision_render_mode", "vision_denoise", "vision_denoiser",
                    "vision_reblur_settings", "vision_pt_settings", "vision_restir_settings",
                    "vision_relax_settings", "vision_accumulation", "vision_stable_planes", "vision_spp",
                    "vision_max_depth", "width", "height", "move_speed", "view_open", "view_x", "view_y",
                    "view_width", "view_height"):
            value = camera[key]
            section[f"camera{index}.{key}"] = _vector(value) if key in {"position", "forward", "world_up"} else _value(value)
    return section


def save_scene(request):
    root = Path(request["project_path"]).resolve(strict=True)
    path = root / "scene.ini"
    state = request["scene"]
    if state["id"] != path.name or path.is_symlink():
        raise ValueError("Only the active portable scene.ini can be saved")
    recover_scene(root)
    _, original = _resolve_source(root)
    updates = {"actors": _actor_section(state["actors"], original["actors"] if "actors" in original else {})}
    if not request.get("actors_only"):
        updates.update({
            "sun": {"enabled": _value(state["sun"]["enabled"]), "sun_direction": _vector(state["sun"]["direction"])},
            "grid": {"enabled": _value(state["grid"]["enabled"])},
            "physics": {k: _vector(v) if k == "gravity" else format(v, ".6f") for k, v in state["physics"].items()},
            "camera": _camera_section(state, original["camera"] if "camera" in original else {}),
            "terrain": state["terrain"], "scripts": {"path": state["script"]},
        })
        if request.get("metadata"):
            updates["scene"] = request["metadata"]
    replace_sections(path, original, updates)
    return {"ok": True, "status": "success", "filepath": str(path)}


def replace_sections(path, original, updates):
    for name, fields in updates.items():
        # actors/camera are complete collections; deleted objects must disappear.
        if name in {"actors", "camera"}:
            original.remove_section(name)
        if not original.has_section(name):
            original.add_section(name)
        for key, value in fields.items():
            original.set(name, key, _value(value).replace("%", "%%"))
    validate_assets(path.parent, {s: dict(original[s]) for s in original.sections()}, verify_hashes=False)

    write_ini_sections(path, original, updates)

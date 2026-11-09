from __future__ import annotations

import configparser
import hashlib
import json
import math
from pathlib import Path
from typing import Any

from .errors import ArchiveParseError


MAX_SNAPSHOT_ITEMS = 100_000


def _float3(value: str, fallback: tuple[float, float, float]) -> list[float]:
    try:
        parts = [float(item.strip()) for item in value.split(",")]
    except (TypeError, ValueError):
        parts = []
    if len(parts) != 3 or not all(math.isfinite(item) for item in parts):
        raise ArchiveParseError(
            "INVALID_VECTOR", f"Expected three finite numbers, got: {value}"
        )
    return parts


def _is_relative_to(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def _actor_fields(section: configparser.SectionProxy) -> dict[str, dict[str, str]]:
    fields: dict[str, dict[str, str]] = {}
    for name, value in section.items():
        key, separator, _ = name.partition(".")
        if separator:
            fields.setdefault(key, {})[name] = value
    return fields


def _read_ini(path: Path) -> configparser.ConfigParser:
    if not path.is_file():
        raise ArchiveParseError(
            "ARCHIVE_FILE_NOT_FOUND", f"Archive file does not exist: {path}", path=str(path)
        )
    config = configparser.ConfigParser()
    try:
        with path.open("r", encoding="utf-8-sig") as stream:
            config.read_file(stream)
    except (OSError, configparser.Error) as exc:
        raise ArchiveParseError(
            "ARCHIVE_PARSE_FAILED", f"Unable to parse archive file: {exc}", path=str(path)
        ) from exc
    return config


def _resolve_source(source: Path) -> tuple[Path, configparser.ConfigParser]:
    selected = source / "scene.ini" if source.is_dir() else source
    if selected.name != "scene.ini":
        raise ArchiveParseError(
            "UNSUPPORTED_ARCHIVE_TYPE", "Expected a scene directory or scene.ini", path=str(selected)
        )
    config = _read_ini(selected)
    if (config.get("format", "type", fallback="") != "corona_scene_folder"
            or config.getint("format", "version", fallback=0) != 1):
        raise ArchiveParseError(
            "UNSUPPORTED_ARCHIVE_VERSION", "Expected corona_scene_folder version 1", path=str(selected)
        )
    # Old external Vision documents are not Corona scenes. Reject the marker
    # before reading actors so proxy geometry is never mistaken for the scene.
    if any(config.has_section(section) for section in (
        "vision", "vision_document", "vision_bindings", "vision_unsupported_shapes"
    )):
        raise ArchiveParseError(
            "UNSUPPORTED_VISION_ARCHIVE", "External Vision archives are no longer supported",
            path=str(selected),
        )
    return selected, config


def _parse_actor(
    section: configparser.SectionProxy,
    key: str,
    project_root: Path,
    *,
    persisted_fields: dict[str, str],
) -> dict[str, Any]:
    route = section.get(f"{key}.route", "").strip().replace("\\", "/")
    asset_path = (project_root / route).resolve() if route else project_root.resolve()
    if route and not _is_relative_to(asset_path, project_root.resolve()):
        raise ArchiveParseError(
            "RESOURCE_PATH_OUTSIDE_PROJECT",
            f"Actor resource escapes the portable scene root: {route}",
            path=str(asset_path),
            details={"actor_name": section.get(f"{key}.name", key)},
        )
    if route and asset_path.suffix.lower() == ".actor" and asset_path.is_file():
        from .assets import actor_descriptor
        try:
            asset_path = Path(actor_descriptor(asset_path, project_root)["model"])
        except (OSError, ValueError, configparser.Error) as exc:
            raise ArchiveParseError("INVALID_ACTOR_DESCRIPTOR", str(exc), path=str(asset_path)) from exc
        if not asset_path.is_relative_to(project_root.resolve()):
            raise ArchiveParseError("RESOURCE_PATH_OUTSIDE_PROJECT", "Actor model escapes project", path=str(asset_path))
    collision = section.get(f"{key}.mechanics.collision_type", "").strip().lower()
    if not collision:
        collision = (
            "box"
            if section.getboolean(f"{key}.mechanics.collision_enabled", fallback=True)
            else "none"
        )
    if collision not in {"box", "mesh", "none"}:
        raise ArchiveParseError(
            "INVALID_COLLISION_TYPE",
            f"Unsupported collision type for {key}: {collision}",
            details={"actor_name": section.get(f"{key}.name", key)},
        )
    body_type = section.get(f"{key}.mechanics.body_type", fallback=None)
    if body_type is not None:
        body_type = body_type.strip().lower()
        if body_type not in {"dynamic", "kinematic", "static", "phantom"}:
            raise ArchiveParseError(
                "INVALID_BODY_TYPE",
                f"Unsupported body type for {key}: {body_type}",
                details={"actor_name": section.get(f"{key}.name", key)},
            )
    optics: dict[str, Any] = {}
    diffuse = section.get(f"{key}.optics.diffuse", fallback="").strip()
    if diffuse:
        optics["diffuse"] = _float3(diffuse, (0.8, 0.8, 0.8))
    emission = section.get(f"{key}.optics.emission", fallback="").strip()
    if emission:
        optics["emission"] = _float3(emission, (0.0, 0.0, 0.0))
    for field in ("metallic", "roughness", "specular", "shininess"):
        value = section.get(f"{key}.optics.{field}", fallback="").strip()
        if value:
            try:
                number = float(value)
            except ValueError as exc:
                raise ArchiveParseError(
                    "INVALID_NUMBER",
                    f"Invalid actor optics value {key}.{field}: {value}",
                    details={"actor_name": section.get(f"{key}.name", key), "field": field},
                ) from exc
            if not math.isfinite(number):
                raise ArchiveParseError(
                    "INVALID_NUMBER",
                    f"Non-finite actor optics value {key}.{field}",
                    details={"actor_name": section.get(f"{key}.name", key), "field": field},
                )
            optics[field] = number
    material_source = section.get(f"{key}.material.source", fallback="legacy").strip()
    try:
        if material_source not in {"legacy", "model", "scene"}:
            raise ValueError("source must be legacy, model, or scene")
        material_overrides = json.loads(section.get(f"{key}.material.overrides", fallback="[]"))
        if not isinstance(material_overrides, list) or len(material_overrides) > 4096 or any(
            not isinstance(row, dict) for row in material_overrides
        ):
            raise ValueError("overrides must contain at most 4096 objects")
    except (ValueError, TypeError) as exc:
        raise ArchiveParseError(
            "INVALID_MATERIAL_CONFIG", f"Invalid material configuration for {key}: {exc}",
            details={"actor_name": section.get(f"{key}.name", key)},
        ) from exc
    texture = section.get(f"{key}.material.texture", fallback="").strip().replace("\\", "/")
    if texture:
        optics["texture"] = texture
        texture_asset_path = (project_root / texture).resolve()
        if not _is_relative_to(
            texture_asset_path, project_root.resolve()
        ):
            raise ArchiveParseError(
                "RESOURCE_PATH_OUTSIDE_PROJECT",
                f"Actor texture escapes the portable scene root: {texture}",
                path=str(texture_asset_path),
                details={"actor_name": section.get(f"{key}.name", key)},
            )
        optics["texture_asset_path"] = str(texture_asset_path)
    try:
        ik_chains = json.loads(section.get(f"{key}.ik.chains", fallback="[]"))
        if not isinstance(ik_chains, list) or len(ik_chains) > 64 or any(
            not isinstance(chain, dict) for chain in ik_chains
        ):
            raise ValueError("expected up to 64 IK chain objects")
    except (ValueError, TypeError) as exc:
        raise ArchiveParseError(
            "INVALID_IK_CHAINS", f"Invalid IK configuration for {key}: {exc}",
            details={"actor_name": section.get(f"{key}.name", key)},
        ) from exc
    try:
        animation_sources = json.loads(section.get(f"{key}.animation.sources", fallback="[]"))
        if not isinstance(animation_sources, list) or any(
            not isinstance(source, str) or not source or "\0" in source for source in animation_sources
        ):
            raise ValueError("expected an array of non-empty file paths")
        animation_sources = [source.replace("\\", "/") for source in animation_sources]
        if len(set(animation_sources)) != len(animation_sources):
            raise ValueError("duplicate animation source")
    except (ValueError, TypeError) as exc:
        raise ArchiveParseError(
            "INVALID_ANIMATION_SOURCES", f"Invalid animation configuration for {key}: {exc}",
            details={"actor_name": section.get(f"{key}.name", key)},
        ) from exc
    for source in animation_sources:
        if not _is_relative_to((project_root / source).resolve(), project_root.resolve()):
            raise ArchiveParseError(
                "RESOURCE_PATH_OUTSIDE_PROJECT", f"Animation source escapes the portable scene root: {source}",
                details={"actor_name": section.get(f"{key}.name", key)},
            )
    try:
        model_node_overrides = json.loads(section.get(f"{key}.model.node_overrides", fallback="[]"))
        if not isinstance(model_node_overrides, list) or any(
            not isinstance(override, dict) for override in model_node_overrides
        ):
            raise ValueError("expected model node override objects")
    except (ValueError, TypeError) as exc:
        raise ArchiveParseError(
            "INVALID_MODEL_NODE_OVERRIDES", f"Invalid model node configuration for {key}: {exc}",
            details={"actor_name": section.get(f"{key}.name", key)},
        ) from exc
    try:
        model_node_unmatched = json.loads(section.get(f"{key}.model.node_unmatched", fallback="[]"))
        if not isinstance(model_node_unmatched, list) or any(
            not isinstance(group, dict) or not isinstance(group.get("resource"), str)
            or not isinstance(group.get("overrides"), list)
            or any(not isinstance(override, dict) for override in group["overrides"])
            for group in model_node_unmatched
        ):
            raise ValueError("expected resource strings and model node override arrays")
    except (ValueError, TypeError) as exc:
        raise ArchiveParseError(
            "INVALID_MODEL_NODE_UNMATCHED", f"Invalid unmatched model node configuration for {key}: {exc}",
            details={"actor_name": section.get(f"{key}.name", key)},
        ) from exc
    return {
        "name": section.get(f"{key}.name", key),
        "actor_guid": section.get(f"{key}.actor_guid", ""),
        "actor_type": section.get(f"{key}.actor_type", "actor"),
        "route": route,
        "asset_path": str(asset_path),
        "runtime_entity_id": section.get(f"{key}.runtime.entity_id", ""),
        "asset_id": section.get(f"{key}.runtime.asset_id", ""),
        "model_ref": section.get(f"{key}.runtime.model_ref", ""),
        "entity_type": section.get(f"{key}.runtime.entity_type", ""),
        "semantic_role": section.get(f"{key}.runtime.semantic_role", ""),
        "source_plan_id": section.get(f"{key}.runtime.source_plan_id", ""),
        "source_batch_id": section.get(f"{key}.runtime.source_batch_id", ""),
        "source_scene_version": max(
            section.getint(f"{key}.runtime.source_scene_version", fallback=1), 1
        ),
        "actor_version": max(
            section.getint(f"{key}.runtime.actor_version", fallback=1), 1
        ),
        "follow_camera": section.getboolean(f"{key}.follow_camera", fallback=False),
        "camera_lock": {
            "enabled": section.getboolean(f"{key}.camera_lock.enabled", fallback=False),
            "position_offset": _float3(
                section.get(f"{key}.camera_lock.position_offset", "0, 0, 2"), (0.0, 0.0, 2.0)
            ),
            "rotation_offset": _float3(
                section.get(f"{key}.camera_lock.rotation_offset", "0, 0, 0"), (0.0, 0.0, 0.0)
            ),
        },
        "transform": {
            "position": _float3(
                section.get(f"{key}.geometry.position", "0, 0, 0"), (0.0, 0.0, 0.0)
            ),
            "rotation": _float3(
                section.get(f"{key}.geometry.rotation", "0, 0, 0"), (0.0, 0.0, 0.0)
            ),
            "scale": _float3(
                section.get(f"{key}.geometry.scale", "1, 1, 1"), (1.0, 1.0, 1.0)
            ),
        },
        "visible": section.getboolean(f"{key}.optics.visible", fallback=True),
        "mechanics": {
            **({"body_type": body_type} if body_type is not None else {}),
            "physics_enabled": section.getboolean(
                f"{key}.mechanics.physics_enabled", fallback=True
            ),
            "collision_type": collision,
        },
        "optics": optics,
        "material": {"source": material_source, "overrides": material_overrides},
        "audio_resource_id": section.get(f"{key}.audio_resource_id", ""),
        "ik_chains": ik_chains,
        "animation_sources": animation_sources,
        "model_node_overrides": model_node_overrides,
        "model_node_resource": section.get(f"{key}.model.node_resource", fallback=""),
        "model_node_unmatched": model_node_unmatched,
        "persisted_fields": persisted_fields,
    }


def _parse_camera(
    section: configparser.SectionProxy | None, index: int, scene_route: str
) -> dict[str, Any]:
    prefix = f"camera{index}."
    get = (lambda name, fallback="": section.get(prefix + name, fallback)) if section else (
        lambda name, fallback="": fallback
    )
    finite_float = lambda name, fallback: _finite_number(get(name, fallback), prefix + name)
    integer = lambda name, fallback: int(get(name, fallback))
    camera_id = get("id", f"{scene_route}#camera{index}")
    vision_mode = str(get("vision_render_mode", "path_tracing")).strip().lower().replace("-", "_")
    legacy_svgf = vision_mode in {"svgf", "vision_svgf"}
    legacy_progressive = vision_mode == "progressive_path_tracing"
    accumulation_value = str(get("vision_accumulation", "")).strip().lower()
    vision_accumulation = (
        configparser.ConfigParser.BOOLEAN_STATES[accumulation_value]
        if accumulation_value in configparser.ConfigParser.BOOLEAN_STATES
        else legacy_progressive
    )
    stable_planes_value = str(get("vision_stable_planes", "true")).strip().lower()
    vision_stable_planes = configparser.ConfigParser.BOOLEAN_STATES.get(stable_planes_value, True)
    denoise_value = str(get("vision_denoise", "")).strip().lower()
    vision_denoise = (
        configparser.ConfigParser.BOOLEAN_STATES[denoise_value]
        if denoise_value in configparser.ConfigParser.BOOLEAN_STATES
        else True
    )
    return {
        "id": camera_id,
        "name": get("name", "MainCamera" if index == 0 else f"Camera{index}"),
        "deletable": str(get("deletable", "false" if index == 0 else "true")).lower()
        == "true",
        "position": _float3(get("position", "0, 0, -5"), (0.0, 0.0, -5.0)),
        "forward": _float3(get("forward", "0, 0, 1"), (0.0, 0.0, 1.0)),
        "world_up": _float3(get("world_up", "0, 1, 0"), (0.0, 1.0, 0.0)),
        "fov": finite_float("fov", "45"),
        "width": integer("width", "1920"),
        "height": integer("height", "1080"),
        "view_open": str(get("view_open", "false")).lower() == "true",
        "view_x": integer("view_x", "120"),
        "view_y": integer("view_y", "120"),
        "view_width": integer("view_width", "960"),
        "view_height": integer("view_height", "540"),
        "move_speed": finite_float("move_speed", "1"),
        "output_mode": get("output_mode", "final_color"),
        "render_backend": get("render_backend", "native"),
        "vision_render_mode": "path_tracing" if legacy_svgf or legacy_progressive else vision_mode,
        "vision_spp": get("vision_spp", ""),
        "vision_max_depth": get("vision_max_depth", ""),
        "vision_denoise": vision_denoise,
        "vision_accumulation": vision_accumulation,
        "vision_stable_planes": vision_stable_planes,
        "vision_denoiser": str(get("vision_denoiser", "svgf" if legacy_svgf else "reblur")).strip().lower(),
        "vision_reblur_settings": json.loads(get("vision_reblur_settings", "{}")),
        "vision_pt_settings": json.loads(get("vision_pt_settings", "{}")),
        "vision_restir_settings": json.loads(get("vision_restir_settings", "{}")),
        "vision_relax_settings": json.loads(get("vision_relax_settings", "{}")),
    }


def _finite_number(value: str, field: str) -> float:
    number = float(value)
    if not math.isfinite(number):
        raise ArchiveParseError(
            "INVALID_NUMBER", f"Non-finite archive value: {field}", details={"field": field}
        )
    return number


def _parse_archive_impl(input_path: str) -> dict[str, Any]:
    source = Path(input_path).expanduser().resolve()
    scene_file, config = _resolve_source(source)
    root = scene_file.parent
    scene_route = "scene.ini"
    scene_section = config["scene"] if config.has_section("scene") else {}
    actors_section = config["actors"] if config.has_section("actors") else None
    actor_fields = _actor_fields(actors_section) if actors_section else {}
    actors = (
        [
            _parse_actor(
                actors_section,
                key,
                root,
                persisted_fields=actor_fields[key],
            )
            for key in sorted(actor_fields)
        ]
        if actors_section
        else []
    )
    if len(actors) > MAX_SNAPSHOT_ITEMS:
        raise ArchiveParseError("ARCHIVE_ITEM_LIMIT_EXCEEDED", "Archive has too many actors")
    for index, actor in enumerate(actors):
        if not actor["actor_guid"]:
            seed = f"{scene_route}|{actor['name']}|{index}".encode("utf-8")
            actor["actor_guid"] = "actor-" + hashlib.sha1(seed).hexdigest()[:16]
    camera_section = config["camera"] if config.has_section("camera") else None
    camera_count = camera_section.getint("count", fallback=1) if camera_section else 1
    if camera_count > MAX_SNAPSHOT_ITEMS:
        raise ArchiveParseError("ARCHIVE_ITEM_LIMIT_EXCEEDED", "Archive has too many cameras")
    cameras = [_parse_camera(camera_section, index, scene_route) for index in range(max(1, camera_count))]
    active_camera_id = (
        camera_section.get("active_id", cameras[0]["id"]) if camera_section else cameras[0]["id"]
    ) or cameras[0]["id"]
    actor_guids: set[str] = set()
    for actor in actors:
        guid = actor["actor_guid"]
        if guid and guid in actor_guids:
            raise ArchiveParseError(
                "DUPLICATE_ACTOR_GUID",
                f"Duplicate actor GUID: {guid}",
                path=str(scene_file),
                details={"actor_guid": guid, "actor_name": actor["name"]},
            )
        if guid:
            actor_guids.add(guid)
    camera_ids: set[str] = set()
    for camera in cameras:
        camera_id = camera["id"]
        if camera_id in camera_ids:
            raise ArchiveParseError(
                "DUPLICATE_CAMERA_ID",
                f"Duplicate camera ID: {camera_id}",
                path=str(scene_file),
                details={"camera_id": camera_id},
            )
        camera_ids.add(camera_id)
    if active_camera_id not in camera_ids:
        raise ArchiveParseError(
            "ACTIVE_CAMERA_NOT_FOUND",
            f"Active camera does not exist: {active_camera_id}",
            path=str(scene_file),
            details={"camera_id": active_camera_id},
        )
    diagnostics = []
    for actor in actors:
        if actor["route"] and not Path(actor["asset_path"]).is_file():
            diagnostics.append(
                {
                    "severity": "error",
                    "recoverable": True,
                    "stage": "archive_parse",
                    "code": "RESOURCE_NOT_FOUND",
                    "message": f"Actor resource does not exist: {actor['route']}",
                    "path": actor["asset_path"],
                    "actor_guid": actor["actor_guid"],
                    "actor_name": actor["name"],
                    "resource_path": actor["route"],
                }
            )
        texture = actor["optics"].get("texture", "")
        texture_asset_path = actor["optics"].get("texture_asset_path", "")
        if texture and texture_asset_path and not Path(texture_asset_path).is_file():
            diagnostics.append(
                {
                    "severity": "warning",
                    "recoverable": True,
                    "stage": "archive_parse",
                    "code": "ATTACHMENT_RESOURCE_NOT_FOUND",
                    "message": f"Actor texture does not exist: {texture}",
                    "path": texture_asset_path,
                    "actor_guid": actor["actor_guid"],
                    "actor_name": actor["name"],
                    "resource_path": texture,
                }
            )
    project_name = scene_section.get("name", root.name)
    physics = {}
    if config.has_section("physics"):
        section = config["physics"]
        if "gravity" in section:
            physics["gravity"] = _float3(section["gravity"], (0.0, -9.8, 0.0))
        for field in ("floor_y", "floor_restitution", "fixed_dt"):
            if field in section:
                physics[field] = _finite_number(section[field], f"physics.{field}")
        if ("fixed_dt" in physics and physics["fixed_dt"] <= 0
                or "floor_restitution" in physics and not 0 <= physics["floor_restitution"] <= 1):
            raise ArchiveParseError("INVALID_PHYSICS", "Physics requires fixed_dt > 0 and restitution in [0, 1]")
    return {
        "schema_version": 1,
        "archive_type": "portable_scene",
        "project_root": str(root.resolve()),
        "project": {"name": project_name},
        "scene": {
            "route": scene_route,
            "name": scene_section.get("name", scene_file.stem),
            "core_version": scene_section.get("core_version", ""),
            "environment": {
                "sun_enabled": config.getboolean("sun", "enabled", fallback=True),
                "sun_direction": _float3(
                    config.get("sun", "sun_direction", fallback="1, 1, 1"),
                    (1.0, 1.0, 1.0),
                ),
                "floor_grid_enabled": config.getboolean("grid", "enabled", fallback=True),
                "physics": physics,
            },
            "scripts": {"path": config.get("scripts", "path", fallback="")},
            "terrain": {
                "type": config.get("terrain", "type", fallback=""),
                "path": config.get("terrain", "path", fallback=""),
            },
            "actors": actors,
            "cameras": cameras,
            "active_camera_id": active_camera_id,
        },
        "extensions": {
            "ini_sections": {
                section_name: dict(config.items(section_name))
                for section_name in config.sections()
            }
        },
        "diagnostics": diagnostics,
    }


def parse_archive(input_path: str) -> dict[str, Any]:
    """Parse an archive without creating engine or Python scene entities."""
    try:
        if Path(input_path).suffix.lower() in {".xml", ".pbrt"}:
            from .external_scene import parse_external_scene
            return parse_external_scene(input_path)
        return _parse_archive_impl(input_path)
    except ArchiveParseError:
        raise
    except (configparser.Error, OSError, TypeError, ValueError) as exc:
        raise ArchiveParseError(
            "INVALID_ARCHIVE_VALUE",
            f"Archive contains an invalid value: {exc}",
            path=str(input_path),
        ) from exc

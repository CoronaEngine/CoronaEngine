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


def _actor_keys(section: configparser.SectionProxy) -> list[str]:
    return sorted({key.split(".", 1)[0] for key in section if "." in key})


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


def _legacy_project_root(scene_file: Path) -> Path:
    candidate = scene_file.parent
    while candidate != candidate.parent:
        if (candidate / "project.ini").is_file():
            return candidate
        candidate = candidate.parent
    raise ArchiveParseError(
        "LEGACY_PROJECT_NOT_FOUND",
        f"Legacy scene has no owning project.ini: {scene_file}",
        path=str(scene_file),
    )


def _resolve_source(source: Path) -> tuple[str, Path, Path, str, configparser.ConfigParser]:
    selected = source
    if source.is_dir():
        selected = source / "scene.ini" if (source / "scene.ini").is_file() else source / "project.ini"

    if selected.name.lower() == "scene.ini":
        scene_config = _read_ini(selected)
        format_type = scene_config.get("format", "type", fallback="")
        version = scene_config.getint("format", "version", fallback=0)
        if format_type != "corona_scene_folder" or version != 1:
            raise ArchiveParseError(
                "UNSUPPORTED_ARCHIVE_VERSION",
                f"Unsupported portable scene format: {format_type or '<missing>'} v{version}",
                path=str(selected),
            )
        return "portable_scene", selected.parent, selected, "scene.ini", scene_config

    if selected.name.lower() == "project.ini":
        project_config = _read_ini(selected)
        route = project_config.get("Project", "entrance_scene", fallback="").strip().replace("\\", "/")
        if not route:
            routes = [item.strip().replace("\\", "/") for item in project_config.get("Project", "scenes", fallback="").split(",") if item.strip()]
            route = routes[0] if routes else ""
        if not route:
            raise ArchiveParseError(
                "ENTRANCE_SCENE_NOT_FOUND", "Legacy project has no entrance scene", path=str(selected)
            )
        scene_file = (selected.parent / route).resolve()
        return "legacy_project", selected.parent, scene_file, route, _read_ini(scene_file)

    if selected.suffix.lower() == ".scene":
        root = _legacy_project_root(selected)
        return (
            "legacy_project",
            root,
            selected,
            selected.relative_to(root).as_posix(),
            _read_ini(selected),
        )

    if selected.suffix.lower() == ".json":
        try:
            document = json.loads(selected.read_text(encoding="utf-8-sig"))
        except (OSError, json.JSONDecodeError) as exc:
            raise ArchiveParseError(
                "VISION_JSON_PARSE_FAILED", f"Unable to parse Vision JSON: {exc}", path=str(selected)
            ) from exc
        if not isinstance(document, dict):
            raise ArchiveParseError(
                "VISION_JSON_STRUCTURE_INVALID",
                "Vision JSON root must be an object",
                path=str(selected),
            )
        config = configparser.ConfigParser()
        config["scene"] = {"name": str(document.get("name") or selected.stem)}
        config["vision"] = {"storage": "source_json", "source_path": str(selected)}
        config["vision_document"] = {
            "version": str(document.get("version") or "1"),
            "encoding": "json",
            "data": json.dumps(document, ensure_ascii=False, separators=(",", ":")),
            "asset_root": str(selected.parent),
        }
        return "vision_json", selected.parent, selected, selected.name, config

    raise ArchiveParseError(
        "UNSUPPORTED_ARCHIVE_TYPE", f"Unsupported archive path: {selected}", path=str(selected)
    )


def _parse_actor(
    section: configparser.SectionProxy,
    key: str,
    project_root: Path,
    *,
    require_project_relative: bool,
) -> dict[str, Any]:
    route = section.get(f"{key}.route", "").strip().replace("\\", "/")
    asset_path = (project_root / route).resolve() if route else project_root.resolve()
    if route and require_project_relative and not _is_relative_to(asset_path, project_root.resolve()):
        raise ArchiveParseError(
            "RESOURCE_PATH_OUTSIDE_PROJECT",
            f"Actor resource escapes the portable scene root: {route}",
            path=str(asset_path),
            details={"actor_name": section.get(f"{key}.name", key)},
        )
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
    texture = section.get(f"{key}.material.texture", fallback="").strip().replace("\\", "/")
    if texture:
        optics["texture"] = texture
        texture_asset_path = (project_root / texture).resolve()
        if require_project_relative and not _is_relative_to(
            texture_asset_path, project_root.resolve()
        ):
            raise ArchiveParseError(
                "RESOURCE_PATH_OUTSIDE_PROJECT",
                f"Actor texture escapes the portable scene root: {texture}",
                path=str(texture_asset_path),
                details={"actor_name": section.get(f"{key}.name", key)},
            )
        optics["texture_asset_path"] = str(texture_asset_path)
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
            "physics_enabled": section.getboolean(
                f"{key}.mechanics.physics_enabled", fallback=True
            ),
            "collision_type": collision,
        },
        "optics": optics,
        "audio_resource_id": section.get(f"{key}.audio_resource_id", ""),
        "persisted_fields": {
            name: value for name, value in section.items() if name.startswith(f"{key}.")
        },
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
        "vision_render_mode": get("vision_render_mode", "path_tracing"),
        "ssao_enabled": str(get("ssao_enabled", "true")).lower() == "true",
        "vision_spp": get("vision_spp", ""),
        "vision_max_depth": get("vision_max_depth", ""),
        "vision_denoise": get("vision_denoise", ""),
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
    archive_type, root, scene_file, scene_route, config = _resolve_source(source)
    legacy = archive_type == "legacy_project"
    metadata_section = "base" if legacy else "scene"
    scene_section = config[metadata_section] if config.has_section(metadata_section) else {}
    actors_section = config["actors"] if config.has_section("actors") else None
    actors = (
        [
            _parse_actor(
                actors_section,
                key,
                root,
                require_project_relative=archive_type == "portable_scene",
            )
            for key in _actor_keys(actors_section)
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
    )
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
    project_name = root.name
    if legacy:
        project_config = _read_ini(root / "project.ini")
        project_name = project_config.get("Project", "name", fallback=root.name)
    else:
        project_name = scene_section.get("name", root.name)
    return {
        "schema_version": 1,
        "archive_type": archive_type,
        "project_root": str(root.resolve()),
        "project": {"name": project_name, "legacy": legacy, "entry_scene": scene_route},
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
            },
            "scripts": {"path": config.get("scripts", "path", fallback="")},
            "terrain": {
                "type": config.get("terrain", "type", fallback=""),
                "path": config.get("terrain", "path", fallback=""),
            },
            "vision": {
                "storage": config.get("vision", "storage", fallback=""),
                "source_id": config.get("vision", "source_id", fallback=""),
                "source_path": config.get("vision", "source_path", fallback=""),
                "import_mode": config.get("vision", "import_mode", fallback=""),
                "document_version": config.get("vision_document", "version", fallback=""),
                "document_encoding": config.get("vision_document", "encoding", fallback=""),
                "document_data": config.get("vision_document", "data", fallback=""),
                "document_asset_root": config.get(
                    "vision_document", "asset_root", fallback=""
                ),
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
        return _parse_archive_impl(input_path)
    except ArchiveParseError:
        raise
    except (configparser.Error, OSError, TypeError, ValueError) as exc:
        raise ArchiveParseError(
            "INVALID_ARCHIVE_VALUE",
            f"Archive contains an invalid value: {exc}",
            path=str(input_path),
        ) from exc

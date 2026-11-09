"""Capture and restore live native scene state for a file run."""
import json as _json
import logging
import time as _time
from pathlib import Path as _Path

_logger = logging.getLogger(__name__)


def _target_token(value):
    text = str(value or "").strip().replace("\\", "/")
    return text.casefold()


def _native_payload(value):
    if isinstance(value, str):
        try:
            value = _json.loads(value)
        except Exception:
            return None
    if not isinstance(value, dict):
        return None
    if value.get("success") is False or value.get("status") == "error":
        return None
    data = value.get("data") if isinstance(value.get("data"), dict) else value
    return data


def _native_payload_or_raise(value, action):
    """Validate a native editor result without discarding its error message."""
    raw = value
    if isinstance(raw, str):
        try:
            raw = _json.loads(raw)
        except Exception as exc:
            raise RuntimeError(f"{action} returned invalid data: {raw}") from exc
    if not isinstance(raw, dict):
        raise RuntimeError(f"{action} returned no valid result")
    if raw.get("success") is False or raw.get("status") == "error":
        message = raw.get("message") or raw.get("error") or "native interface failed"
        raise RuntimeError(f"{action} failed: {message}")
    data = raw.get("data") if isinstance(raw.get("data"), dict) else raw
    return data


def _normalize_native_collision_type(value):
    if isinstance(value, dict):
        value = value.get("type", value.get("shape"))
    if isinstance(value, str):
        candidate = value.strip().lower()
        if candidate in ("none", "box", "mesh"):
            return candidate
        if candidate in ("off", "disabled", "false", "0"):
            return "none"
        return "box"
    if value in (False, 0, None):
        return "none"
    if value == 2:
        return "mesh"
    return "box"


def _native_scene_snapshot(scene_name):
    from api.editor_api import get_script_runtime_editor_api

    try:
        return _native_payload(get_script_runtime_editor_api().scene.get_snapshot(scene_name)), []
    except Exception as exc:
        return None, [str(exc)]


def capture(scene_name, session):
    payload, errors = _native_scene_snapshot(scene_name)
    if not payload:
        raise RuntimeError(f"Unable to capture scene: {errors}")
    if not session or payload.get("archive_session") != session:
        raise RuntimeError("PROJECT_CONTEXT_CHANGED")
    return {"binding_mode": "native_editor", "scene_name": payload.get("scene") or payload.get("id") or scene_name, "payload": payload}


def _snapshot_actor_key(data):
    guid = str((data or {}).get("actor_guid") or "").strip()
    if guid:
        return "guid:" + guid.casefold()
    return "name:" + _target_token((data or {}).get("name") or (data or {}).get("route") or "")


def _set_native_runtime_camera(scene_route, position, forward, world_up, fov, camera_name=""):
    """Update the main editor viewport camera in memory without writing the scene file."""
    try:
        from api.editor_api import get_script_runtime_editor_api

        viewport = get_script_runtime_editor_api().viewport
        setter = getattr(viewport, "set_camera_pose", None) if viewport is not None else None
        if not callable(setter):
            return False
        payload = _native_payload(setter(
            str(scene_route or ""),
            str(camera_name or ""),
            {
                "position": list(position),
                "forward": list(forward),
                "world_up": list(world_up),
                "fov": float(fov),
                "persist": False,
            },
        ))
        return isinstance(payload, dict) and payload.get("status") not in ("error", "failed")
    except Exception as exc:
        _logger.debug("[FileScripts] native runtime camera update failed: %s", exc)
        return False


def _restore_native_camera_state(scene_route, camera_data):
    if not isinstance(camera_data, dict):
        return False
    return _set_native_runtime_camera(
        scene_route,
        camera_data.get("position") or [0.0, 0.0, -5.0],
        camera_data.get("forward") or [0.0, 0.0, 1.0],
        camera_data.get("world_up") or camera_data.get("up") or [0.0, 1.0, 0.0],
        camera_data.get("fov", 45.0),
        camera_data.get("camera_id") or camera_data.get("id") or camera_data.get("name") or "",
    )


def _restore_native_actor_state(scene_route, actor_data, session):
    from api.editor_api import get_script_runtime_editor_api

    actor_name = str(actor_data.get("name") or actor_data.get("route") or "")
    if not actor_name:
        raise RuntimeError("Scene snapshot actor is missing a name")
    geometry = actor_data.get("geometry") if isinstance(actor_data.get("geometry"), dict) else {}
    mechanics = actor_data.get("mechanics") if isinstance(actor_data.get("mechanics"), dict) else {}
    api = get_script_runtime_editor_api()
    scene_api = api.scene
    setter = getattr(scene_api, "set_actor_transform", None) if scene_api is not None else None
    if not callable(setter):
        raise RuntimeError("native set_editor_actor_transform is unavailable")

    # Pause physics before restoring transforms so simulation cannot race restoration.
    if mechanics:
        _native_payload_or_raise(
            api.scene_tools.set_actor_physics(scene_route, actor_name, {"body_type": "static"}),
            f"Pause actor {actor_name} physics",
        )

    transform = {
        key: list(geometry[key])
        for key in ("position", "rotation", "scale")
        if isinstance(geometry.get(key), (list, tuple))
    }
    if transform:
        _native_payload_or_raise(
            setter(scene_route, actor_name, {**transform, "persist": False, "archive_session": session}),
            f"Restore actor {actor_name} transform",
        )

    _native_payload_or_raise(
        api.scene_tools.set_actor_state(scene_route, actor_name, {"visible": bool(actor_data.get("visible", True))}),
        f"Restore actor {actor_name} visibility",
    )
    if mechanics:
        _native_payload_or_raise(
            api.scene_tools.set_actor_physics(scene_route, actor_name, {
                key: mechanics[key] for key in
                ("body_type", "mass", "restitution", "damping", "linear_lock", "angular_lock")
                if key in mechanics
            }),
            f"Restore actor {actor_name} mechanics",
        )


def _snapshot_actor_match(actors, expected):
    expected_key = _snapshot_actor_key(expected)
    expected_guid = str((expected or {}).get("actor_guid") or "").strip().casefold()
    expected_name = str((expected or {}).get("name") or "").strip().casefold()
    expected_route = str((expected or {}).get("route") or (expected or {}).get("path") or "").strip().replace("\\", "/").casefold()
    for actor in actors:
        if _snapshot_actor_key(actor) == expected_key:
            return actor
    if expected_guid:
        for actor in actors:
            if str(actor.get("actor_guid") or "").strip().casefold() == expected_guid:
                return actor
    if expected_name:
        matches = [actor for actor in actors if str(actor.get("name") or "").strip().casefold() == expected_name]
        if len(matches) == 1:
            return matches[0]
    if expected_route and not expected_guid and not expected_name:
        matches = [
            actor for actor in actors
            if str(actor.get("route") or actor.get("path") or "").strip().replace("\\", "/").casefold() == expected_route
        ]
        if len(matches) == 1:
            return matches[0]
    return None


def _values_close(expected, actual, tolerance=1.0e-3):
    try:
        return abs(float(expected) - float(actual)) <= tolerance
    except (TypeError, ValueError):
        return expected == actual


def _native_actor_state_mismatches(expected, actual):
    mismatches = []
    expected_geometry = expected.get("geometry") if isinstance(expected.get("geometry"), dict) else {}
    actual_geometry = actual.get("geometry") if isinstance(actual.get("geometry"), dict) else {}
    for key in ("position", "rotation", "scale"):
        expected_value = expected_geometry.get(key)
        actual_value = actual_geometry.get(key)
        if not isinstance(expected_value, (list, tuple)):
            continue
        if not isinstance(actual_value, (list, tuple)) or len(actual_value) < len(expected_value):
            mismatches.append(key)
            continue
        if any(not _values_close(left, right) for left, right in zip(expected_value, actual_value)):
            mismatches.append(key)
    for key in ("visible", "collision"):
        if key in expected and bool(expected.get(key)) != bool(actual.get(key)):
            mismatches.append(key)

    expected_mechanics = expected.get("mechanics") if isinstance(expected.get("mechanics"), dict) else {}
    actual_mechanics = actual.get("mechanics") if isinstance(actual.get("mechanics"), dict) else {}
    for key in ("mass", "restitution", "damping"):
        if key in expected_mechanics and not _values_close(
            expected_mechanics.get(key), actual_mechanics.get(key)
        ):
            mismatches.append("mechanics." + key)
    if "physics_enabled" in expected_mechanics and bool(expected_mechanics.get("physics_enabled")) != bool(actual_mechanics.get("physics_enabled")):
        mismatches.append("mechanics.physics_enabled")
    for key in ("linear_lock", "angular_lock"):
        expected_value = expected_mechanics.get(key)
        actual_value = actual_mechanics.get(key)
        if isinstance(expected_value, (list, tuple)):
            if not isinstance(actual_value, (list, tuple)) or tuple(bool(v) for v in expected_value[:3]) != tuple(bool(v) for v in actual_value[:3]):
                mismatches.append("mechanics." + key)
    return mismatches


def _verify_native_scene_restored(scene_route, original):
    payload, errors = _native_scene_snapshot(scene_route)
    if payload is None:
        return ["Unable to read the restored native scene" + (f": {'; '.join(errors)}" if errors else "")]
    current = [item for item in payload.get("actors", []) if isinstance(item, dict)]
    failures = []
    matched_ids = set()
    for expected in original:
        actual = _snapshot_actor_match(current, expected)
        actor_name = str(expected.get("name") or expected.get("route") or "(unnamed)")
        if actual is None:
            failures.append(f"Missing actor {actor_name}")
            continue
        matched_ids.add(id(actual))
        mismatches = _native_actor_state_mismatches(expected, actual)
        if mismatches:
            failures.append(f"Actor {actor_name} fields not restored: {', '.join(mismatches)}")
    extras = [
        str(actor.get("name") or actor.get("route") or "(unnamed)")
        for actor in current
        if id(actor) not in matched_ids
    ]
    if extras:
        failures.append("Runtime-created actors still exist: " + ", ".join(extras))
    return failures


def restore(snapshot):
    if not isinstance(snapshot, dict):
        raise RuntimeError("\u65e0\u6548\u7684\u573a\u666f\u6062\u590d\u5feb\u7167")
    mode = snapshot.get("binding_mode")
    scene_route = str(snapshot.get("scene_name") or "")
    payload = snapshot.get("payload") if isinstance(snapshot.get("payload"), dict) else {}
    original = [item for item in payload.get("actors", []) if isinstance(item, dict)]

    if mode != "native_editor":
        raise ValueError("Native scene snapshot required")
    from api.editor_api import get_script_runtime_editor_api

    api = get_script_runtime_editor_api()
    session = snapshot["payload"]["archive_session"]
    if api.scene.read_active().get("archive_session") != session:
        raise RuntimeError("PROJECT_CONTEXT_CHANGED")
    scene_tools = api.scene_tools
    if scene_tools is None:
        raise RuntimeError("native SceneTools adapter is unavailable")
    _logger.info(
        "[FileScripts] native scene restore begin: scene=%s actors=%d",
        scene_route,
        len(original),
    )
    remover = getattr(scene_tools, "remove_actor", None)
    creator = getattr(scene_tools, "create_actor", None)
    verification_failures = []
    for attempt in range(3):
        current_payload, errors = _native_scene_snapshot(scene_route)
        if current_payload is None:
            raise RuntimeError(
                "Unable to read the current native scene while stopping"
                + (f": {'; '.join(errors)}" if errors else "")
            )
        if current_payload.get("archive_session") != session:
            raise RuntimeError("PROJECT_CONTEXT_CHANGED")
        project = payload.get("project_path")
        if project and _Path(current_payload.get("project_path") or "").resolve() != _Path(project).resolve():
            raise RuntimeError("The active project changed; the preview snapshot was not restored")
        current = [
            item for item in current_payload.get("actors", []) if isinstance(item, dict)
        ]

        # Remove runtime-created actors. Prefer GUID; use name/route only as fallback.
        runtime_created = [
            item for item in current
            if not any(_snapshot_actor_match([item], expected) is not None for expected in original)
        ]
        if runtime_created and not callable(remover):
            names = ", ".join(str(item.get("name") or item.get("route") or "") for item in runtime_created)
            raise RuntimeError(f"Native remove actor API is unavailable; cannot remove: {names}")
        for item in runtime_created:
            actor_name = str(item.get("name") or item.get("route") or "")
            _native_payload_or_raise(
                remover(scene_route, actor_name),
                f"Remove runtime-created actor {actor_name}",
            )

        refreshed, errors = _native_scene_snapshot(scene_route)
        if refreshed is None:
            raise RuntimeError(
                "Unable to refresh the native scene after removing runtime actors"
                + (f": {'; '.join(errors)}" if errors else "")
            )
        current = [item for item in refreshed.get("actors", []) if isinstance(item, dict)]

        # Recreate original actors deleted during runtime.
        missing_originals = [item for item in original if _snapshot_actor_match(current, item) is None]
        if missing_originals and not callable(creator):
            names = ", ".join(str(item.get("name") or item.get("route") or "") for item in missing_originals)
            raise RuntimeError(f"Native create actor API is unavailable; cannot recreate: {names}")
        for item in missing_originals:
            route = str(item.get("route") or item.get("path") or item.get("model") or "")
            actor_type = str(item.get("actor_type") or "model")
            actor_name = str(item.get("name") or _Path(route).stem or "actor")
            actor_data = dict(item)
            actor_data.update(actor_name=actor_name, name=actor_name, skip_if_exists=False)
            _native_payload_or_raise(
                creator(scene_route, route, actor_type, actor_data),
                f"Recreate original actor {actor_name}",
            )

        refreshed, errors = _native_scene_snapshot(scene_route)
        if refreshed is None:
            raise RuntimeError(
                "Unable to refresh the native scene after recreating actors"
                + (f": {'; '.join(errors)}" if errors else "")
            )
        current = [item for item in refreshed.get("actors", []) if isinstance(item, dict)]
        for item in original:
            actor_name = str(item.get("name") or item.get("route") or "")
            current_actor = _snapshot_actor_match(current, item)
            if current_actor is None:
                raise RuntimeError(f"Actor {actor_name} is still missing after recreation")
            if _native_actor_state_mismatches(item, current_actor):
                _restore_native_actor_state(scene_route, item, session)

        # Native writes and render/physics synchronization may cross a short frame.
        # Re-read and verify; retry twice and never report restored on mismatch.
        _time.sleep(0.02 * (attempt + 1))
        verification_failures = _verify_native_scene_restored(scene_route, original)
        if not verification_failures:
            break
        _logger.warning(
            "[FileScripts] native scene restore verification failed (attempt %d/3): %s",
            attempt + 1,
            "; ".join(verification_failures),
        )

    if verification_failures:
        raise RuntimeError("Scene restore verification failed: " + "; ".join(verification_failures))

    camera_restored = _restore_native_camera_state(scene_route, payload.get("camera"))
    _logger.info(
        "[FileScripts] native scene restore verified: scene=%s camera=%s",
        scene_route,
        "restored" if camera_restored else "unavailable",
    )
    return True

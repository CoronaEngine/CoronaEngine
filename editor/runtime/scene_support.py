"""Runtime support for scene manifests and legacy scene auto-save.

This module owns scene persistence helpers used by the embedded script runtime.
The project support module re-exports these names for older import paths.
"""

import configparser
import logging
import threading
from functools import wraps
from typing import Any, Callable, Dict, Tuple

logger = logging.getLogger(__name__)


def get_project_scenes(ini_path: str) -> list[str]:
    """Read the ordered scene routes from a project's ``scenes`` field."""
    config = configparser.ConfigParser()
    config.read(ini_path, encoding="utf-8")
    raw = config.get("Project", "scenes", fallback="").strip()
    if not raw:
        return []
    return [scene.strip() for scene in raw.split(",") if scene.strip()]


def set_project_scenes(ini_path: str, scenes: list[str]) -> None:
    """Write the ordered scene routes to a project's ``scenes`` field."""
    config = configparser.ConfigParser()
    config.read(ini_path, encoding="utf-8")
    if "Project" not in config:
        config["Project"] = {}
    config["Project"]["scenes"] = ",".join(scenes)
    with open(ini_path, "w", encoding="utf-8") as handle:
        config.write(handle)


def append_project_scene(ini_path: str, scene_route: str) -> None:
    """Append a scene route unless it is already present."""
    scenes = get_project_scenes(ini_path)
    if scene_route not in scenes:
        scenes.append(scene_route)
        set_project_scenes(ini_path, scenes)


_save_timers: Dict[int, Tuple[threading.Timer, Any]] = {}
_save_timers_lock = threading.RLock()


def flush_pending_auto_saves() -> int:
    """Synchronously flush all debounced ``save_data`` calls."""
    with _save_timers_lock:
        pending = list(_save_timers.items())
        _save_timers.clear()

    flushed = 0
    for _, (timer, target) in pending:
        timer.cancel()
        try:
            if hasattr(target, "save_data"):
                target.save_data()
                flushed += 1
        except Exception:
            logger.exception("flush pending auto-save failed")
    return flushed


def cancel_pending_auto_saves() -> int:
    """Cancel all debounced ``save_data`` calls without flushing them."""
    with _save_timers_lock:
        pending = list(_save_timers.values())
        _save_timers.clear()

    for timer, _ in pending:
        timer.cancel()
    return len(pending)


def auto_save(func: Callable) -> Callable:
    """Debounce a ``save_data`` call after a mutating operation succeeds."""
    @wraps(func)
    def wrapper(self, *args, **kwargs):
        result = func(self, *args, **kwargs)
        if result is True and hasattr(self, "save_data"):
            obj_key = id(self)
            with _save_timers_lock:
                old = _save_timers.pop(obj_key, None)
            if old is not None:
                old[0].cancel()

            def _do_save():
                try:
                    self.save_data()
                except Exception:
                    logger.exception("Automatic scene save failed")
                finally:
                    with _save_timers_lock:
                        current = _save_timers.get(obj_key)
                        if current and current[0] is timer:
                            _save_timers.pop(obj_key, None)

            timer = threading.Timer(0.5, _do_save)
            with _save_timers_lock:
                _save_timers[obj_key] = (timer, self)
            timer.start()
        return result

    return wrapper

"""Thin Python proxy for C++ collaboration state.

Python agents may run local AI/tool logic, but network transport, locks,
preview intents, room state, member state, and agent roster live in C++.
"""
from __future__ import annotations

import logging
import threading
from typing import Any, Dict, List, Optional

logger = logging.getLogger(__name__)
_PREVIEW_COLLISION_DELTA = 0.5
EDITOR_API_OVERRIDE: Any | None = None


def _editor_api():
    if EDITOR_API_OVERRIDE is not None:
        return EDITOR_API_OVERRIDE
    try:
        from api.editor_api import CoronaEditorApi

        return CoronaEditorApi
    except Exception as exc:
        logger.debug("[Collab] editor network API unavailable: %s", exc)
        return None


def _network_api():
    api = _editor_api()
    return getattr(api, "network", None) if api is not None else None


def _position3(position: Optional[List[float]]) -> List[float]:
    values = list(position or [])
    while len(values) < 3:
        values.append(0.0)
    return [float(values[0]), float(values[1]), float(values[2])]


class CollaborationManager:
    def lock_object(self, object_id: str, user_id: str, operation: str = "modify") -> bool:
        network = _network_api()
        method = getattr(network, "lock_object", None) if network else None
        if not callable(method):
            return False
        result = method(object_id, user_id, operation)
        return bool(result.get("ok")) if isinstance(result, dict) else bool(result)

    def unlock_object(self, object_id: str, user_id: str) -> bool:
        network = _network_api()
        method = getattr(network, "unlock_object", None) if network else None
        if not callable(method):
            return False
        result = method(object_id, user_id)
        return bool(result.get("ok")) if isinstance(result, dict) else bool(result)

    def is_locked(self, object_id: str) -> Optional[str]:
        network = _network_api()
        method = getattr(network, "get_lock_owner", None) if network else None
        if not callable(method):
            return None
        result = method(object_id)
        owner = result.get("owner") if isinstance(result, dict) else result
        return owner or None

    def broadcast_intent(
        self,
        user_id: str,
        tooltip: str,
        preview_position: List[float] = None,
        status: str = "placing_object",
    ):
        network = _network_api()
        method = getattr(network, "broadcast_intent", None) if network else None
        if callable(method):
            return method(user_id, tooltip, _position3(preview_position), status)
        return None

    def clear_intent(self, user_id: str):
        self.broadcast_intent(user_id, "", [0.0, 0.0, 0.0], "idle")

    def get_active_intents(self) -> Dict[str, Dict[str, Any]]:
        return {}

    def get_status_bar_text(self) -> str:
        return "无活跃操作"

    def check_preview_collision(
        self,
        user_id: str,
        position: List[float],
        exclude_user: bool = True,
    ) -> Optional[str]:
        network = _network_api()
        method = getattr(network, "check_preview_collision", None) if network else None
        if not callable(method):
            return None
        result = method(
            user_id, _position3(position), _PREVIEW_COLLISION_DELTA
        )
        conflict = result.get("conflict_user_id") if isinstance(result, dict) else result
        if exclude_user and conflict == user_id:
            return None
        return conflict or None

    def clear(self):
        return None


_COLLAB_INSTANCE: Optional[CollaborationManager] = None
_COLLAB_LOCK = threading.Lock()


def get_collaboration_manager() -> CollaborationManager:
    global _COLLAB_INSTANCE
    if _COLLAB_INSTANCE is None:
        with _COLLAB_LOCK:
            if _COLLAB_INSTANCE is None:
                _COLLAB_INSTANCE = CollaborationManager()
    return _COLLAB_INSTANCE


__all__ = ["CollaborationManager", "get_collaboration_manager"]

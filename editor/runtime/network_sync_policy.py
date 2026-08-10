"""Central policy for deciding which editor objects participate in network sync."""

from __future__ import annotations

import logging
import threading
import weakref
from contextvars import ContextVar
from dataclasses import dataclass
from pathlib import PurePath
from typing import Callable, Optional

logger = logging.getLogger(__name__)

EmitActorCreate = Callable[[dict], None]
PrepareActorCreate = Callable[[object], None]

_lock = threading.RLock()
_active_transaction: ContextVar[Optional["_TransactionState"]] = ContextVar(
    "network_sync_active_transaction",
    default=None,
)
_preserved_entries_by_key: dict[str, list["_QueuedActorCreate"]] = {}
_published_actor_create_ids: set[str] = set()
_sync_pause_depth = 0
_DEFAULT_TRANSACTION_KEY = "__default__"
_AI_SCENE_FRAMEWORK_SYNC_NAMES = {
    "__room_box",
    "__room_terrain",
    "__terrain_grass",
    "__terrain_boundary",
    "__interior_floor",
    "__foundation_surface",
}
_AI_SCENE_FRAMEWORK_SYNC_PREFIXES = ("__shell_",)


@dataclass
class _QueuedActorCreate:
    actor_ref: weakref.ReferenceType
    prepare: Optional[PrepareActorCreate]
    emit: EmitActorCreate


@dataclass
class _TransactionState:
    key: str
    entries: list[_QueuedActorCreate]
    depth: int = 0
    rollback_requested: bool = False


def _last_path_part(value: object) -> str:
    text = str(value or "").replace("\\", "/").strip()
    if not text:
        return ""
    return PurePath(text).name or text


def is_internal_sync_name(value: object) -> bool:
    """Return True for the shared internal/temp naming convention."""
    text = str(value or "").strip()
    if text.startswith("__"):
        return True
    return _last_path_part(text).startswith("__")


def is_ai_scene_framework_sync_name(value: object) -> bool:
    text = str(value or "").strip()
    leaf = _last_path_part(text)
    return (
        text in _AI_SCENE_FRAMEWORK_SYNC_NAMES
        or leaf in _AI_SCENE_FRAMEWORK_SYNC_NAMES
        or any(
            text.startswith(prefix) or leaf.startswith(prefix)
            for prefix in _AI_SCENE_FRAMEWORK_SYNC_PREFIXES
        )
    )


def is_internal_actor_sync_name(value: object) -> bool:
    return is_internal_sync_name(value) and not is_ai_scene_framework_sync_name(value)


def actor_data_is_syncable(actor_data: dict | None) -> bool:
    """Defensive frontend-facing data filter mirrored from the Python policy."""
    return actor_data_sync_block_reason(actor_data) is None


def actor_data_sync_block_reason(actor_data: dict | None) -> str | None:
    if not isinstance(actor_data, dict):
        return "invalid_actor_data"
    if actor_data.get("_suppress_network_broadcast"):
        return "suppressed"
    if actor_data.get("actor_type", "") == "actor":
        return "actor_type_actor"
    if not isinstance(actor_data.get("geometry"), dict):
        return "missing_geometry"
    if is_internal_actor_sync_name(actor_data.get("name")):
        return "internal_actor_name"
    if is_internal_sync_name(actor_data.get("scene")):
        return "internal_scene_name"
    if not (actor_data.get("path") or actor_data.get("model")):
        return "missing_model_path"
    return None


def actor_is_syncable(actor: object) -> bool:
    """Return True if an Actor is eligible for network create sync."""
    return actor_sync_block_reason(actor) is None


def actor_sync_block_reason(actor: object) -> str | None:
    if getattr(actor, "_suppress_network_broadcast", False):
        return "suppressed"
    if getattr(actor, "actor_type", "") == "actor":
        return "actor_type_actor"
    if not hasattr(actor, "_geometry"):
        return "missing_geometry"
    if is_internal_actor_sync_name(getattr(actor, "name", "")):
        return "internal_actor_name"

    parent = getattr(actor, "parent", None)
    if parent is None:
        return "missing_parent_scene"
    scene_route = getattr(parent, "route", "") or getattr(parent, "name", "")
    if is_internal_sync_name(scene_route):
        return "internal_scene_name"
    return None


def _log_filtered_actor_create(reason: str, actor: object, actor_data: dict | None = None) -> None:
    data = actor_data if isinstance(actor_data, dict) else {}
    parent = getattr(actor, "parent", None)
    scene = (
        data.get("scene")
        or getattr(parent, "route", "")
        or getattr(parent, "name", "")
    )
    logger.warning(
        "Actor create network sync filtered: reason=%s actor='%s' type='%s' "
        "guid='%s' scene='%s' path='%s' model='%s'",
        reason,
        data.get("name") or getattr(actor, "name", ""),
        data.get("actor_type") or getattr(actor, "actor_type", ""),
        data.get("actor_guid") or getattr(actor, "actor_guid", ""),
        scene,
        data.get("path") or getattr(actor, "route", ""),
        data.get("model") or getattr(actor, "model_path", ""),
    )


def actor_is_still_in_scene(actor: object) -> bool:
    """Return True if a deferred Actor still exists in its scene at flush time."""
    if not actor_is_syncable(actor):
        return False
    parent = getattr(actor, "parent", None)
    get_actors = getattr(parent, "get_actors", None)
    if not callable(get_actors):
        return True
    try:
        return actor in get_actors()
    except Exception:
        logger.debug("Failed to inspect scene actors during network sync flush", exc_info=True)
        return True


def publish_actor_created(
    actor: object,
    *,
    prepare: Optional[PrepareActorCreate],
    emit: EmitActorCreate,
) -> None:
    """Publish, queue, or drop an Actor create event according to the sync policy."""
    reason = actor_sync_block_reason(actor)
    if reason is not None:
        _log_filtered_actor_create(reason, actor)
        return

    transaction = _active_transaction.get()
    if transaction is not None and transaction.depth > 0:
        transaction.entries.append(_QueuedActorCreate(weakref.ref(actor), prepare, emit))
        return

    _emit_actor_create(actor, prepare, emit)


def _actor_create_identity_key(actor: object) -> str:
    parent = getattr(actor, "parent", None)
    scene = str(getattr(parent, "route", "") or getattr(parent, "name", "") or "")
    actor_guid = str(getattr(actor, "actor_guid", "") or "")
    return f"{scene}:{actor_guid}" if scene and actor_guid else ""


def set_engine_sync_paused(paused: bool) -> None:
    """Best-effort request for the frontend Network bridge to pause dirty sync."""
    global _sync_pause_depth
    with _lock:
        if paused:
            _sync_pause_depth += 1
            should_emit = _sync_pause_depth == 1
        else:
            if _sync_pause_depth > 0:
                _sync_pause_depth -= 1
            should_emit = _sync_pause_depth == 0
        if not should_emit:
            return

    try:
        from runtime.editor_host import emit_editor_event

        emit_editor_event(
            "network-sync-pause-request", [{"paused": bool(paused)}]
        )
    except Exception:
        logger.debug("Failed to emit network sync pause request", exc_info=True)


class DeferredActorBroadcasts:
    """Transaction that defers actor-create broadcasts until a workflow boundary."""

    def __init__(self, *, pause_engine_sync: bool = False, transaction_key: object = None):
        self._pause_engine_sync = pause_engine_sync
        self._transaction_key = str(transaction_key or _DEFAULT_TRANSACTION_KEY)
        self._outcome: str | None = None
        self._closed = False
        self._state: _TransactionState | None = None
        self._token = None
        self._owns_context = False

    def __enter__(self) -> "DeferredActorBroadcasts":
        state = _active_transaction.get()
        if state is None:
            state = _TransactionState(key=self._transaction_key, entries=[], depth=0)
            self._token = _active_transaction.set(state)
            self._owns_context = True
        self._state = state
        state.depth += 1
        if self._pause_engine_sync:
            set_engine_sync_paused(True)
        return self

    def commit(self) -> None:
        self._outcome = "commit"

    def preserve(self) -> None:
        self._outcome = "preserve"

    def rollback(self) -> None:
        self._outcome = "rollback"

    def __exit__(self, exc_type, exc, tb) -> bool:
        if exc_type is not None:
            self.rollback()
        elif self._outcome is None:
            self.commit()
        self.close()
        return False

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True

        state = self._state
        if state is None:
            return

        entries_to_flush: list[_QueuedActorCreate] = []
        outcome = self._outcome or "commit"
        if outcome == "rollback":
            state.rollback_requested = True

        state.depth = max(0, state.depth - 1)
        should_finish = state.depth == 0
        if not should_finish:
            if self._pause_engine_sync:
                set_engine_sync_paused(False)
            return

        with _lock:
            if state.rollback_requested:
                _preserved_entries_by_key.pop(state.key, None)
            elif outcome == "preserve":
                _preserved_entries_by_key.setdefault(state.key, []).extend(state.entries)
            else:
                preserved = _preserved_entries_by_key.pop(state.key, [])
                entries_to_flush = [*preserved, *state.entries]

        if self._owns_context and self._token is not None:
            _active_transaction.reset(self._token)

        try:
            if entries_to_flush:
                _flush_actor_creates(entries_to_flush)
        finally:
            if self._pause_engine_sync:
                set_engine_sync_paused(False)


def deferred_actor_broadcasts(
    *,
    pause_engine_sync: bool = False,
    transaction_key: object = None,
) -> DeferredActorBroadcasts:
    return DeferredActorBroadcasts(
        pause_engine_sync=pause_engine_sync,
        transaction_key=transaction_key,
    )


def _flush_actor_creates(entries: list[_QueuedActorCreate]) -> None:
    seen: set[str] = set()
    for entry in entries:
        actor = entry.actor_ref()
        if actor is None:
            logger.debug("Deferred actor create network sync skipped: actor reference expired")
            continue
        reason = actor_sync_block_reason(actor)
        if reason is not None:
            logger.info(
                "Deferred actor create network sync filtered during flush: reason=%s actor='%s'",
                reason,
                getattr(actor, "name", ""),
            )
            _log_filtered_actor_create(reason, actor)
            continue
        if not actor_is_still_in_scene(actor):
            logger.info(
                "Deferred actor create network sync skipped: actor left scene actor='%s' guid='%s'",
                getattr(actor, "name", ""),
                getattr(actor, "actor_guid", ""),
            )
            continue
        actor_guid = str(getattr(actor, "actor_guid", "") or id(actor))
        if actor_guid in seen:
            continue
        seen.add(actor_guid)
        _emit_actor_create(actor, entry.prepare, entry.emit)


def _emit_actor_create(
    actor: object,
    prepare: Optional[PrepareActorCreate],
    emit: EmitActorCreate,
) -> None:
    identity_key = _actor_create_identity_key(actor)
    if identity_key:
        with _lock:
            if identity_key in _published_actor_create_ids:
                logger.debug("Actor create network sync deduped: key=%s", identity_key)
                return
    if prepare is not None:
        prepare(actor)
    actor_data = actor.to_dict()
    reason = actor_data_sync_block_reason(actor_data)
    if reason is not None:
        _log_filtered_actor_create(reason, actor, actor_data)
        return
    emit(actor_data)
    if identity_key:
        with _lock:
            _published_actor_create_ids.add(identity_key)


def reset_for_tests() -> None:
    global _preserved_entries_by_key, _published_actor_create_ids, _sync_pause_depth
    with _lock:
        _active_transaction.set(None)
        _preserved_entries_by_key = {}
        _published_actor_create_ids = set()
        _sync_pause_depth = 0

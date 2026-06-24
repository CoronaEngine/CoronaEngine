from __future__ import annotations

import asyncio
import inspect
import logging
import threading
import time
import uuid
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable


class GenerationJobStatus(str, Enum):
    QUEUED = "queued"
    WAITING_USER = "waiting_user"
    PREPARING = "preparing"
    COMPOSING = "composing"
    SUBMITTING = "submitting"
    POLLING = "polling"
    DOWNLOADING = "downloading"
    POSTPROCESSING = "postprocessing"
    IMPORTING = "importing"
    DONE = "done"
    FAILED = "failed"
    CANCELLED = "cancelled"
    ABANDONED = "abandoned"
    PAUSED = "paused"


TERMINAL_STATUSES = {
    GenerationJobStatus.DONE,
    GenerationJobStatus.FAILED,
    GenerationJobStatus.CANCELLED,
    GenerationJobStatus.ABANDONED,
}


STAGE_TO_STATUS = {
    "prepare": GenerationJobStatus.PREPARING,
    "compose": GenerationJobStatus.COMPOSING,
    "submit": GenerationJobStatus.SUBMITTING,
    "poll": GenerationJobStatus.POLLING,
    "download": GenerationJobStatus.DOWNLOADING,
    "postprocess": GenerationJobStatus.POSTPROCESSING,
    "import": GenerationJobStatus.IMPORTING,
}


DEFAULT_STAGE_ORDER = ("prepare", "submit", "poll", "download", "postprocess", "import")


@dataclass
class GenerationJob:
    payload: dict[str, Any]
    runtime_context: dict[str, Any] = field(default_factory=dict)
    job_id: str = field(default_factory=lambda: f"gen-{uuid.uuid4().hex[:12]}")
    session_id: str = ""
    room_id: str = ""
    plan_id: str = ""
    batch_id: str = ""
    priority: int = 0
    sequence: int = 0
    status: GenerationJobStatus = GenerationJobStatus.QUEUED
    current_stage: str = ""
    error: str = ""
    created_at: float = field(default_factory=time.time)
    updated_at: float = field(default_factory=time.time)
    cancel_requested: bool = False
    abandon_requested: bool = False
    result: dict[str, Any] = field(default_factory=dict)
    _done_event: threading.Event = field(default_factory=threading.Event, repr=False)

    def as_dict(self) -> dict[str, Any]:
        return {
            "job_id": self.job_id,
            "session_id": self.session_id,
            "room_id": self.room_id,
            "plan_id": self.plan_id,
            "batch_id": self.batch_id,
            "priority": self.priority,
            "sequence": self.sequence,
            "status": self.status.value,
            "current_stage": self.current_stage,
            "error": self.error,
            "created_at": self.created_at,
            "updated_at": self.updated_at,
            "cancel_requested": self.cancel_requested,
            "abandon_requested": self.abandon_requested,
            "result": dict(self.result),
            "payload": dict(self.payload),
        }

    def compose_kwargs(self) -> dict[str, Any]:
        """Runtime-only kwargs for SceneComposer.compose()."""
        return {
            "interaction_coordinator": self.runtime_context.get("interaction_coordinator"),
            "room_id": self.room_id or str(self.payload.get("room_id") or self.session_id or ""),
            "plan_id": self.plan_id,
            "session_id": self.session_id,
        }


class GenerationScheduler:
    """Single-thread asyncio scheduler for AI generation stages.

    It is intentionally provider-agnostic: concrete Hunyuan/Rodin/image calls can
    be migrated behind stage handlers without changing LANChat / SeedPlan flow.
    """

    def __init__(
        self,
        *,
        stage_handlers: dict[str, Callable[[GenerationJob], Any]] | None = None,
        stage_order: tuple[str, ...] = DEFAULT_STAGE_ORDER,
        concurrency: dict[str, int] | None = None,
        queue_limit: int = 0,
        max_retained_terminal_jobs: int = 256,
        thread_name: str = "AI_GenerationScheduler",
        auto_start: bool = True,
    ) -> None:
        self._stage_handlers = dict(stage_handlers or {})
        self._stage_order = tuple(stage_order)
        self._concurrency = {
            "prepare": 1,
            "submit": 2,
            "poll": 32,
            "download": 4,
            "postprocess": 1,
            "import": 1,
            **(concurrency or {}),
        }
        self._queue_limit = int(queue_limit or 0)
        self._max_retained_terminal_jobs = int(max_retained_terminal_jobs or 0)
        self._pruned_terminal_jobs = 0
        self._thread_name = thread_name
        self._loop: asyncio.AbstractEventLoop | None = None
        self._queue: asyncio.Queue[str] | None = None
        self._thread: threading.Thread | None = None
        self._ready = threading.Event()
        self._stop_requested = threading.Event()
        self._shutdown_requested = False
        self._lock = threading.RLock()
        self._jobs: dict[str, GenerationJob] = {}
        self._pending_job_ids: set[str] = set()
        self._next_job_sequence = 0
        self._paused_sessions: set[str] = set()
        self._recent_events: list[dict[str, Any]] = []
        self._recent_event_limit = 100
        self._semaphores: dict[str, asyncio.Semaphore] = {}
        self._logger = logging.getLogger(__name__)
        if auto_start:
            self.ensure_running()

    def ensure_running(self) -> None:
        with self._lock:
            if self._shutdown_requested:
                raise RuntimeError("GenerationScheduler has been shut down")
        if self._thread is not None and self._thread.is_alive():
            return
        self._stop_requested.clear()
        self._ready.clear()
        self._thread = threading.Thread(target=self._run_loop, name=self._thread_name, daemon=True)
        self._thread.start()
        self._ready.wait(timeout=2.0)

    def submit(self, payload: dict[str, Any]) -> dict[str, Any]:
        payload = dict(payload or {})
        runtime_context = dict(payload.pop("_runtime_context", None) or {})
        if "interaction_coordinator" in payload:
            runtime_context.setdefault("interaction_coordinator", payload.pop("interaction_coordinator"))
        with self._lock:
            if self._shutdown_requested:
                self._record_event_locked(
                    "submit_rejected_after_shutdown",
                    status="rejected",
                    session_id=str(payload.get("session_id") or payload.get("room_id") or ""),
                    room_id=str(payload.get("room_id") or payload.get("session_id") or ""),
                    plan_id=str(payload.get("plan_id") or ""),
                    batch_id=str(payload.get("batch_id") or ""),
                    priority=int(payload.get("priority") or 0),
                )
                return {
                    "job_id": "",
                    "status": "rejected",
                    "success": False,
                    "error": "generation scheduler has been shut down",
                }
        self.ensure_running()
        with self._lock:
            if self._queue_limit and self._queued_count_locked() >= self._queue_limit:
                self._record_event_locked(
                    "queue_full",
                    status=GenerationJobStatus.WAITING_USER.value,
                    session_id=str(payload.get("session_id") or payload.get("room_id") or ""),
                    room_id=str(payload.get("room_id") or payload.get("session_id") or ""),
                    plan_id=str(payload.get("plan_id") or ""),
                    batch_id=str(payload.get("batch_id") or ""),
                    priority=int(payload.get("priority") or 0),
                )
                return {
                    "job_id": "",
                    "status": GenerationJobStatus.WAITING_USER.value,
                    "error": "generation queue is full",
                }
            sequence = self._next_job_sequence
            self._next_job_sequence += 1
            job = GenerationJob(
                payload=payload,
                runtime_context=runtime_context,
                job_id=str(payload.get("job_id") or f"gen-{uuid.uuid4().hex[:12]}"),
                session_id=str(payload.get("session_id") or payload.get("room_id") or ""),
                room_id=str(payload.get("room_id") or payload.get("session_id") or ""),
                plan_id=str(payload.get("plan_id") or ""),
                batch_id=str(payload.get("batch_id") or ""),
                priority=int(payload.get("priority") or 0),
                sequence=sequence,
            )
            self._jobs[job.job_id] = job
            self._record_event_locked("submit", **self._job_ref(job))
        self._call_soon(self._enqueue_job, job.job_id)
        return job.as_dict()

    def status(self, job_id: str) -> dict[str, Any]:
        with self._lock:
            job = self._jobs.get(job_id)
            if job is None:
                return {"job_id": job_id, "status": "not_found"}
            return job.as_dict()

    def snapshot(self) -> dict[str, Any]:
        """Return an internal diagnostic snapshot without runtime payloads."""
        with self._lock:
            jobs = list(self._jobs.values())
            status_counts: dict[str, int] = {}
            stage_counts: dict[str, int] = {}
            for job in jobs:
                status_counts[job.status.value] = status_counts.get(job.status.value, 0) + 1
                if job.current_stage:
                    stage_counts[job.current_stage] = stage_counts.get(job.current_stage, 0) + 1
            queued_jobs = sorted(
                (
                    self._job_ref(job)
                    for job in jobs
                    if job.status == GenerationJobStatus.QUEUED
                ),
                key=lambda item: (-int(item["priority"]), int(item.get("sequence") or 0), str(item["job_id"])),
            )
            active_jobs = [
                self._job_ref(job)
                for job in jobs
                if job.status not in TERMINAL_STATUSES and job.status != GenerationJobStatus.QUEUED
            ]
            queued_count = self._queued_count_locked()
            queue_pressure = 0.0
            if self._queue_limit:
                queue_pressure = min(1.0, queued_count / float(self._queue_limit))
            snapshot = {
                "thread_alive": bool(self._thread and self._thread.is_alive()),
                "stop_requested": self._stop_requested.is_set(),
                "shutdown_requested": self._shutdown_requested,
                "queue_limit": self._queue_limit,
                "queued_count": queued_count,
                "queue_pressure": queue_pressure,
                "paused_sessions": sorted(self._paused_sessions),
                "status_counts": status_counts,
                "stage_counts": stage_counts,
                "concurrency": dict(self._concurrency),
                "stage_order": list(self._stage_order),
                "queued_jobs": queued_jobs,
                "active_jobs": active_jobs,
                "recent_events": list(self._recent_events),
                "total_jobs": len(jobs),
                "retained_terminal_job_limit": self._max_retained_terminal_jobs,
                "pruned_terminal_jobs": self._pruned_terminal_jobs,
            }
            snapshot["diagnosis"] = self._diagnose_snapshot_locked(snapshot)
            return snapshot

    def public_snapshot(self) -> dict[str, Any]:
        """Return a UI/chat-safe scheduler diagnostic snapshot."""
        return self._public_snapshot_from(self.snapshot())

    def session_snapshot(self, session_id: str) -> dict[str, Any]:
        """Return an internal scheduler snapshot for one room/session."""
        session = str(session_id or "")
        with self._lock:
            jobs = [
                job
                for job in self._jobs.values()
                if self._job_matches_session_or_room(job, session)
            ]
            status_counts: dict[str, int] = {}
            stage_counts: dict[str, int] = {}
            for job in jobs:
                status_counts[job.status.value] = status_counts.get(job.status.value, 0) + 1
                if job.current_stage:
                    stage_counts[job.current_stage] = stage_counts.get(job.current_stage, 0) + 1
            queued_jobs = sorted(
                (
                    self._job_ref(job)
                    for job in jobs
                    if job.status == GenerationJobStatus.QUEUED
                ),
                key=lambda item: (-int(item["priority"]), int(item.get("sequence") or 0), str(item["job_id"])),
            )
            active_jobs = [
                self._job_ref(job)
                for job in jobs
                if job.status not in TERMINAL_STATUSES and job.status != GenerationJobStatus.QUEUED
            ]
            queued_count = sum(
                1
                for job in jobs
                if job.status in {GenerationJobStatus.QUEUED, GenerationJobStatus.PAUSED}
            )
            queue_pressure = 0.0
            if self._queue_limit:
                queue_pressure = min(1.0, queued_count / float(self._queue_limit))
            recent_events = [
                dict(event)
                for event in self._recent_events
                if session in {str(event.get("session_id") or ""), str(event.get("room_id") or "")}
            ]
            paused = bool(session and session in self._paused_sessions)
            snapshot = {
                "session_id": session,
                "thread_alive": bool(self._thread and self._thread.is_alive()),
                "stop_requested": self._stop_requested.is_set(),
                "shutdown_requested": self._shutdown_requested,
                "queue_limit": self._queue_limit,
                "queued_count": queued_count,
                "queue_pressure": queue_pressure,
                "active_count": len(active_jobs),
                "paused": paused,
                "paused_sessions": [session] if paused else [],
                "status_counts": status_counts,
                "stage_counts": stage_counts,
                "queued_jobs": queued_jobs,
                "active_jobs": active_jobs,
                "recent_events": recent_events,
                "total_jobs": len(jobs),
                "retained_terminal_job_limit": self._max_retained_terminal_jobs,
                "pruned_terminal_jobs": self._pruned_terminal_jobs,
            }
            snapshot["diagnosis"] = self._diagnose_snapshot_locked(snapshot)
            snapshot["state"] = snapshot["diagnosis"]["state"]
            return snapshot

    def public_session_snapshot(self, session_id: str) -> dict[str, Any]:
        """Return a UI/chat-safe scheduler snapshot for one room/session."""
        return self._public_snapshot_from(self.session_snapshot(session_id))

    def cancel(self, job_id: str, *, abandon_remote: bool = False) -> dict[str, Any]:
        with self._lock:
            job = self._jobs.get(job_id)
            if job is None:
                return {"job_id": job_id, "status": "not_found", "success": False}
            if job.status in TERMINAL_STATUSES:
                return {"job_id": job_id, "status": job.status.value, "success": False}
            job.cancel_requested = True
            job.abandon_requested = bool(abandon_remote)
            self._record_event_locked("cancel_requested", **self._job_ref(job))
            if job.status == GenerationJobStatus.QUEUED:
                self._pending_job_ids.discard(job.job_id)
                self._set_status_locked(
                    job,
                    GenerationJobStatus.ABANDONED if abandon_remote else GenerationJobStatus.CANCELLED,
                    error="cancelled before submit",
                )
                job._done_event.set()
        return {"job_id": job_id, "status": "cancelling", "success": True}

    def cancel_session(self, session_id: str, *, abandon_remote: bool = False) -> dict[str, Any]:
        """Cancel all non-terminal jobs for a room/session.

        This is the control-plane primitive used when a multiplayer room is
        closed or abandoned: queued work is removed immediately, and running
        work observes cancel_requested at the next scheduler boundary.
        """
        session = str(session_id or "")
        cancelled_job_ids: list[str] = []
        if not session:
            return {"session_id": session, "status": "not_found", "success": False, "job_ids": []}
        with self._lock:
            self._paused_sessions.discard(session)
            jobs = [
                job
                for job in self._jobs.values()
                if self._job_matches_session_or_room(job, session) and job.status not in TERMINAL_STATUSES
            ]
            for job in jobs:
                job.cancel_requested = True
                job.abandon_requested = bool(abandon_remote)
                cancelled_job_ids.append(job.job_id)
                self._record_event_locked("cancel_requested", **self._job_ref(job))
                if job.status in {GenerationJobStatus.QUEUED, GenerationJobStatus.PAUSED}:
                    self._pending_job_ids.discard(job.job_id)
                    self._set_status_locked(
                        job,
                        GenerationJobStatus.ABANDONED if abandon_remote else GenerationJobStatus.CANCELLED,
                        error="session cancelled",
                    )
                    job._done_event.set()
            self._record_event_locked(
                "cancel_session",
                session_id=session,
                cancelled_count=len(cancelled_job_ids),
            )
        return {
            "session_id": session,
            "status": "cancelling" if cancelled_job_ids else "not_found",
            "success": bool(cancelled_job_ids),
            "cancelled_count": len(cancelled_job_ids),
            "job_ids": cancelled_job_ids,
        }

    def update_job(
        self,
        job_id: str,
        *,
        priority: int | None = None,
        payload_updates: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """Update a queued future batch before any provider stage starts."""
        with self._lock:
            job = self._jobs.get(job_id)
            if job is None:
                return {"job_id": job_id, "status": "not_found", "success": False}
            if job.status != GenerationJobStatus.QUEUED:
                return {
                    "job_id": job_id,
                    "status": job.status.value,
                    "success": False,
                    "error": "only queued jobs can be updated",
                }
            if priority is not None:
                job.priority = int(priority)
                job.payload["priority"] = int(priority)
            if payload_updates:
                job.payload.update(dict(payload_updates))
            job.updated_at = time.time()
            self._record_event_locked("update", **self._job_ref(job))
            updated = job.as_dict()
        updated["success"] = True
        return updated

    def pause_session(self, session_id: str) -> dict[str, Any]:
        with self._lock:
            session = str(session_id)
            self._paused_sessions.add(session)
            self._record_event_locked("pause_session", session_id=session)
        return {"session_id": session_id, "status": "paused", "success": True}

    def resume_session(self, session_id: str) -> dict[str, Any]:
        with self._lock:
            session = str(session_id)
            self._paused_sessions.discard(session)
            self._record_event_locked("resume_session", session_id=session)
        return {"session_id": session_id, "status": "running", "success": True}

    def wait(self, job_id: str, timeout: float = 5.0) -> dict[str, Any]:
        with self._lock:
            job = self._jobs.get(job_id)
        if job is None:
            return {"job_id": job_id, "status": "not_found"}
        completed = job._done_event.wait(timeout=timeout)
        with self._lock:
            current = self._jobs.get(job_id)
            if current is not None:
                return current.as_dict()
        if completed and job.status in TERMINAL_STATUSES:
            return job.as_dict()
        return {"job_id": job_id, "status": "not_found"}

    def shutdown(self, timeout: float = 2.0) -> None:
        with self._lock:
            self._shutdown_requested = True
            self._stop_requested.set()
        if self._loop is not None and not self._loop.is_closed():
            self._loop.call_soon_threadsafe(self._loop.stop)
        if self._thread is not None and self._thread.is_alive():
            self._thread.join(timeout=timeout)

    def _run_loop(self) -> None:
        loop = asyncio.new_event_loop()
        self._loop = loop
        asyncio.set_event_loop(loop)
        self._queue = asyncio.Queue()
        self._semaphores = {
            stage: asyncio.Semaphore(max(1, int(limit)))
            for stage, limit in self._concurrency.items()
        }
        loop.create_task(self._worker())
        self._ready.set()
        try:
            loop.run_forever()
        finally:
            pending = [task for task in asyncio.all_tasks(loop) if not task.done()]
            for task in pending:
                task.cancel()
            if pending:
                loop.run_until_complete(asyncio.gather(*pending, return_exceptions=True))
            loop.close()

    def _call_soon(self, callback: Callable[..., Any], *args: Any) -> None:
        if self._loop is None:
            raise RuntimeError("GenerationScheduler loop is not running")
        self._loop.call_soon_threadsafe(callback, *args)

    def _enqueue_job(self, job_id: str) -> None:
        if self._queue is None:
            raise RuntimeError("GenerationScheduler queue is not initialized")
        with self._lock:
            job = self._jobs.get(job_id)
            if job is not None and job.status == GenerationJobStatus.QUEUED:
                self._pending_job_ids.add(job_id)
        self._queue.put_nowait(job_id)

    async def _worker(self) -> None:
        assert self._queue is not None
        while not self._stop_requested.is_set():
            await self._queue.get()
            try:
                job_id = self._take_next_queued_job_id()
                if job_id is not None:
                    await self._run_job(job_id)
            finally:
                self._queue.task_done()

    def _take_next_queued_job_id(self) -> str | None:
        with self._lock:
            candidates = [
                self._jobs[job_id]
                for job_id in self._pending_job_ids
                if job_id in self._jobs and self._jobs[job_id].status == GenerationJobStatus.QUEUED
            ]
            if not candidates:
                return None
            selected = min(candidates, key=lambda job: (-job.priority, job.sequence, job.job_id))
            self._pending_job_ids.discard(selected.job_id)
            return selected.job_id

    async def _run_job(self, job_id: str) -> None:
        with self._lock:
            job = self._jobs.get(job_id)
        if job is None:
            return
        try:
            stage_order = tuple(job.runtime_context.get("stage_order") or self._stage_order)
            for stage in stage_order:
                if await self._maybe_cancel(job):
                    return
                await self._wait_if_paused(job)
                if await self._maybe_cancel(job):
                    return
                status = STAGE_TO_STATUS.get(stage, GenerationJobStatus.PREPARING)
                with self._lock:
                    self._set_status_locked(job, status, stage=stage)
                semaphore = self._semaphores.setdefault(stage, asyncio.Semaphore(1))
                async with semaphore:
                    if await self._maybe_cancel(job):
                        return
                    result = await self._run_stage_handler(stage, job)
                    if isinstance(result, dict):
                        paused_result = self._paused_stage_result(result)
                        with self._lock:
                            job.result.update(result)
                            job.updated_at = time.time()
                            if paused_result:
                                self._set_status_locked(job, GenerationJobStatus.PAUSED, stage=stage)
                                return
            with self._lock:
                self._set_status_locked(job, GenerationJobStatus.DONE, stage="")
        except asyncio.CancelledError:
            with self._lock:
                self._set_status_locked(job, GenerationJobStatus.CANCELLED, error="scheduler task cancelled")
        except Exception as exc:  # noqa: BLE001
            self._logger.warning("Generation job failed", exc_info=True)
            with self._lock:
                self._set_status_locked(job, GenerationJobStatus.FAILED, error=str(exc))
        finally:
            job._done_event.set()
            self._notify_terminal_job(job)

    async def _run_stage_handler(self, stage: str, job: GenerationJob) -> Any:
        job_handlers = job.runtime_context.get("stage_handlers")
        if isinstance(job_handlers, dict) and stage in job_handlers:
            handler = job_handlers.get(stage)
        else:
            handler = self._stage_handlers.get(stage)
        if handler is None:
            if str(job.payload.get("job_type") or "") == "scene_generation":
                raise RuntimeError(f"scene_generation stage handler missing: {stage}")
            await asyncio.sleep(0)
            return {"completed_stages": [*job.result.get("completed_stages", []), stage]}
        value = handler(job)
        if inspect.isawaitable(value):
            value = await value
        return value

    @staticmethod
    def _paused_stage_result(result: dict[str, Any]) -> dict[str, Any] | None:
        if result.get("paused"):
            return result
        compose_result = result.get("compose_result")
        if isinstance(compose_result, dict) and compose_result.get("paused"):
            return compose_result
        return None

    def _notify_terminal_job(self, job: GenerationJob) -> None:
        if job.status not in TERMINAL_STATUSES:
            return
        coordinator = job.runtime_context.get("interaction_coordinator")
        handler = job.runtime_context.get("on_job_complete")
        if not callable(handler) and coordinator is not None:
            handler = getattr(coordinator, "ingest_generation_job_status", None)
        if not callable(handler):
            return
        try:
            handler(job.as_dict())
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Generation job terminal callback failed: %s", exc)

    async def _wait_if_paused(self, job: GenerationJob) -> None:
        while True:
            if await self._maybe_cancel(job):
                return
            with self._lock:
                paused = any(
                    identifier and identifier in self._paused_sessions
                    for identifier in (job.session_id, job.room_id)
                )
                if paused:
                    self._set_status_locked(job, GenerationJobStatus.PAUSED)
            if not paused:
                return
            await asyncio.sleep(0.01)

    async def _maybe_cancel(self, job: GenerationJob) -> bool:
        with self._lock:
            if not job.cancel_requested:
                return False
            terminal = (
                GenerationJobStatus.ABANDONED
                if job.abandon_requested or job.status in {GenerationJobStatus.POLLING, GenerationJobStatus.DOWNLOADING}
                else GenerationJobStatus.CANCELLED
            )
            self._set_status_locked(job, terminal, error="cancel requested")
            job._done_event.set()
            return True

    def _queued_count_locked(self) -> int:
        return sum(
            1
            for job in self._jobs.values()
            if job.status in {GenerationJobStatus.QUEUED, GenerationJobStatus.PAUSED}
        )

    @staticmethod
    def _job_matches_session_or_room(job: GenerationJob, identifier: str) -> bool:
        target = str(identifier or "")
        if not target:
            return False
        return target in {str(job.session_id or ""), str(job.room_id or "")}

    @classmethod
    def _public_snapshot_from(cls, snapshot: dict[str, Any]) -> dict[str, Any]:
        safe = {
            "available": snapshot.get("available") if "available" in snapshot else None,
            "thread_alive": bool(snapshot.get("thread_alive")),
            "stop_requested": bool(snapshot.get("stop_requested")),
            "shutdown_requested": bool(snapshot.get("shutdown_requested")),
            "queue_limit": int(snapshot.get("queue_limit") or 0),
            "queued_count": int(snapshot.get("queued_count") or 0),
            "queue_pressure": float(snapshot.get("queue_pressure") or 0.0),
            "active_count": int(snapshot.get("active_count") or len(snapshot.get("active_jobs") or [])),
            "paused": bool(snapshot.get("paused")),
            "paused_session_count": len(snapshot.get("paused_sessions") or []),
            "status_counts": dict(snapshot.get("status_counts") or {}),
            "stage_counts": dict(snapshot.get("stage_counts") or {}),
            "queued_jobs": [
                cls._public_job_ref(item)
                for item in list(snapshot.get("queued_jobs") or [])[:20]
                if isinstance(item, dict)
            ],
            "active_jobs": [
                cls._public_job_ref(item)
                for item in list(snapshot.get("active_jobs") or [])[:20]
                if isinstance(item, dict)
            ],
            "recent_events": [
                cls._public_event_ref(item)
                for item in list(snapshot.get("recent_events") or [])[-20:]
                if isinstance(item, dict)
            ],
            "total_jobs": int(snapshot.get("total_jobs") or 0),
            "retained_terminal_job_limit": int(snapshot.get("retained_terminal_job_limit") or 0),
            "pruned_terminal_jobs": int(snapshot.get("pruned_terminal_jobs") or 0),
            "diagnosis": cls._public_diagnosis(snapshot.get("diagnosis") or {}),
        }
        if safe["available"] is None:
            safe.pop("available", None)
        if "state" in snapshot:
            safe["state"] = str(snapshot.get("state") or safe["diagnosis"].get("state") or "")
        return safe

    @staticmethod
    def _public_job_ref(job_ref: dict[str, Any]) -> dict[str, Any]:
        return {
            "room_id": str(job_ref.get("room_id") or ""),
            "priority": int(job_ref.get("priority") or 0),
            "status": str(job_ref.get("status") or ""),
            "current_stage": str(job_ref.get("current_stage") or ""),
            "created_at": float(job_ref.get("created_at") or 0.0),
            "updated_at": float(job_ref.get("updated_at") or 0.0),
        }

    @staticmethod
    def _public_event_ref(event: dict[str, Any]) -> dict[str, Any]:
        safe = {
            "event_type": str(event.get("event_type") or ""),
            "created_at": float(event.get("created_at") or 0.0),
        }
        for key in ("status", "current_stage", "priority", "cancelled_count", "pruned_count"):
            if key in event:
                safe[key] = event[key]
        return safe

    @staticmethod
    def _public_diagnosis(diagnosis: dict[str, Any]) -> dict[str, Any]:
        return {
            "state": str(diagnosis.get("state") or ""),
            "reasons": [str(item) for item in list(diagnosis.get("reasons") or [])[:8] if str(item)],
            "recommended_actions": [
                str(item)
                for item in list(diagnosis.get("recommended_actions") or [])[:8]
                if str(item)
            ],
            "queue_pressure": float(diagnosis.get("queue_pressure") or 0.0),
            "queued_count": int(diagnosis.get("queued_count") or 0),
            "active_count": int(diagnosis.get("active_count") or 0),
            "paused_session_count": int(diagnosis.get("paused_session_count") or 0),
            "latest_queue_full_at": float(diagnosis.get("latest_queue_full_at") or 0.0),
        }

    def _set_status_locked(
        self,
        job: GenerationJob,
        status: GenerationJobStatus,
        *,
        stage: str | None = None,
        error: str = "",
    ) -> None:
        previous_status = job.status
        previous_stage = job.current_stage
        job.status = status
        if stage is not None:
            job.current_stage = stage
        if error:
            job.error = error
        job.updated_at = time.time()
        if previous_status != job.status or previous_stage != job.current_stage:
            self._record_event_locked("status_change", **self._job_ref(job))
        if job.status in TERMINAL_STATUSES:
            self._prune_terminal_jobs_locked()

    def _record_event_locked(self, event_type: str, **payload: Any) -> None:
        event = {
            "event_type": str(event_type),
            "created_at": time.time(),
        }
        safe_keys = {
            "job_id",
            "session_id",
            "room_id",
            "plan_id",
            "batch_id",
            "priority",
            "status",
            "current_stage",
            "cancelled_count",
            "pruned_count",
        }
        for key in safe_keys:
            if key in payload:
                event[key] = payload[key]
        self._recent_events.append(event)
        if len(self._recent_events) > self._recent_event_limit:
            self._recent_events = self._recent_events[-self._recent_event_limit:]
        self._log_event(event)

    def _log_event(self, event: dict[str, Any]) -> None:
        event_type = str(event.get("event_type") or "")
        if not event_type:
            return
        self._logger.info(
            "[GenerationScheduler] event=%s room_id=%s session_id=%s plan_id=%s job_id=%s batch_id=%s "
            "status=%s stage=%s priority=%s cancelled_count=%s pruned_count=%s",
            event_type,
            event.get("room_id") or "",
            event.get("session_id") or "",
            event.get("plan_id") or "",
            event.get("job_id") or "",
            event.get("batch_id") or "",
            event.get("status") or "",
            event.get("current_stage") or "",
            event.get("priority") if "priority" in event else "",
            event.get("cancelled_count") if "cancelled_count" in event else "",
            event.get("pruned_count") if "pruned_count" in event else "",
        )

    def _prune_terminal_jobs_locked(self) -> None:
        limit = self._max_retained_terminal_jobs
        if limit <= 0:
            return
        terminal_jobs = sorted(
            (
                job
                for job in self._jobs.values()
                if job.status in TERMINAL_STATUSES
            ),
            key=lambda item: (item.sequence, str(item.job_id)),
        )
        overflow = len(terminal_jobs) - limit
        if overflow <= 0:
            return
        for job in terminal_jobs[:overflow]:
            self._jobs.pop(job.job_id, None)
            self._pending_job_ids.discard(job.job_id)
            self._pruned_terminal_jobs += 1
        self._record_event_locked("terminal_jobs_pruned", pruned_count=overflow)

    @staticmethod
    def _diagnose_snapshot_locked(snapshot: dict[str, Any]) -> dict[str, Any]:
        """Build a payload-safe resource diagnosis from a scheduler snapshot."""
        reasons: list[str] = []
        actions: list[str] = []
        recent_events = [
            event for event in snapshot.get("recent_events", [])
            if isinstance(event, dict)
        ]
        latest_queue_full_at = next(
            (
                float(event.get("created_at") or 0.0)
                for event in reversed(recent_events)
                if event.get("event_type") == "queue_full"
            ),
            0.0,
        )
        queue_pressure = float(snapshot.get("queue_pressure") or 0.0)
        queued_count = int(snapshot.get("queued_count") or 0)
        active_count = len(snapshot.get("active_jobs") or [])
        paused_sessions = list(snapshot.get("paused_sessions") or [])
        status_counts = snapshot.get("status_counts") or {}
        stage_counts = snapshot.get("stage_counts") or {}
        shutdown_requested = bool(snapshot.get("shutdown_requested"))

        if shutdown_requested:
            reasons.append("scheduler_stopped")
            actions.append("start_a_new_generation_session")
        if paused_sessions:
            reasons.append("paused_sessions")
            actions.append("resume_or_cancel_paused_sessions")
        if latest_queue_full_at:
            reasons.append("recent_queue_full")
            actions.append("wait_or_cancel_low_priority_jobs")
        if queue_pressure >= 1.0:
            reasons.append("queue_at_capacity")
            actions.append("defer_new_generation_until_queue_drains")
        elif queue_pressure >= 0.75:
            reasons.append("queue_near_capacity")
            actions.append("avoid_submitting_noncritical_batches")
        if int(stage_counts.get("import") or 0) > 0:
            reasons.append("import_stage_busy")
            actions.append("avoid_parallel_engine_writes")
        if int(status_counts.get(GenerationJobStatus.WAITING_USER.value) or 0) > 0:
            reasons.append("waiting_user_backpressure")
            actions.append("ask_user_to_wait_or_reduce_scope")

        if shutdown_requested:
            state = "stopped"
        elif paused_sessions:
            state = "paused"
        elif queue_pressure >= 1.0 or latest_queue_full_at:
            state = "saturated"
        elif queue_pressure >= 0.75:
            state = "strained"
        elif active_count > 0 or queued_count > 0:
            state = "active"
        else:
            state = "idle"

        deduped_actions = list(dict.fromkeys(actions))
        return {
            "state": state,
            "reasons": list(dict.fromkeys(reasons)),
            "recommended_actions": deduped_actions,
            "queue_pressure": queue_pressure,
            "queued_count": queued_count,
            "active_count": active_count,
            "paused_session_count": len(paused_sessions),
            "latest_queue_full_at": latest_queue_full_at,
        }

    @staticmethod
    def _job_ref(job: GenerationJob) -> dict[str, Any]:
        return {
            "job_id": job.job_id,
            "session_id": job.session_id,
            "room_id": job.room_id,
            "plan_id": job.plan_id,
            "batch_id": job.batch_id,
            "priority": job.priority,
            "sequence": job.sequence,
            "status": job.status.value,
            "current_stage": job.current_stage,
            "created_at": job.created_at,
            "updated_at": job.updated_at,
        }


__all__ = [
    "DEFAULT_STAGE_ORDER",
    "GenerationJob",
    "GenerationJobStatus",
    "GenerationScheduler",
]

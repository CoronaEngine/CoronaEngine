"""Execute saved project files in the host interpreter and own their stop/restore lifecycle."""
import hashlib
import importlib.util
import logging
from pathlib import Path
import sys
import threading
import time
import uuid
from typing import Any, Optional

from runtime.editor_host import set_editor_camera_input_enabled
from runtime.project_context import require_project_session
from script_runtime import file_api, scene_state
from script_runtime.file_api import FileRun

logger = logging.getLogger(__name__)


class FileScripts:
    _exec_thread: Optional[threading.Thread] = None
    _exec_context_id: Optional[str] = None
    # Receipts are owned by the serialized Python service dispatcher. Keep them
    # for this host lifetime, including after a project or script finishes.
    _file_requests: dict[str, tuple[tuple | None, str | None]] = {}
    _exec_lock = threading.RLock()
    _exec_state: dict[str, Any] = {
        "status": "idle",
        "outcome": "",
        "error": "",
        "contextId": "",
        "sceneName": "",
        "actorName": "",
        "targetType": "",
        "requestedScene": "",
        "requestedActor": "",
        "resolvedSceneName": "",
        "resolvedActorName": "",
        "bindingMode": "",
        "nativeScene": "",
        "actorCandidates": [],
        "marker": {},
        "waiting": {},
        "startedAt": 0.0,
        "finishedAt": 0.0,
        "inputLocked": False,
        "snapshotCaptured": False,
        "restoreStatus": "idle",
        "restoreError": "",
    }
    _exec_state_snapshot: Optional[dict[str, Any]] = None
    _exec_input_locked = False
    _run: FileRun | None = None
    _stopping = threading.RLock()
    _shutdown_requested = False

    @classmethod
    def _replace_exec_state(cls, **updates) -> dict:
        with cls._exec_lock:
            cls._exec_state.update(updates)
            return dict(cls._exec_state)

    @classmethod
    def _set_exec_input_locked(cls, locked: bool) -> None:
        locked = bool(locked)
        with cls._exec_lock:
            cls._exec_input_locked = locked
            cls._exec_state["inputLocked"] = locked
        try:
            set_editor_camera_input_enabled(not locked, reason="file_run")
        except Exception:
            logger.debug("[FileScripts] file editor camera input gate unavailable", exc_info=True)

    @classmethod
    def has_restore_snapshot(cls) -> bool:
        with cls._exec_lock:
            return cls._exec_state_snapshot is not None

    @classmethod
    def _restore_exec_snapshot(cls) -> tuple[bool, str | None]:
        with cls._exec_lock:
            snapshot = cls._exec_state_snapshot
            if snapshot is None:
                # Stopping an idle/already-restored world is a successful no-op.
                cls._exec_state.update(
                    snapshotCaptured=False, restoreStatus="idle", restoreError=""
                )
                return False, None
            cls._exec_state.update(restoreStatus="restoring", restoreError="")
        try:
            scene_state.restore(snapshot)
            with cls._exec_lock:
                cls._exec_state_snapshot = None
                cls._exec_state.update(
                    snapshotCaptured=False, restoreStatus="restored", restoreError=""
                )
            return True, None
        except Exception as exc:
            logger.exception("[FileScripts] file state restore failed")
            error = str(exc)
            cls._replace_exec_state(restoreStatus="error", restoreError=error)
            return False, error

    @classmethod
    def run_file(cls, request: dict) -> dict:
        return cls._run_file(request, verify_revision=True)

    @classmethod
    def run_path(cls, request: dict) -> dict:
        return cls._run_file(request, verify_revision="revision" in request)

    @classmethod
    def _run_file(cls, request: dict, *, verify_revision: bool) -> dict:
        """Compile the bytes read once from the active project's file."""
        request_id = request.get("requestId")
        if not isinstance(request_id, str) or not 0 < len(request_id) <= 128:
            return {"status": "error", "message": "A script requestId is required"}
        root, error = require_project_session(request)
        if error:
            return error
        identity = tuple(request.get(key) for key in (
            "archive_session", "project_path", "path", "revision", "target_type", "scene_name", "actor_name"))
        if request_id in cls._file_requests:
            previous, run_id = cls._file_requests[request_id]
            if run_id and previous != identity:
                return {"status": "error", "code": "REQUEST_CONFLICT", "message": "Script requestId was reused"}
            return cls.get_file_run(request)
        try:
            relative = Path(request["path"])
            if relative.anchor or relative.suffix != ".py":
                raise ValueError("Expected a project-relative .py file")
            path = (root / relative).resolve(strict=True)
            if not path.is_relative_to(root) or path.suffix != ".py":
                raise ValueError("Script must be a .py file in the active project")
            source = path.read_bytes()
            if verify_revision and hashlib.sha256(source).hexdigest() != request.get("revision"):
                return {"status": "error", "code": "SCRIPT_CHANGED", "message": "脚本已更改，请重新保存并运行"}
            # Compile the verified bytes; a concurrent save cannot change this run.
            program = compile(source, str(path), "exec")
        except (KeyError, TypeError, ValueError, OSError, SyntaxError) as exc:
            return {"status": "error", "message": str(exc)}
        if not cls._stopping.acquire(blocking=False):
            return {"status": "busy", "message": "File restoration in progress"}
        try:
            result = cls._start_single(request.get("scene_name", ""), request.get("actor_name", ""),
                                       request.get("target_type", "actor"), filepath=path, program=program,
                                       request_id=request_id, session=request["archive_session"])
        finally:
            cls._stopping.release()
        if result.get("runId"):
            cls._file_requests[request_id] = (identity, result["runId"])
            result["requestId"] = request_id
        return result

    @classmethod
    def get_file_run(cls, request: dict) -> dict:
        request_id = request.get("requestId")
        record = cls._file_requests.get(request_id)
        if record is None:
            return {"status": "unknown", "requestId": request_id}
        _, run_id = record
        if run_id is None:
            return {"status": "cancelled", "requestId": request_id}
        with cls._exec_lock:
            state = cls._exec_state if run_id == cls._exec_state.get("runId") else {}
            return {"status": state.get("status", "finished"), "runId": run_id,
                    "requestId": request_id, "snapshotCaptured": bool(state.get("snapshotCaptured"))}

    @classmethod
    def stop_file(cls, request: dict) -> dict:
        if not cls._stopping.acquire(blocking=False):
            return {"status": "busy", "message": "File restoration in progress"}
        try:
            return cls._stop_requested_run(request)
        finally:
            cls._stopping.release()

    @classmethod
    def _stop_requested_run(cls, request):
        run_id = request.get("runId")
        request_id = request.get("requestId")
        if request_id and not run_id:
            # A close can overtake a delayed start. Remember cancellation so a
            # late delivery or retry cannot launch a script for a closed panel.
            record = cls._file_requests.setdefault(request_id, (None, None))
            run_id = record[1]
            if not run_id or run_id != cls._exec_state.get("runId"):
                return {"status": "stopped", "requestId": request_id}
        if not run_id or run_id != cls._exec_state.get("runId"):
            return {"status": "error", "code": "STALE_RUN", "message": "脚本运行实例已更改"}
        return cls.stop_script_execution(request.get("restore_state", True))

    @classmethod
    def _start_single(cls, scene_name, actor_name, target_type, *, filepath, program, request_id, session) -> dict:
        if cls._shutdown_requested:
            return {"status": "error", "message": "File runtime is shutting down"}
        try:
            with cls._exec_lock:
                active_thread = bool(cls._exec_thread and cls._exec_thread.is_alive())
                if active_thread:
                    return {
                        "status": "error",
                        "message": "上一次脚本仍在结束中，请稍后重试",
                        "outcome": "execution_finishing",
                    }
                if cls._exec_state_snapshot is not None:
                    return {
                        "status": "error",
                        "message": "上一次脚本运行仍保留可恢复快照，请先停止并恢复或重新加载编辑器",
                        "outcome": "snapshot_pending",
                    }

            requested_scene = scene_name or ""
            requested_actor = actor_name or ""
            target_type = "actor" if str(target_type or "actor").lower() == "model" else str(target_type or "actor").lower()
            try:
                from api.editor_api import get_script_runtime_editor_api
                scene = get_script_runtime_editor_api().scene
                active = scene.read_active()
                if active.get("archive_session") != session:
                    raise RuntimeError("PROJECT_CONTEXT_CHANGED")
                if scene_name and scene_name != active["scene"]:
                    raise ValueError(f"Scene {scene_name} is not active")
                scene_name = active["scene"]
                if target_type == "actor":
                    actor_name = scene.read_actor(session, actor_name)["name"]
                elif target_type != "project":
                    raise ValueError(f"Invalid file target: {target_type}")
                state_snapshot = scene_state.capture(scene_name, session)
                snapshot_error = None
            except (RuntimeError, ValueError, KeyError) as exc:
                state_snapshot, snapshot_error = None, str(exc)
            if state_snapshot is None:
                message = f"无法创建运行前场景快照: {snapshot_error or '未知错误'}"
                with cls._exec_lock:
                    cls._exec_state_snapshot = None
                cls._set_exec_input_locked(False)
                cls._replace_exec_state(
                    status="error", outcome="snapshot_error", error=message,
                    inputLocked=False, snapshotCaptured=False,
                    restoreStatus="idle", restoreError="",
                    finishedAt=time.time(),
                )
                return {"status": "error", "message": message, "outcome": "snapshot_error"}
            with cls._exec_lock:
                cls._exec_state_snapshot = state_snapshot

            run_id = uuid.uuid4().hex
            context_id = run_id
            run = FileRun(run_id, session, target_type, scene_name, actor_name)
            cls._run = run
            started_at = time.time()
            cls._replace_exec_state(
                status="starting",
                outcome="",
                error="",
                contextId=context_id,
                runId=run_id,
                requestId=request_id,
                sceneName=scene_name,
                actorName=actor_name,
                targetType=target_type,
                requestedScene=requested_scene,
                requestedActor=requested_actor,
                resolvedSceneName=scene_name,
                resolvedActorName=actor_name,
                bindingMode="native_editor",
                nativeScene=scene_name,
                actorCandidates=[],
                marker={},
                waiting={},
                startedAt=started_at,
                finishedAt=0.0,
                inputLocked=True,
                snapshotCaptured=True,
                restoreStatus="idle",
                restoreError="",
            )
            cls._set_exec_input_locked(True)

            def _run_in_thread():
                final_status, outcome = cls._run_code_file(filepath, run, program)
                if run.restore_on_exit:
                    # Do not restore from the script thread: the stop coordinator
                    # must be free to wait for handlers before restoring the scene.
                    with cls._exec_lock:
                        cls._exec_state.update(
                            status="stopping",
                            outcome=outcome,
                            finishedAt=time.time(),
                        )
                        if cls._exec_thread is threading.current_thread():
                            cls._exec_thread = None
                    threading.Thread(
                        target=cls.stop_file,
                        args=({"runId": run.id},),
                        daemon=True,
                        name="file-stop-restore",
                    ).start()
                    return

                with cls._exec_lock:
                    current_status = cls._exec_state.get("status")
                    if current_status not in ("error", "stopped"):
                        cls._exec_state.update(
                            status=final_status,
                            outcome=outcome,
                            finishedAt=time.time(),
                        )
                    cls._set_exec_input_locked(False)
                    if cls._exec_thread is threading.current_thread():
                        cls._exec_thread = None

            with cls._exec_lock:
                cls._exec_context_id = context_id
                cls._exec_thread = threading.Thread(
                    target=_run_in_thread, daemon=True, name="file-exec"
                )
                cls._exec_thread.start()

            return {
                "status": "started",
                "filepath": str(filepath),
                "runId": run_id,
                "contextId": context_id,
                "sceneName": scene_name,
                "actorName": actor_name,
                "targetType": target_type,
                "requestedScene": requested_scene,
                "requestedActor": requested_actor,
                "resolvedSceneName": scene_name,
                "resolvedActorName": actor_name,
                "bindingMode": "native_editor",
            }
        except Exception as exc:
            cls._set_exec_input_locked(False)
            with cls._exec_lock:
                cls._exec_state_snapshot = None
            logger.exception("[FileScripts] file startup failed")
            cls._replace_exec_state(
                status="error", outcome="startup_error", error=str(exc), finishedAt=time.time()
            )
            return {"status": "error", "message": str(exc)}

    @classmethod
    def stop_script_execution(cls, restore_state: bool = False) -> dict:
        # A second stop (including project replacement) must not race restoration.
        if not cls._stopping.acquire(blocking=False):
            return {"status": "busy", "message": "File restoration in progress"}
        try:
            return cls._stop_and_restore(restore_state)
        finally:
            cls._stopping.release()

    @classmethod
    def _stop_and_restore(cls, restore_state):
        run = cls._run
        thread_to_stop = cls._exec_thread
        if run is not None:
            run.stop()
        if thread_to_stop and thread_to_stop.is_alive():
            thread_to_stop.join(timeout=0.5)
        thread_stopped = not (thread_to_stop and thread_to_stop.is_alive())
        child_threads = run.child_threads() if run else []
        pending_children = cls._wait_for_threads(child_threads, 0.75)
        if (not thread_stopped or pending_children) and run and run.on_quarantine:
            run.on_quarantine("File threads missed cooperative stop deadline")
        if not thread_stopped or pending_children:
            names = [thread_to_stop.name] if thread_to_stop and thread_to_stop.is_alive() else []
            names.extend(child.name for child in pending_children)
            message = "脚本未协作停止，已隔离且未恢复场景: " + ", ".join(sorted(set(names)))
            cls._set_exec_input_locked(False)
            with cls._exec_lock:
                cls._exec_state.update(
                    status="error",
                    outcome="stop_timeout",
                    error=message,
                    finishedAt=time.time(),
                    inputLocked=False,
                )
            return {
                "status": "error",
                "message": message,
                "restored": False,
                "pendingThreads": sorted(set(names)),
            }

        restored = False
        restore_error = None
        if bool(restore_state):
            restored, restore_error = cls._restore_exec_snapshot()
        else:
            with cls._exec_lock:
                cls._exec_state_snapshot = None
                cls._exec_state.update(snapshotCaptured=False, restoreStatus="idle", restoreError="")

        cls._set_exec_input_locked(False)
        with cls._exec_lock:
            cls._exec_thread = None
            cls._exec_context_id = None
            cls._run = None
            cls._exec_state.update(
                status="stopped",
                outcome="stopped",
                error="" if not restore_error else f"场景恢复失败: {restore_error}",
                finishedAt=time.time(),
                inputLocked=False,
            )
        result = {
            "status": "stopped",
            "restored": restored,
            "snapshotCaptured": cls._exec_state_snapshot is not None,
            "restoreStatus": cls._exec_state.get("restoreStatus", "idle"),
        }
        if restore_error:
            result["restoreError"] = restore_error
            result["message"] = f"已停止，但场景恢复失败: {restore_error}"
        return result

    @classmethod
    def request_shutdown(cls) -> None:
        cls._shutdown_requested = True
        if cls._run:
            cls._run.stop()

    @classmethod
    def shutdown(cls, timeout: float = 1.0) -> dict:
        cls.request_shutdown()
        run = cls._run
        threads = [cls._exec_thread]
        if run:
            threads.extend(run.child_threads())
        pending = cls._wait_for_threads(threads, timeout)
        if pending and run and run.on_quarantine:
            run.on_quarantine("Script shutdown timed out")
        cls._set_exec_input_locked(False)
        return {"service": "FileScripts", "state": "stop_timeout" if pending else "stopped",
                "thread_alive": bool(pending), "pending_threads": [t.name for t in pending]}

    @classmethod
    def snapshot(cls) -> dict:
        with cls._exec_lock:
            exec_thread = cls._exec_thread
        alive = [exec_thread.name] if exec_thread and exec_thread.is_alive() else []
        return {
            "service": "FileScripts",
            "state": "stop_requested" if cls._shutdown_requested else "ready",
            "thread_alive": bool(alive),
            "pending_threads": sorted(alive),
        }

    @classmethod
    def get_script_status(cls) -> dict:
        with cls._exec_lock:
            state = dict(cls._exec_state)
            state["inputLocked"] = bool(cls._exec_input_locked)
            state["snapshotCaptured"] = cls._exec_state_snapshot is not None
            context_id = cls._exec_context_id or state.get("contextId")
            thread_alive = bool(cls._exec_thread and cls._exec_thread.is_alive())
            if cls._exec_thread is not None and not thread_alive:
                cls._exec_thread = None
        state["threadAlive"] = thread_alive
        run = cls._run
        if run and run.id == context_id:
            state.update(marker={"id": run.marker[0], "name": run.marker[1]},
                         waiting={"id": run.waiting[0], "name": run.waiting[1]})
        status_name = str(state.get("status") or "idle")
        started_at = float(state.get("startedAt") or 0.0)
        starting_recently = (
            status_name == "starting"
            and started_at > 0.0
            and (time.time() - started_at) < 1.0
        )
        if thread_alive and status_name not in ("starting", "running"):
            state["status"] = "running"
        elif not thread_alive and not starting_recently:
            # A status string can survive a project switch or an interrupted worker.
            # Only a live worker may own the file camera input lock.
            cls._set_exec_input_locked(False)
            state["inputLocked"] = False
            if status_name in ("starting", "running", "stopping"):
                terminal_status = "stopped" if status_name == "stopping" else "completed"
                terminal_outcome = str(state.get("outcome") or terminal_status)
                with cls._exec_lock:
                    cls._exec_state.update(
                        status=terminal_status,
                        outcome=terminal_outcome,
                        finishedAt=float(state.get("finishedAt") or time.time()),
                        inputLocked=False,
                    )
                state.update(
                    status=terminal_status,
                    outcome=terminal_outcome,
                    finishedAt=float(state.get("finishedAt") or time.time()),
                )
        return state

    # ------------------------------------------------------------------
    # File execution helpers
    # ------------------------------------------------------------------
    @staticmethod
    def _wait_for_threads(threads: list[threading.Thread], timeout: float) -> list[threading.Thread]:
        """在统一截止时间内等待线程，避免按物体逐个累计超时。"""
        current = threading.current_thread()
        pending = [thread for thread in threads if thread and thread is not current and thread.is_alive()]
        deadline = time.monotonic() + max(0.0, float(timeout))
        while pending and time.monotonic() < deadline:
            for thread in list(pending):
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                thread.join(timeout=min(0.01, remaining))
            pending = [thread for thread in pending if thread.is_alive()]
        return pending

    @classmethod
    def _run_code_file(cls, code_path, ctx, program):
        module_name = "corona_file_" + ctx.id
        try:
            spec = importlib.util.spec_from_file_location(module_name, code_path)
            module = importlib.util.module_from_spec(spec)
            sys.modules[module_name] = module
            with file_api.bind(ctx):
                try:
                    file_api.check_stop()
                    exec(program, module.__dict__)
                    cls._replace_exec_state(status="running", outcome="", error="")
                    module.run()
                    while ctx.keep_alive():
                        file_api.wait(0.1)
                except SystemExit:
                    pass
                status = "stopped" if ctx.stopped.is_set() else "completed"
                return ctx.result() or (status, status)
        except Exception as exc:
            logger.exception("File execution failed: %s", code_path)
            cls._replace_exec_state(outcome="error", error=str(exc), finishedAt=time.time())
            return "error", "error"
        finally:
            sys.modules.pop(module_name, None)

    @classmethod
    def key_event(cls, key, modifiers="", display_key=""):
        run = cls._run
        if run:
            mods = [m.strip() for m in modifiers.split(",") if m.strip()] if modifiers else []
            run.send_input("key_down", key, mods, display_key or key)
        return {"status": "ok"}

    @classmethod
    def key_release(cls, key, display_key=""):
        run = cls._run
        if run:
            run.send_input("key_up", key, [], display_key or key)
        return {"status": "ok"}

    @classmethod
    def mouse_event(cls, *args):
        run = cls._run
        if run:
            run.send_input("mouse", *args)
        return {"status": "ok"}

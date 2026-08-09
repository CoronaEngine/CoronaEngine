import hashlib
import importlib.util
import json
import logging
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
from typing import Any, Optional

from api.editor_api import (
    emit_compat_editor_event,
    get_active_project_path,
    get_compat_editor_selection,
    set_compat_editor_camera_input_enabled,
)
from runtime.scene_support import (
    cancel_pending_auto_saves,
    flush_pending_auto_saves,
    get_project_scenes,
)
from config.paths_config import get_default_paths
from config.project_state import settings_manager

core_path = get_default_paths()

logger = logging.getLogger(__name__)


def _request_thread_stop(
    thread: threading.Thread,
    timeout: float = 3.0,
    context_id: str | None = None,
):
    """Cooperatively stop a script thread and isolate it when it misses the deadline."""
    from script_runtime.engine import corona_engine as corona_engine_scratch

    corona_engine_scratch.request_stop(context_id)
    thread.join(timeout=timeout)
    if not thread.is_alive():
        return True

    snapshot = corona_engine_scratch.runtime_context_snapshot(context_id) or {}
    reason = (
        f"thread={getattr(thread, 'name', 'scratch')} context={context_id or ''} "
        f"node={snapshot.get('current_node_id', '')}:{snapshot.get('current_node_name', '')} "
        f"missed cooperative stop deadline {timeout:.3f}s"
    )
    corona_engine_scratch.isolate_context(context_id, reason)
    logger.error("[ScratchTool] isolated non-cooperative script: %s", reason)
    return False


class ScratchTool:
    generated_script_dir = core_path.generated_script_dir
    os.makedirs(generated_script_dir, exist_ok=True)

    _exec_thread: Optional[threading.Thread] = None
    _exec_context_id: Optional[str] = None
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
        "pythonScenes": [],
        "nativeScene": "",
        "actorCandidates": [],
        "currentNodeId": "",
        "currentNodeName": "",
        "waitingEdgeId": "",
        "waitingEdgeName": "",
        "startedAt": 0.0,
        "finishedAt": 0.0,
        "inputLocked": False,
        "snapshotCaptured": False,
        "restoreStatus": "idle",
        "restoreError": "",
    }
    _exec_state_snapshot: Optional[dict[str, Any]] = None
    _exec_input_locked = False
    _persistence_lock = threading.RLock()
    _shutdown_requested = False

    _preview_lock = threading.RLock()
    _preview_threads: list[dict[str, Any]] = []
    _preview_status = "idle"
    _preview_errors: list[str] = []
    _preview_warnings: list[str] = []
    _preview_state_snapshot: Optional[dict[str, Any]] = None
    _preview_input_locked = False
    _preview_restart_generation = 0
    _preview_restart_in_progress = False
    _preview_restart_lock = threading.Lock()
    _preview_scope = "project"
    _preview_scene_name = ""
    _preview_results: dict[str, dict[str, Any]] = {}
    _preview_started_count = 0
    _preview_blockly_count = 0
    _preview_node_graph_count = 0
    _preview_stop_thread: Optional[threading.Thread] = None
    _preview_restore_status = "idle"
    _preview_restore_error = ""
    _preview_restored = False
    _preview_stopped_count = 0
    _preview_restore_pending = False
    _preview_restore_in_progress = False

    # ------------------------------------------------------------------
    # Project Blockly persistence
    # ------------------------------------------------------------------
    @classmethod
    def _active_project_path(cls) -> Path:
        # The native project launcher updates CoronaEditor.ini immediately, but
        # this long-lived Python settings singleton can still point at the
        # project that was active when the editor process started. Refresh the
        # launcher setting before every Blockly load/save/run operation.
        try:
            settings_manager.load()
            latest_project = settings_manager.config.get(
                "General", "last_project", fallback=""
            ).strip()
            if latest_project:
                latest_path = Path(latest_project).resolve()
                current = settings_manager.active_project_path
                current_path = Path(current).resolve() if current else None
                if (
                    (latest_path / "project.ini").is_file()
                    and current_path != latest_path
                ):
                    settings_manager.set_active_project(str(latest_path))
        except Exception:
            logger.exception(
                "[ScratchTool] failed to refresh active project "
                "from launcher settings"
            )

        project_path = settings_manager.active_project_path
        if not project_path:
            project_path = get_active_project_path()
        if not project_path:
            raise RuntimeError("No project is open; Blockly cannot be saved or run")
        return Path(project_path)

    @staticmethod
    def _project_identity(path: Path | str) -> str:
        """Return a stable, case-insensitive identity for a Windows project path."""
        return os.path.normcase(str(Path(path).expanduser().resolve()))

    @classmethod
    def _request_project_context(cls, data: dict) -> tuple[Optional[Path], Optional[dict]]:
        """Pin one request to the active project and reject stale frontend requests."""
        expected_raw = str(data.get("project_path") or "").strip()
        expected_path = Path(expected_raw).expanduser().resolve() if expected_raw else None

        active_raw = settings_manager.active_project_path
        if not active_raw:
            active_raw = get_active_project_path()

        if not active_raw:
            if expected_path is not None and settings_manager.set_active_project(str(expected_path)):
                return expected_path, None
            return None, {
                "status": "error",
                "code": "NO_ACTIVE_PROJECT",
                "message": "no active project is available for this Blockly request",
                "project_path": str(expected_path) if expected_path is not None else "",
            }

        project_path = Path(active_raw).expanduser().resolve()
        if expected_raw:
            if cls._project_identity(expected_path) != cls._project_identity(project_path):
                return project_path, {
                    "status": "error",
                    "code": "PROJECT_CONTEXT_CHANGED",
                    "message": "active project changed; stale Blockly request was ignored",
                    "project_path": str(project_path),
                }
        return project_path, None

    @classmethod
    def _blockly_dir(cls, project_path: Optional[Path] = None) -> Path:
        directory = (project_path or cls._active_project_path()) / "Scripts" / "blockly"
        directory.mkdir(parents=True, exist_ok=True)
        return directory

    @classmethod
    def _manifest_path(cls, project_path: Optional[Path] = None) -> Path:
        return cls._blockly_dir(project_path) / "manifest.json"

    @classmethod
    def _load_manifest(cls, project_path: Optional[Path] = None) -> dict:
        path = cls._manifest_path(project_path)
        if not path.exists():
            return {"targets": []}
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
            if not isinstance(data, dict):
                return {"targets": []}
            targets = data.get("targets")
            if not isinstance(targets, list):
                data["targets"] = []
            return data
        except Exception:
            logger.exception("[ScratchTool] failed to load manifest: %s", path)
            return {"targets": []}

    @staticmethod
    def _atomic_write_text(path: Path, content: str) -> None:
        """Atomically replace a project file so concurrent floating panels never see it half-written."""
        path.parent.mkdir(parents=True, exist_ok=True)
        temp_path: Optional[Path] = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                newline="",
                dir=path.parent,
                prefix=f".{path.name}.",
                suffix=".tmp",
                delete=False,
            ) as handle:
                temp_path = Path(handle.name)
                handle.write(content)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(temp_path, path)
            temp_path = None
        finally:
            if temp_path is not None:
                try:
                    temp_path.unlink(missing_ok=True)
                except OSError:
                    pass

    @classmethod
    def _atomic_write_json(cls, path: Path, payload: Any) -> None:
        cls._atomic_write_text(
            path,
            json.dumps(payload, ensure_ascii=False, indent=2),
        )

    @classmethod
    def _write_manifest(cls, manifest: dict, project_path: Optional[Path] = None) -> None:
        cls._atomic_write_json(cls._manifest_path(project_path), manifest)

    @staticmethod
    def _normalize_script_kind(script_kind: Any) -> str:
        return "node_graph" if str(script_kind or "blockly").strip().lower() == "node_graph" else "blockly"

    @classmethod
    def _target_id(
        cls,
        target_type: str,
        scene_name: str = "",
        actor_name: str = "",
        script_kind: str = "blockly",
    ) -> str:
        normalized_type = "actor" if target_type == "model" else target_type
        kind = cls._normalize_script_kind(script_kind)
        prefix = "node_graph:" if kind == "node_graph" else ""
        if normalized_type == "project":
            return f"{prefix}project:global"
        return f"{prefix}actor:{scene_name}:{actor_name}"

    @staticmethod
    def _target_digest(target_id: str) -> str:
        return hashlib.sha1(target_id.encode("utf-8")).hexdigest()[:12]

    @staticmethod
    def _normalize_payload(payload: Any) -> dict:
        if payload is None:
            return {}
        if isinstance(payload, str):
            return json.loads(payload) if payload.strip() else {}
        if isinstance(payload, dict):
            return payload
        raise TypeError("payload must be an object")

    @staticmethod
    def _with_context_prelude(code: str, target_type: str, scene_name: str, actor_name: str) -> str:
        target_type = "actor" if target_type == "model" else target_type
        prelude = "from script_runtime.engine import corona_engine as _CE\n"
        if target_type == "project":
            prelude += "_CE.set_project_global()\n"
        elif scene_name and actor_name:
            prelude += f"_CE.set_target({scene_name!r}, {actor_name!r})\n"
        return prelude + (code or "")

    @classmethod
    def save_blockly_target(cls, payload: dict | str) -> dict:
        """Save generated Python and workspace JSON for one Blockly or node-graph target."""
        try:
            data = cls._normalize_payload(payload)
            project_path, project_error = cls._request_project_context(data)
            if project_error:
                return project_error
            target_type = "actor" if data.get("target_type") == "model" else (data.get("target_type") or "actor")
            if target_type not in ("project", "actor"):
                return {"status": "error", "message": f"unsupported target_type: {target_type}"}

            script_kind = cls._normalize_script_kind(data.get("script_kind"))
            scene_name = str(data.get("scene_name") or "")
            actor_name = str(data.get("actor_name") or "")
            if target_type == "actor" and (not scene_name or not actor_name):
                return {"status": "error", "message": "actor target requires scene_name and actor_name"}
            if target_type == "project":
                scene_name = ""
                actor_name = ""

            target_id = cls._target_id(target_type, scene_name, actor_name, script_kind)
            digest = cls._target_digest(target_id)
            if script_kind == "node_graph":
                prefix = "node_graph_project" if target_type == "project" else "node_graph_actor"
            else:
                prefix = "project_global" if target_type == "project" else "actor"
            blockly_dir = cls._blockly_dir(project_path)
            code_path = blockly_dir / f"{prefix}_{digest}.py"
            workspace_path = blockly_dir / f"{prefix}_{digest}.blockly.json"

            code = cls._with_context_prelude(
                str(data.get("code") or ""), target_type, scene_name, actor_name
            )

            def _rel(path: Path) -> str:
                return path.relative_to(project_path).as_posix()

            validation_errors = data.get("validation_errors") or []
            if not isinstance(validation_errors, list):
                validation_errors = [str(validation_errors)]
            target = {
                "id": target_id,
                "target_type": target_type,
                "script_kind": script_kind,
                "scene_name": scene_name,
                "actor_name": actor_name,
                "code_path": _rel(code_path),
                "workspace_path": _rel(workspace_path),
                "updated_at": time.time(),
                "enabled": bool(data.get("enabled", True)),
                "runnable": bool(data.get("runnable", True)),
                "validation_errors": [str(item) for item in validation_errors if str(item)],
            }

            # NodeGraph can be opened in a detached CEF panel. Serialize the complete
            # workspace/code/manifest transaction and atomically replace every file so
            # another panel can never interpret a temporarily truncated manifest as an
            # empty world and overwrite the user's graph.
            with cls._persistence_lock:
                cls._atomic_write_text(code_path, code)
                cls._atomic_write_json(workspace_path, data.get("workspace") or {})
                manifest = cls._load_manifest(project_path)
                targets = [t for t in manifest.get("targets", []) if t.get("id") != target_id]
                targets.append(target)
                targets.sort(
                    key=lambda t: (
                        0 if t.get("target_type") == "project" else 1,
                        t.get("scene_name", ""),
                        t.get("actor_name", ""),
                        cls._normalize_script_kind(t.get("script_kind")),
                    )
                )
                manifest["targets"] = targets
                cls._write_manifest(manifest, project_path)
            return {"status": "saved", "target": target, "project_path": str(project_path)}
        except Exception as exc:
            logger.exception("[ScratchTool] save_blockly_target failed")
            return {"status": "error", "message": str(exc)}

    @classmethod
    def load_blockly_target(cls, payload: dict | str) -> dict:
        """Load a saved Blockly or node-graph workspace for one target."""
        try:
            data = cls._normalize_payload(payload)
            project_path, project_error = cls._request_project_context(data)
            if project_error:
                return project_error
            target_type = "actor" if data.get("target_type") == "model" else (data.get("target_type") or "actor")
            if target_type not in ("project", "actor"):
                return {"status": "error", "message": f"unsupported target_type: {target_type}"}
            script_kind = cls._normalize_script_kind(data.get("script_kind"))

            scene_name = str(data.get("scene_name") or "")
            actor_name = str(data.get("actor_name") or "")
            if target_type == "project":
                scene_name = ""
                actor_name = ""
            elif not scene_name or not actor_name:
                return {"status": "error", "message": "actor target requires scene_name and actor_name"}

            target_id = cls._target_id(target_type, scene_name, actor_name, script_kind)
            with cls._persistence_lock:
                manifest = cls._load_manifest(project_path)
                target = next((item for item in manifest.get("targets", []) if item.get("id") == target_id), None)
                if not target:
                    return {
                        "status": "missing",
                        "target": {
                            "id": target_id,
                            "target_type": target_type,
                            "script_kind": script_kind,
                            "scene_name": scene_name,
                            "actor_name": actor_name,
                        },
                        "workspace": {},
                        "project_path": str(project_path),
                    }

                workspace_rel = target.get("workspace_path") or ""
                workspace_path = (project_path / workspace_rel).resolve()
                try:
                    workspace_path.relative_to(project_path)
                except ValueError:
                    return {"status": "error", "message": "workspace_path escapes active project"}
                if not workspace_path.exists():
                    return {
                        "status": "missing",
                        "target": target,
                        "workspace": {},
                        "project_path": str(project_path),
                    }
                with open(workspace_path, "r", encoding="utf-8") as f:
                    workspace = json.load(f)
            if not isinstance(workspace, dict):
                workspace = {}
            return {
                "status": "loaded",
                "target": target,
                "workspace": workspace,
                "project_path": str(project_path),
            }
        except Exception as exc:
            logger.exception("[ScratchTool] load_blockly_target failed")
            return {"status": "error", "message": str(exc)}



    # ------------------------------------------------------------------
    # Legacy single-code execution
    # ------------------------------------------------------------------
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
            set_compat_editor_camera_input_enabled(not locked, reason="node_graph")
        except Exception:
            logger.debug("[ScratchTool] node graph editor camera input gate unavailable", exc_info=True)

    @classmethod
    def _capture_exec_snapshot(cls, resolved: dict[str, Any]) -> tuple[Optional[dict[str, Any]], str | None]:
        try:
            from script_runtime.engine import corona_engine as corona_engine_scratch
            snapshot = corona_engine_scratch.capture_runtime_scene_state(
                resolved.get("scene_name", ""),
                resolved.get("scene"),
                resolved.get("binding_mode", ""),
            )
            return snapshot, None
        except Exception as exc:
            logger.exception("[ScratchTool] node graph state snapshot failed")
            return None, str(exc)

    @classmethod
    def _restore_exec_snapshot(cls) -> tuple[bool, str | None]:
        with cls._exec_lock:
            snapshot = cls._exec_state_snapshot
            cls._exec_state.update(restoreStatus="restoring", restoreError="")
        if not snapshot:
            error = "\u6ca1\u6709\u53ef\u7528\u4e8e\u6062\u590d\u7684\u8282\u70b9\u56fe\u8fd0\u884c\u5feb\u7167"
            cls._replace_exec_state(restoreStatus="error", restoreError=error)
            return False, error
        try:
            cancel_pending_auto_saves()
            from script_runtime.engine import corona_engine as corona_engine_scratch
            corona_engine_scratch.restore_runtime_scene_state(snapshot)
            with cls._exec_lock:
                cls._exec_state_snapshot = None
                cls._exec_state.update(
                    snapshotCaptured=False, restoreStatus="restored", restoreError=""
                )
            return True, None
        except Exception as exc:
            logger.exception("[ScratchTool] node graph state restore failed")
            error = str(exc)
            cls._replace_exec_state(restoreStatus="error", restoreError=error)
            return False, error

    @classmethod
    def execute_python_code(
        cls,
        code: str,
        index: int,
        scene_name: str = "",
        actor_name: str = "",
        target_type: str = "actor",
    ) -> dict:
        if cls._shutdown_requested:
            return {"status": "error", "message": "Scratch runtime is shutting down"}
        try:
            from script_runtime.engine import corona_engine as corona_engine_scratch

            with cls._preview_lock:
                live_preview_threads = sum(
                    1 for info in cls._preview_threads
                    if info.get("thread") and info["thread"].is_alive()
                )
                preview_status = cls._preview_status
                preview_scope = cls._preview_scope
                preview_has_snapshot = cls._preview_state_snapshot is not None
                preview_active = bool(
                    preview_has_snapshot
                    or live_preview_threads
                    or preview_status in ("starting", "running", "stopping")
                )
            if preview_active:
                message = "全局运行或项目预览正在进行，请先停止并恢复后再运行单物体脚本"
                logger.warning(
                    "[ScratchTool] rejected single execution during preview: "
                    "status=%s scope=%s snapshot=%s live_threads=%d scene=%s actor=%s",
                    preview_status, preview_scope, preview_has_snapshot,
                    live_preview_threads, scene_name, actor_name,
                )
                return {
                    "status": "error",
                    "message": message,
                    "outcome": "preview_running",
                    "previewStatus": preview_status,
                    "previewScope": preview_scope,
                }

            requested_scene = scene_name or ""
            requested_actor = actor_name or ""
            target_type = "actor" if str(target_type or "actor").lower() == "model" else str(target_type or "actor").lower()
            resolved = corona_engine_scratch.resolve_runtime_target(target_type, scene_name, actor_name)
            if resolved.get("status") != "ok":
                message = str(resolved.get("message") or "\u65e0\u6cd5\u7ed1\u5b9a\u8fd0\u884c\u76ee\u6807")
                cls._replace_exec_state(
                    status="error",
                    outcome="binding_error",
                    error=message,
                    contextId="",
                    sceneName=scene_name or "",
                    actorName=actor_name or "",
                    targetType=target_type,
                    requestedScene=requested_scene,
                    requestedActor=requested_actor,
                    resolvedSceneName="",
                    resolvedActorName="",
                    bindingMode=resolved.get("binding_mode", ""),
                    pythonScenes=resolved.get("python_scenes", []),
                    nativeScene=resolved.get("native_scene", ""),
                    actorCandidates=resolved.get("actor_candidates", []),
                    currentNodeId="",
                    currentNodeName="",
                    waitingEdgeId="",
                    waitingEdgeName="",
                    startedAt=time.time(),
                    finishedAt=time.time(),
                )
                return {
                    "status": "error", "message": message, "outcome": "binding_error",
                    "requestedScene": requested_scene, "requestedActor": requested_actor,
                    "pythonScenes": resolved.get("python_scenes", []),
                    "nativeScene": resolved.get("native_scene", ""),
                    "actorCandidates": resolved.get("actor_candidates", []),
                    "bindingMode": resolved.get("binding_mode", ""),
                }

            target_type = resolved.get("target_type", target_type)
            scene_name = resolved.get("scene_name", scene_name)
            actor_name = resolved.get("actor_name", actor_name)

            with cls._exec_lock:
                active_thread = bool(cls._exec_thread and cls._exec_thread.is_alive())
                previous_status = str(cls._exec_state.get("status") or "idle")
                previous_outcome = str(cls._exec_state.get("outcome") or "")
                if active_thread:
                    return {
                        "status": "error",
                        "message": "\u4e0a\u4e00\u6b21\u8282\u70b9\u56fe\u811a\u672c\u4ecd\u5728\u7ed3\u675f\u4e2d\uff0c\u8bf7\u7a0d\u540e\u91cd\u8bd5",
                        "outcome": "execution_finishing",
                    }
                # A failed execution is terminal.  Its diagnostic remains visible in
                # _exec_state, but a stale pre-run snapshot must never make the Run
                # button unusable after the graph has been fixed.  Restore failures
                # are intentionally retained so the user can retry restoration.
                retryable_terminal = previous_status in ("error", "completed") or previous_outcome in (
                    "error", "startup_error", "binding_error", "snapshot_error"
                )
                restore_failed = str(cls._exec_state.get("restoreStatus") or "") == "error"
                if retryable_terminal and not restore_failed:
                    cls._exec_state_snapshot = None
                    cls._exec_thread = None
                    cls._exec_context_id = None
                    cls._exec_state.update(snapshotCaptured=False, inputLocked=False)
                    cls._set_exec_input_locked(False)
                if cls._exec_state_snapshot is not None:
                    return {
                        "status": "error",
                        "message": "\u4e0a\u4e00\u6b21\u8282\u70b9\u56fe\u8fd0\u884c\u4ecd\u4fdd\u7559\u53ef\u6062\u590d\u5feb\u7167\uff0c\u8bf7\u5148\u505c\u6b62\u5e76\u6062\u590d\u6216\u91cd\u65b0\u52a0\u8f7d\u7f16\u8f91\u5668",
                        "outcome": "snapshot_pending",
                    }
            state_snapshot, snapshot_error = cls._capture_exec_snapshot(resolved)
            if state_snapshot is None:
                message = f"\u65e0\u6cd5\u521b\u5efa\u8fd0\u884c\u524d\u573a\u666f\u5feb\u7167: {snapshot_error or '\u672a\u77e5\u9519\u8bef'}"
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

            filename = f"blockly_code{'_' + str(index) if index else ''}.py"
            filepath = cls.generated_script_dir / filename

            for old_file in os.listdir(cls.generated_script_dir):
                if old_file.startswith("blockly_code") and old_file.endswith(".py"):
                    try:
                        os.remove(cls.generated_script_dir / old_file)
                    except Exception:
                        pass

            final_code = cls._with_context_prelude(code, target_type, scene_name, actor_name)
            with open(filepath, "w", encoding="utf-8") as f:
                f.write(final_code)

            old_thread = None
            old_context = None
            with cls._exec_lock:
                if cls._exec_thread and cls._exec_thread.is_alive():
                    old_thread = cls._exec_thread
                    old_context = cls._exec_context_id
            if old_thread is not None:
                if not _request_thread_stop(old_thread, timeout=0.5, context_id=old_context):
                    raise RuntimeError("previous Blockly script did not stop cooperatively")

            context_id = cls._target_id(target_type, scene_name, actor_name)
            started_at = time.time()
            cls._replace_exec_state(
                status="starting",
                outcome="",
                error="",
                contextId=context_id,
                sceneName=scene_name,
                actorName=actor_name,
                targetType=target_type,
                requestedScene=requested_scene,
                requestedActor=requested_actor,
                resolvedSceneName=scene_name,
                resolvedActorName=actor_name,
                bindingMode=resolved.get("binding_mode", ""),
                pythonScenes=resolved.get("python_scenes", []),
                nativeScene=resolved.get("native_scene", ""),
                actorCandidates=resolved.get("actor_candidates", []),
                currentNodeId="",
                currentNodeName="",
                waitingEdgeId="",
                waitingEdgeName="",
                startedAt=started_at,
                finishedAt=0.0,
                inputLocked=True,
                snapshotCaptured=True,
                restoreStatus="idle",
                restoreError="",
            )
            cls._set_exec_input_locked(True)

            def _run_in_thread():
                outcome = cls._run_code_file(
                    filepath,
                    {
                        "id": context_id,
                        "target_type": target_type,
                        "scene_name": scene_name,
                        "actor_name": actor_name,
                        "_resolved": resolved,
                    },
                    single_exec=True,
                )
                if outcome == "stop_restore":
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
                        target=cls.stop_script_execution,
                        args=(True,),
                        daemon=True,
                        name="blockly-exec-stop-restore",
                    ).start()
                    return

                with cls._exec_lock:
                    current_status = cls._exec_state.get("status")
                    if current_status not in ("error", "stopped"):
                        final_status = "completed" if outcome == "completed" else "stopped" if outcome in ("stopped", "restart") else "error"
                        cls._exec_state.update(
                            status=final_status,
                            outcome=outcome,
                            finishedAt=time.time(),
                        )
                    if outcome in ("completed", "error"):
                        cls._exec_state_snapshot = None
                        cls._exec_state["snapshotCaptured"] = False
                    cls._set_exec_input_locked(False)
                    if cls._exec_thread is threading.current_thread():
                        cls._exec_thread = None
                        cls._exec_context_id = None

            with cls._exec_lock:
                cls._exec_context_id = context_id
                cls._exec_thread = threading.Thread(
                    target=_run_in_thread, daemon=True, name="blockly-exec"
                )
                cls._exec_thread.start()

            return {
                "status": "started",
                "filepath": str(filepath),
                "contextId": context_id,
                "sceneName": scene_name,
                "actorName": actor_name,
                "targetType": target_type,
                "requestedScene": requested_scene,
                "requestedActor": requested_actor,
                "resolvedSceneName": scene_name,
                "resolvedActorName": actor_name,
                "bindingMode": resolved.get("binding_mode", ""),
            }
        except Exception as exc:
            cls._set_exec_input_locked(False)
            with cls._exec_lock:
                cls._exec_state_snapshot = None
            logger.exception("[ScratchTool] execute_python_code failed")
            cls._replace_exec_state(
                status="error", outcome="startup_error", error=str(exc), finishedAt=time.time()
            )
            return {"status": "error", "message": str(exc)}

    @classmethod
    def stop_script_execution(cls, restore_state: bool = False) -> dict:
        from script_runtime.engine import corona_engine as corona_engine_scratch

        thread_to_stop = None
        context_id = None
        with cls._exec_lock:
            context_id = cls._exec_context_id
            if cls._exec_thread and cls._exec_thread.is_alive():
                thread_to_stop = cls._exec_thread

        if thread_to_stop is not None:
            thread_stopped = _request_thread_stop(
                thread_to_stop, timeout=0.5, context_id=context_id
            )
        elif context_id:
            corona_engine_scratch.request_stop(context_id)
            thread_stopped = True
        else:
            thread_stopped = True

        child_threads = (
            corona_engine_scratch.active_child_threads({context_id}) if context_id else []
        )
        pending_children = cls._wait_for_threads(child_threads, 0.75)
        if pending_children:
            corona_engine_scratch.isolate_context(
                context_id,
                "child script threads missed cooperative stop deadline: "
                + ", ".join(sorted({child.name for child in pending_children})),
            )

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
            cls._exec_state.update(
                status="stopped",
                outcome="stopped",
                error="" if not restore_error else f"\u573a\u666f\u6062\u590d\u5931\u8d25: {restore_error}",
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
            result["message"] = f"\u5df2\u505c\u6b62\uff0c\u4f46\u573a\u666f\u6062\u590d\u5931\u8d25: {restore_error}"
        return result

    @classmethod
    def request_shutdown(cls) -> None:
        from script_runtime.engine import corona_engine as corona_engine_scratch

        cls._shutdown_requested = True
        corona_engine_scratch.request_stop_all()

    @classmethod
    def shutdown(cls, timeout: float = 1.0) -> dict:
        """Bounded service shutdown; never restores state while workers remain alive."""
        from script_runtime.engine import corona_engine as corona_engine_scratch

        cls.request_shutdown()
        with cls._exec_lock:
            exec_thread = cls._exec_thread
            exec_context = cls._exec_context_id
        with cls._preview_lock:
            preview_infos = list(cls._preview_threads)
            preview_contexts = {
                str((info.get("target") or {}).get("id") or "")
                for info in preview_infos
                if (info.get("target") or {}).get("id")
            }
        context_ids = {value for value in preview_contexts | {exec_context or ""} if value}
        threads = [exec_thread] if exec_thread else []
        threads.extend(info.get("thread") for info in preview_infos if info.get("thread"))
        threads.extend(corona_engine_scratch.active_child_threads(context_ids or None))
        pending = cls._wait_for_threads(
            list({thread for thread in threads if thread}),
            max(0.0, float(timeout)),
        )
        if pending:
            reason = "Scratch service shutdown timeout: " + ", ".join(
                sorted({thread.name for thread in pending})
            )
            for context_id in context_ids:
                corona_engine_scratch.isolate_context(context_id, reason)
            cls._set_exec_input_locked(False)
            cls._set_preview_input_locked(False)
            return {
                "service": "ScratchTool",
                "state": "stop_timeout",
                "thread_alive": True,
                "pending_threads": sorted({thread.name for thread in pending}),
            }
        cls._set_exec_input_locked(False)
        cls._set_preview_input_locked(False)
        return {
            "service": "ScratchTool",
            "state": "stopped",
            "thread_alive": False,
            "pending_threads": [],
        }

    @classmethod
    def snapshot(cls) -> dict:
        with cls._exec_lock:
            exec_thread = cls._exec_thread
        with cls._preview_lock:
            preview_threads = [
                info.get("thread") for info in cls._preview_threads if info.get("thread")
            ]
        alive = [
            thread.name for thread in [exec_thread, *preview_threads]
            if thread and thread.is_alive()
        ]
        return {
            "service": "ScratchTool",
            "state": "stop_requested" if cls._shutdown_requested else "ready",
            "thread_alive": bool(alive),
            "pending_threads": sorted(alive),
        }

    @classmethod
    def get_script_status(cls) -> dict:
        from script_runtime.engine import corona_engine as corona_engine_scratch

        with cls._exec_lock:
            state = dict(cls._exec_state)
            state["inputLocked"] = bool(cls._exec_input_locked)
            state["snapshotCaptured"] = cls._exec_state_snapshot is not None
            context_id = cls._exec_context_id or state.get("contextId")
            thread_alive = bool(cls._exec_thread and cls._exec_thread.is_alive())
            if cls._exec_thread is not None and not thread_alive:
                cls._exec_thread = None
        state["threadAlive"] = thread_alive
        snapshot = corona_engine_scratch.runtime_context_snapshot(context_id) if context_id else None
        if snapshot:
            state.update(
                currentNodeId=snapshot.get("current_node_id", ""),
                currentNodeName=snapshot.get("current_node_name", ""),
                waitingEdgeId=snapshot.get("waiting_edge_id", ""),
                waitingEdgeName=snapshot.get("waiting_edge_name", ""),
            )
            binding_error = snapshot.get("binding_error", "")
            if binding_error and not state.get("error"):
                state["error"] = binding_error
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
            # Only a live worker may own the node-graph camera input lock.
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
    # Project preview
    # ------------------------------------------------------------------
    @classmethod
    def _set_preview_input_locked(cls, locked: bool) -> None:
        with cls._preview_lock:
            cls._preview_input_locked = bool(locked)
        try:
            # CoronaEditor owns the reason-based lock set and propagates the aggregate
            # state to the native camera-follow controller.  Do not call the native
            # setter directly here, otherwise another active lock could be released.
            set_compat_editor_camera_input_enabled(not locked, reason="game_preview")
        except Exception:
            logger.debug("[ScratchTool] editor camera input gate unavailable", exc_info=True)

    @staticmethod
    def _scene_names_match(left: Any, right: Any) -> bool:
        def tokens(value: Any) -> set[str]:
            text = str(value or "").strip().replace("\\", "/").casefold()
            if not text:
                return set()
            name = text.rsplit("/", 1)[-1]
            stem = name.rsplit(".", 1)[0]
            return {text, name, stem}
        return bool(tokens(left) & tokens(right))

    @classmethod
    def _preview_payload_locked(cls) -> dict[str, Any]:
        targets = [dict(item) for item in cls._preview_results.values()]
        targets.sort(key=lambda item: (item.get("scene_name", ""), item.get("actor_name", ""), item.get("script_kind", "")))
        running_count = sum(1 for item in targets if item.get("status") in ("starting", "running"))
        completed_count = sum(1 for item in targets if item.get("status") == "completed")
        error_count = sum(1 for item in targets if item.get("status") == "error")
        payload = {
            "status": cls._preview_status,
            "scope": cls._preview_scope,
            "scene_name": cls._preview_scene_name,
            "started_count": cls._preview_started_count,
            "running_count": running_count,
            "completed_count": completed_count,
            "error_count": error_count,
            "blockly_count": cls._preview_blockly_count,
            "node_graph_count": cls._preview_node_graph_count,
            "targets": targets,
            "errors": list(cls._preview_errors),
            "warnings": list(cls._preview_warnings),
            "input_locked": cls._preview_input_locked,
            "has_snapshot": cls._preview_state_snapshot is not None,
            "restart_generation": cls._preview_restart_generation,
            "restore_status": cls._preview_restore_status,
            "restore_error": cls._preview_restore_error,
            "restored": cls._preview_restored,
            "stopped_count": cls._preview_stopped_count,
            "restore_pending": cls._preview_restore_pending,
            "stop_pending": bool(
                (cls._preview_stop_thread and cls._preview_stop_thread.is_alive())
                or cls._preview_restore_pending
                or cls._preview_restore_in_progress
            ),
        }
        payload.update({
            "sceneName": payload["scene_name"],
            "startedCount": payload["started_count"],
            "runningCount": payload["running_count"],
            "completedCount": payload["completed_count"],
            "errorCount": payload["error_count"],
            "blocklyCount": payload["blockly_count"],
            "nodeGraphCount": payload["node_graph_count"],
            "inputLocked": payload["input_locked"],
            "hasSnapshot": payload["has_snapshot"],
        })
        return payload

    @classmethod
    def start_game_preview(cls, payload: dict | str | None = None) -> dict:
        if cls._shutdown_requested:
            return {"status": "error", "message": "Scratch runtime is shutting down"}
        data = cls._normalize_payload(payload)
        scope = str(data.get("scope") or "project").strip().lower()
        requested_scene = str(data.get("scene_name") or data.get("sceneName") or "").strip()
        if scope not in ("project", "scene"):
            return {"status": "error", "message": f"unsupported preview scope: {scope}"}
        if scope == "scene" and not requested_scene:
            return {"status": "error", "message": "scene preview requires scene_name"}

        logger.info(
            "[ScratchTool] preview start requested: scope=%s scene=%s",
            scope, requested_scene,
        )
        with cls._exec_lock:
            single_active = bool(
                (cls._exec_thread and cls._exec_thread.is_alive())
                or cls._exec_state_snapshot is not None
                or cls._exec_state.get("status") in ("starting", "running")
            )
        if single_active:
            return {
                "status": "error",
                "message": "单物体脚本正在运行，请先停止后再启动全局运行",
                "outcome": "single_script_running",
            }
        with cls._preview_lock:
            preview_active = bool(
                cls._preview_state_snapshot is not None
                or any(info.get("thread") and info["thread"].is_alive() for info in cls._preview_threads)
                or cls._preview_status in ("starting", "running", "stopping")
            )
        if preview_active:
            return {"status": "error", "message": "已有预览或全局运行正在进行，请先停止并恢复"}

        try:
            project_path = cls._active_project_path().resolve()
        except Exception as exc:
            logger.exception("[ScratchTool] preview project resolution failed")
            return {"status": "error", "message": str(exc)}

        from script_runtime.engine import corona_engine as corona_engine_scratch
        corona_engine_scratch.clear_runtime_state_snapshots()
        try:
            flush_pending_auto_saves()
            targets, warnings = cls._prepare_preview_targets(
                scope, requested_scene, project_path
            )
            flush_pending_auto_saves()
        except Exception as exc:
            logger.exception("[ScratchTool] start_game_preview prepare failed")
            return {"status": "error", "message": str(exc)}

        with cls._preview_lock:
            cls._preview_scope = scope
            cls._preview_scene_name = requested_scene if scope == "scene" else ""
            cls._preview_errors = []
            cls._preview_warnings = list(warnings)
            cls._preview_threads = []
            cls._preview_started_count = len(targets)
            cls._preview_blockly_count = sum(1 for item in targets if cls._normalize_script_kind(item.get("script_kind")) == "blockly")
            cls._preview_node_graph_count = sum(1 for item in targets if cls._normalize_script_kind(item.get("script_kind")) == "node_graph")
            cls._preview_results = {}
            for target in targets:
                cls._preview_results[target["id"]] = {
                    "id": target["id"],
                    "target_type": target.get("target_type", "actor"),
                    "script_kind": cls._normalize_script_kind(target.get("script_kind")),
                    "scene_name": target.get("scene_name", ""),
                    "actor_name": target.get("actor_name", ""),
                    "requested_scene_name": target.get("requested_scene_name", target.get("scene_name", "")),
                    "requested_actor_name": target.get("requested_actor_name", target.get("actor_name", "")),
                    "binding_mode": target.get("binding_mode", ""),
                    "status": "starting",
                    "outcome": "",
                    "error": "",
                    "started_at": 0.0,
                    "finished_at": 0.0,
                }
            cls._preview_status = "starting" if targets else "idle"
            cls._preview_state_snapshot = None
            cls._preview_restart_generation = 0
            cls._preview_restart_in_progress = False
            cls._preview_stop_thread = None
            cls._preview_restore_status = "idle"
            cls._preview_restore_error = ""
            cls._preview_restored = False
            cls._preview_stopped_count = 0
            cls._preview_restore_pending = False
            cls._preview_restore_in_progress = False

        if not targets:
            cls._set_preview_input_locked(False)
            target_label = "项目全局节点图" if scope == "project" else "当前场景脚本"
            message = f"没有可运行的{target_label}"
            if warnings:
                message += f"：{warnings[0]}"
            logger.warning(
                "[ScratchTool] preview start rejected: scope=%s scene=%s reason=%s",
                scope, requested_scene, message,
            )
            with cls._preview_lock:
                cls._preview_status = "error"
                cls._preview_errors = [message]
                payload = cls._preview_payload_locked()
                payload["message"] = message
                payload["outcome"] = "no_runnable_targets"
                return payload

        try:
            state_snapshot = cls._create_scoped_preview_snapshot(
                scope, targets, project_path
            )
        except Exception as exc:
            logger.exception("[ScratchTool] preview state snapshot failed")
            with cls._preview_lock:
                cls._preview_status = "error"
                cls._preview_errors.append(f"创建预览状态快照失败: {exc}")
                return {**cls._preview_payload_locked(), "message": f"创建预览状态快照失败: {exc}"}

        with cls._preview_lock:
            cls._preview_state_snapshot = state_snapshot
            cls._preview_status = "running"
        cls._set_preview_input_locked(True)

        pending_threads = []
        for target in targets:
            code_path = project_path / target["code_path"]
            thread = threading.Thread(
                target=cls._run_preview_target,
                args=(code_path, target),
                daemon=True,
                name=f"blockly-preview-{cls._target_digest(target['id'])}",
            )
            pending_threads.append({"thread": thread, "target": target})
        with cls._preview_lock:
            cls._preview_threads = list(pending_threads)
            for info in pending_threads:
                info["thread"].start()
            return cls._preview_payload_locked()

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
    def _finalize_game_preview_stop(cls, infos: list[dict[str, Any]]) -> None:
        """后台停止所有预览线程，确认结束后再恢复场景。"""
        from script_runtime.engine import corona_engine as corona_engine_scratch

        with cls._preview_lock:
            # A target's main thread may have completed and removed itself from
            # _preview_threads while a clone/broadcast child thread is still alive.
            # Keep every started preview context in the stop set, otherwise that
            # orphan child can continue changing actors while restoration runs.
            context_ids = {str(value) for value in cls._preview_results if str(value)}
        context_ids.update({
            str((info.get("target") or {}).get("id") or "")
            for info in infos
            if (info.get("target") or {}).get("id")
        })
        main_threads = [info.get("thread") for info in infos if info.get("thread")]
        try:
            corona_engine_scratch.request_stop_all()
            child_threads = corona_engine_scratch.active_child_threads(context_ids)
            pending = cls._wait_for_threads(main_threads + child_threads, 1.25)

            # 广播/克隆处理器可能在主线程退出期间新建，再抓取一次。
            child_threads = corona_engine_scratch.active_child_threads(context_ids)
            pending = list({thread for thread in pending + child_threads if thread and thread.is_alive()})
            if pending:
                for context_id in context_ids:
                    corona_engine_scratch.isolate_context(
                        context_id,
                        "preview threads missed cooperative stop deadline: "
                        + ", ".join(sorted({thread.name for thread in pending})),
                    )

            if pending:
                names = ", ".join(sorted({thread.name for thread in pending}))
                message = f"仍有 {len(pending)} 个脚本线程未停止: {names}"
                with cls._preview_lock:
                    cls._preview_threads = [
                        info for info in cls._preview_threads
                        if info.get("thread") and info["thread"].is_alive()
                    ]
                    cls._preview_status = "error"
                    cls._preview_restore_status = "error"
                    cls._preview_restore_error = message
                    if message not in cls._preview_errors:
                        cls._preview_errors.append(message)
                logger.error("[ScratchTool] %s", message)
                return

            with cls._preview_lock:
                cls._preview_threads = []
                cls._preview_stopped_count = len(infos)
                # Native Editor APIs are safest when invoked from the bridge/status
                # request thread. The background coordinator only terminates script
                # threads; the next status poll performs restoration synchronously.
                cls._preview_restore_pending = True
                cls._preview_restore_status = "pending"
                cls._preview_status = "stopping"
        except Exception as exc:
            logger.exception("[ScratchTool] stop_game_preview finalize failed")
            with cls._preview_lock:
                cls._preview_status = "error"
                cls._preview_restore_status = "error"
                cls._preview_restore_error = str(exc)
                message = f"停止并恢复失败: {exc}"
                if message not in cls._preview_errors:
                    cls._preview_errors.append(message)
        finally:
            with cls._preview_lock:
                cls._preview_stop_thread = None
                keep_locked = bool(
                    cls._preview_state_snapshot is not None
                    and (
                        cls._preview_restore_pending
                        or cls._preview_restore_in_progress
                        or (
                            cls._preview_restore_status == "error"
                            and any(info.get("thread") and info["thread"].is_alive() for info in cls._preview_threads)
                        )
                    )
                )
            if not keep_locked:
                cls._set_preview_input_locked(False)

    @classmethod
    def stop_game_preview(cls) -> dict:
        from script_runtime.engine import corona_engine as corona_engine_scratch

        with cls._preview_lock:
            cls._prune_preview_locked()
            existing_stopper = cls._preview_stop_thread
            if existing_stopper and existing_stopper.is_alive():
                cls._preview_status = "stopping"
                return cls._preview_payload_locked()
            if cls._preview_restore_pending or cls._preview_restore_in_progress:
                cls._preview_status = "stopping"
                return cls._preview_payload_locked()

            infos = list(cls._preview_threads)
            has_snapshot = cls._preview_state_snapshot is not None
            context_ids = {str(value) for value in cls._preview_results if str(value)}
            context_ids.update({
                str((info.get("target") or {}).get("id") or "")
                for info in infos
                if (info.get("target") or {}).get("id")
            })
            child_threads = corona_engine_scratch.active_child_threads(context_ids)
            if not infos and not child_threads and not has_snapshot:
                cls._preview_status = "stopped"
                cls._preview_restore_status = "restored"
                cls._preview_restore_error = ""
                cls._preview_restored = False
                cls._set_preview_input_locked(False)
                return cls._preview_payload_locked()

            cls._preview_status = "stopping"
            cls._preview_restore_status = "stopping"
            cls._preview_restore_error = ""
            cls._preview_restored = False
            cls._preview_restore_pending = False
            stopper = threading.Thread(
                target=cls._finalize_game_preview_stop,
                args=(infos,),
                daemon=True,
                name="blockly-preview-stop",
            )
            cls._preview_stop_thread = stopper

        corona_engine_scratch.request_stop_all()
        stopper.start()
        with cls._preview_lock:
            return cls._preview_payload_locked()

    @classmethod
    def get_game_preview_status(cls) -> dict:
        from script_runtime.engine import corona_engine as corona_engine_scratch
        with cls._preview_lock:
            cls._prune_preview_locked()
            should_restore = cls._preview_restore_pending and not cls._preview_restore_in_progress
        if should_restore:
            cls._complete_preview_restore()
        with cls._preview_lock:
            live_threads = any(
                info.get("thread") and info["thread"].is_alive()
                for info in cls._preview_threads
            )
            stop_thread_alive = bool(
                cls._preview_stop_thread and cls._preview_stop_thread.is_alive()
            )
            preview_work_active = bool(
                live_threads
                or stop_thread_alive
                or cls._preview_restore_pending
                or cls._preview_restore_in_progress
            )
        if not preview_work_active:
            # Terminal/idle previews must never keep the editor camera disabled.
            # Call even when the Python flag is already false to re-sync native state.
            cls._set_preview_input_locked(False)
        with cls._preview_lock:
            status = cls._preview_payload_locked()
        status["worker_active"] = preview_work_active
        status["workerActive"] = preview_work_active
        status["runtime_states"] = corona_engine_scratch.runtime_state_snapshots()
        return status

    @classmethod
    def _complete_preview_restore(cls) -> None:
        """Restore the scene on the bridge/status thread after scripts stop."""
        from script_runtime.engine import corona_engine as corona_engine_scratch

        with cls._preview_lock:
            if not cls._preview_restore_pending or cls._preview_restore_in_progress:
                return
            cls._preview_restore_pending = False
            cls._preview_restore_in_progress = True
            cls._preview_restore_status = "restoring"
            cls._preview_status = "stopping"

        restored = False
        restore_error: str | None = None
        try:
            restored, restore_error = cls._restore_preview_state_snapshot()
            if not restored and not restore_error:
                restore_error = "The runtime scene snapshot was lost; restoration cannot continue"
            if restored and not restore_error:
                corona_engine_scratch.clear_runtime_state_snapshots()
        except Exception as exc:
            logger.exception("[ScratchTool] preview restore completion failed")
            restore_error = str(exc)
        finally:
            with cls._preview_lock:
                cls._preview_restore_in_progress = False
                cls._preview_restored = bool(restored and not restore_error)
                cls._preview_restore_error = restore_error or ""
                cls._preview_restore_status = "error" if restore_error else "restored"
                cls._preview_status = "error" if restore_error else "stopped"
                if restore_error:
                    message = f"场景恢复失败: {restore_error}"
                    if message not in cls._preview_errors:
                        cls._preview_errors.append(message)
            # Never leave camera input locked after all script threads have stopped.
            # The snapshot is retained on failure so the user can retry restoration.
            cls._set_preview_input_locked(False)

    @classmethod
    def _prepare_preview_targets(
        cls,
        scope: str = "project",
        requested_scene: str = "",
        project_path: Optional[Path] = None,
    ) -> tuple[list[dict], list[str]]:
        project_path = (project_path or cls._active_project_path()).resolve()
        manifest = cls._load_manifest(project_path)
        targets: list[dict] = []
        warnings: list[str] = []
        project_graph_found = False
        from script_runtime.engine import corona_engine as corona_engine_scratch

        for raw in manifest.get("targets", []):
            target = dict(raw)
            target["script_kind"] = cls._normalize_script_kind(
                target.get("script_kind")
            )
            target_type = (
                "actor"
                if target.get("target_type") == "model"
                else target.get("target_type")
            )
            target["target_type"] = target_type

            if scope == "project":
                is_global_project_graph = (
                    target.get("id") == "node_graph:project:global"
                    and target_type == "project"
                    and target["script_kind"] == "node_graph"
                )
                if not is_global_project_graph:
                    continue
                project_graph_found = True
            else:
                if target_type != "actor":
                    continue
                if not cls._scene_names_match(
                    target.get("scene_name", ""), requested_scene
                ):
                    continue

            if not target.get("enabled", True):
                warnings.append(
                    f"项目全局节点图已禁用，已跳过: {target.get('id')}"
                )
                continue
            if not target.get("runnable", True):
                errors = target.get("validation_errors") or ["脚本配置无效"]
                warnings.append(
                    f"节点图不可运行，已跳过: {target.get('id')} "
                    f"({'; '.join(map(str, errors))})"
                )
                continue
            code_path = target.get("code_path")
            if not code_path or not (project_path / code_path).exists():
                warnings.append(f"节点图生成代码不存在，已跳过: {target.get('id')}")
                continue

            if scope == "project":
                targets.append(target)
                continue
            if target_type != "actor":
                warnings.append(f"未知节点图目标，已跳过: {target.get('id')}")
                continue

            requested_target_scene = str(target.get("scene_name") or "")
            requested_actor = str(target.get("actor_name") or "")
            resolved = corona_engine_scratch.resolve_runtime_target(
                "actor", requested_target_scene, requested_actor
            )
            if resolved.get("status") != "ok":
                warnings.append(
                    f"目标绑定失败，已跳过: {requested_target_scene}/{requested_actor} "
                    f"({resolved.get('message') or '未知错误'})"
                )
                continue
            if not cls._scene_names_match(
                resolved.get("scene_name", ""), requested_scene
            ):
                warnings.append(
                    f"目标绑定到了其他场景，已跳过: {requested_target_scene}/{requested_actor} "
                    f"-> {resolved.get('scene_name', '')}"
                )
                continue
            target.update({
                "requested_scene_name": requested_target_scene,
                "requested_actor_name": requested_actor,
                "scene_name": resolved.get("scene_name", requested_target_scene),
                "actor_name": resolved.get("actor_name", requested_actor),
                "binding_mode": resolved.get("binding_mode", ""),
                "_resolved": resolved,
            })
            targets.append(target)

        if scope == "project" and not project_graph_found:
            warnings.append('当前项目还没有已保存的全局节点图')

        targets.sort(key=lambda item: (
            0 if item.get("target_type") == "project" else 1,
            item.get("scene_name", ""),
            item.get("actor_name", ""),
            item.get("script_kind", ""),
        ))
        return targets, warnings

    @classmethod
    def _create_scoped_preview_snapshot(
        cls,
        scope: str,
        targets: list[dict],
        project_path: Optional[Path] = None,
    ) -> dict[str, Any]:
        from script_runtime.engine import corona_engine as corona_engine_scratch
        snapshots: list[dict[str, Any]] = []
        seen: set[tuple[str, str]] = set()
        for target in targets:
            if target.get("target_type") != "actor":
                continue
            resolved = target.get("_resolved") or {}
            key = (str(resolved.get("binding_mode") or ""), str(resolved.get("scene_name") or target.get("scene_name") or ""))
            if key in seen:
                continue
            seen.add(key)
            snapshots.append(corona_engine_scratch.capture_runtime_scene_state(
                scene_name=key[1],
                scene=resolved.get("scene"),
                binding_mode=key[0],
            ))
        snapshot: dict[str, Any] = {"kind": "runtime", "scope": scope, "snapshots": snapshots}
        if scope == "project" and any(item.get("target_type") == "project" for item in targets):
            snapshot["legacy"] = cls._create_preview_state_snapshot(project_path)
        return snapshot

    @classmethod
    def _create_preview_state_snapshot(
        cls, project_path: Optional[Path] = None
    ) -> dict[str, Any]:
        from script_runtime.compat.legacy_scene_adapter import get_or_create_scene

        snapshot: dict[str, Any] = {"scenes": {}}
        for route in cls._project_scene_routes(project_path):
            scene = get_or_create_scene(route)
            scene_state: dict[str, Any] = {
                "actors": {},
                "cameras": {},
                "environment": {},
                "enabled": cls._safe_call(scene, "is_enabled"),
                "simulation_enabled": cls._safe_call(scene, "is_simulation_enabled"),
            }

            env = scene.get_environment() if hasattr(scene, "get_environment") else None
            if env is not None:
                scene_state["environment"] = {
                    "sun_direction": cls._safe_call(env, "get_sun_direction"),
                    "floor_grid": cls._safe_call(env, "get_floor_grid"),
                    "gravity": cls._safe_call(env, "get_gravity"),
                    "floor_y": cls._safe_call(env, "get_floor_y"),
                    "floor_restitution": cls._safe_call(env, "get_floor_restitution"),
                    "fixed_dt": cls._safe_call(env, "get_fixed_dt"),
                }

            for camera in scene.get_cameras():
                camera_name = getattr(camera, "name", "")
                if not camera_name:
                    continue
                scene_state["cameras"][camera_name] = {
                    "position": cls._safe_call(camera, "get_position"),
                    "forward": cls._safe_call(camera, "get_forward"),
                    "world_up": cls._safe_call(camera, "get_world_up"),
                    "fov": cls._safe_call(camera, "get_fov"),
                    "output_mode": cls._safe_call(camera, "get_output_mode"),
                    "width": getattr(camera, "width", None),
                    "height": getattr(camera, "height", None),
                }

            for actor in scene.get_actors():
                actor_name = getattr(actor, "name", "")
                if not actor_name:
                    continue
                scene_state["actors"][actor_name] = {
                    "position": cls._safe_call(actor, "get_position"),
                    "rotation": cls._safe_call(actor, "get_rotation"),
                    "scale": cls._safe_call(actor, "get_scale"),
                    "visible": cls._safe_call(actor, "get_visible"),
                    "mass": cls._safe_call(actor, "get_mass"),
                    "restitution": cls._safe_call(actor, "get_restitution"),
                    "damping": cls._safe_call(actor, "get_damping"),
                    "physics_enabled": cls._safe_call(actor, "get_physics_enabled"),
                    "collision_enabled": cls._safe_call(actor, "get_collision_enabled"),
                }

            snapshot["scenes"][route] = scene_state

        logger.info("[ScratchTool] preview state snapshot captured: %d scenes", len(snapshot["scenes"]))
        return snapshot

    @classmethod
    def _restore_preview_state_snapshot(
        cls, clear_snapshot: bool = True
    ) -> tuple[bool, str | None]:
        with cls._preview_lock:
            snapshot = cls._preview_state_snapshot
        if not snapshot:
            return False, None

        try:
            cancel_pending_auto_saves()
            restored_scenes: set[str] = set()
            if snapshot.get("kind") == "runtime":
                from script_runtime.engine import corona_engine as corona_engine_scratch
                for runtime_snapshot in snapshot.get("snapshots") or []:
                    corona_engine_scratch.restore_runtime_scene_state(runtime_snapshot)
                    restored_scenes.add(str(runtime_snapshot.get("scene_name") or ""))
                legacy = snapshot.get("legacy")
            else:
                legacy = snapshot

            if legacy:
                from script_runtime.compat.legacy_scene_adapter import get_scene
                for route, scene_state in (legacy.get("scenes") or {}).items():
                    scene = get_scene(route)
                    if scene is None:
                        continue
                    cls._restore_scene_state(scene, scene_state)
                    restored_scenes.add(route)
                    try:
                        scene.save_data()
                    except Exception:
                        logger.exception("[ScratchTool] failed to save restored scene: %s", route)

            cls._notify_preview_state_restored({item for item in restored_scenes if item})
            if clear_snapshot:
                with cls._preview_lock:
                    cls._preview_state_snapshot = None
            logger.info("[ScratchTool] preview state restored: %d scenes", len(restored_scenes))
            return True, None
        except Exception as exc:
            logger.exception("[ScratchTool] preview state restore failed")
            return False, str(exc)

    @staticmethod
    def _safe_call(target: object, method_name: str):
        if target is None or not hasattr(target, method_name):
            return None
        try:
            return getattr(target, method_name)()
        except Exception:
            return None

    @staticmethod
    def _apply_if_present(target: object, method_name: str, value, *extra_args) -> None:
        if value is None or target is None or not hasattr(target, method_name):
            return
        try:
            getattr(target, method_name)(value, *extra_args)
        except TypeError:
            getattr(target, method_name)(value)
        except Exception:
            logger.exception("[ScratchTool] restore %s failed", method_name)

    @classmethod
    def _restore_scene_state(cls, scene: object, scene_state: dict[str, Any]) -> None:
        cls._apply_if_present(scene, "set_enabled", scene_state.get("enabled"))
        cls._apply_if_present(scene, "set_simulation_enabled", scene_state.get("simulation_enabled"))

        env = scene.get_environment() if hasattr(scene, "get_environment") else None
        env_state = scene_state.get("environment") or {}
        if env is not None:
            cls._apply_if_present(env, "set_sun_direction", env_state.get("sun_direction"))
            cls._apply_if_present(env, "set_floor_grid", env_state.get("floor_grid"))
            cls._apply_if_present(env, "set_gravity", env_state.get("gravity"))
            cls._apply_if_present(env, "set_floor_y", env_state.get("floor_y"))
            cls._apply_if_present(env, "set_floor_restitution", env_state.get("floor_restitution"))
            cls._apply_if_present(env, "set_fixed_dt", env_state.get("fixed_dt"))

        for camera_name, camera_state in (scene_state.get("cameras") or {}).items():
            camera = scene.find_camera(camera_name) if hasattr(scene, "find_camera") else None
            if camera is None:
                continue
            position = camera_state.get("position")
            forward = camera_state.get("forward")
            world_up = camera_state.get("world_up")
            fov = camera_state.get("fov")
            if position is not None and forward is not None and world_up is not None and fov is not None:
                cls._apply_if_present(camera, "set", position, forward, world_up, fov)
            output_mode = camera_state.get("output_mode")
            cls._apply_if_present(camera, "set_output_mode", output_mode)
            width = camera_state.get("width")
            height = camera_state.get("height")
            if width is not None and height is not None:
                cls._apply_if_present(camera, "set_size", int(width), int(height))

        for actor_name, actor_state in (scene_state.get("actors") or {}).items():
            actor = scene.find_actor(actor_name) if hasattr(scene, "find_actor") else None
            if actor is None:
                continue
            cls._apply_if_present(actor, "set_position", actor_state.get("position"), True)
            cls._apply_if_present(actor, "set_rotation", actor_state.get("rotation"), True)
            cls._apply_if_present(actor, "set_scale", actor_state.get("scale"), True)
            cls._apply_if_present(actor, "set_visible", actor_state.get("visible"))
            cls._apply_if_present(actor, "set_mass", actor_state.get("mass"))
            cls._apply_if_present(actor, "set_restitution", actor_state.get("restitution"))
            cls._apply_if_present(actor, "set_damping", actor_state.get("damping"))
            cls._apply_if_present(actor, "set_physics_enabled", actor_state.get("physics_enabled"))
            cls._apply_if_present(actor, "set_collision_enabled", actor_state.get("collision_enabled"))

    @staticmethod
    def _notify_preview_state_restored(scene_routes: set[str]) -> None:
        try:
            selected_scene, selected_actor = get_compat_editor_selection()
            for route in scene_routes or {selected_scene or ""}:
                emit_compat_editor_event("scene-tree-changed", [route])
            if selected_scene and selected_actor:
                emit_compat_editor_event("actor-change", ["actor", selected_scene, selected_actor])
            elif selected_scene:
                emit_compat_editor_event("actor-change", ["scene", selected_scene, ""])
        except Exception:
            logger.exception("[ScratchTool] failed to notify frontend after preview state restore")

    @classmethod
    def _project_scene_routes(
        cls, project_path: Optional[Path] = None
    ) -> list[str]:
        project_path = (project_path or cls._active_project_path()).resolve()
        ini_path = project_path / "project.ini"
        scenes = get_project_scenes(str(ini_path)) if ini_path.exists() else []
        if not scenes and (project_path / "Scene").is_dir():
            scenes = [
                f"Scene/{path.name}"
                for path in sorted((project_path / "Scene").iterdir())
                if path.suffix == ".scene"
            ]
        entrance = ""
        try:
            if settings_manager.active_project_config:
                entrance = settings_manager.active_project_config["Project"].get("entrance_scene", "")
        except Exception:
            entrance = ""
        if entrance and entrance not in scenes:
            scenes.insert(0, entrance)
        return scenes

    @classmethod
    def _run_preview_target(cls, code_path: Path, target: dict) -> None:
        context_id = target.get("id", "")
        current_thread = threading.current_thread()
        with cls._preview_lock:
            observed_generation = cls._preview_restart_generation
            result = cls._preview_results.get(context_id)
            if result is not None:
                result.update(status="running", started_at=time.time())

        outcome = "completed"
        try:
            while True:
                outcome = cls._run_code_file(code_path, target, single_exec=False)
                if outcome in ("stop_restore", "game_end"):
                    # Reuse the exact global stop button flow. Game-over/win exits
                    # must also restore the editor scene automatically; otherwise the
                    # pre-run snapshot remains active and the viewport stays in-game.
                    cls.stop_game_preview()
                    break
                if outcome == "restart":
                    with cls._preview_restart_lock:
                        with cls._preview_lock:
                            if cls._preview_restart_generation == observed_generation:
                                cls._preview_restart_in_progress = True
                                cls._preview_restart_generation += 1
                                should_restore = True
                            else:
                                should_restore = False
                        if should_restore:
                            from script_runtime.engine import corona_engine as corona_engine_scratch
                            corona_engine_scratch.request_stop_all()
                            restored, error = cls._restore_preview_state_snapshot(clear_snapshot=False)
                            if error:
                                with cls._preview_lock:
                                    cls._preview_errors.append(f"restart: {error}")
                            elif not restored:
                                logger.warning("[ScratchTool] restart requested without preview snapshot")
                            with cls._preview_lock:
                                cls._preview_restart_in_progress = False
                    with cls._preview_lock:
                        observed_generation = cls._preview_restart_generation
                    continue

                while True:
                    with cls._preview_lock:
                        generation = cls._preview_restart_generation
                        restarting = cls._preview_restart_in_progress
                        status = cls._preview_status
                    if restarting:
                        time.sleep(0.02)
                        continue
                    break
                if generation != observed_generation and status in ("running", "starting"):
                    observed_generation = generation
                    continue
                break
        finally:
            should_unlock = False
            with cls._preview_lock:
                result = cls._preview_results.get(context_id)
                if result is not None:
                    if outcome == "error":
                        result["status"] = "error"
                    elif outcome in ("stopped", "restart", "stop_restore", "game_end"):
                        result["status"] = "stopped"
                    else:
                        result["status"] = "completed"
                    result["outcome"] = outcome
                    result["finished_at"] = time.time()
                cls._preview_threads = [
                    info for info in cls._preview_threads
                    if info.get("thread") is not current_thread
                    and info.get("thread")
                    and info["thread"].is_alive()
                ]
                if not cls._preview_threads and cls._preview_status in ("starting", "running"):
                    # Keep the pre-run snapshot until explicit restore succeeds.
                    # Clearing it when scripts finish makes a later stop unable to restore.
                    # The stop action remains available while the snapshot exists.
                    cls._preview_status = "completed"
                    should_unlock = True
            if should_unlock:
                from script_runtime.engine import corona_engine as corona_engine_scratch
                corona_engine_scratch.clear_runtime_state_snapshots()
                cls._set_preview_input_locked(False)

    @classmethod
    def _run_code_file(cls, code_path: Path, target: dict, single_exec: bool) -> str:
        from script_runtime.engine import corona_engine as corona_engine_scratch

        context_id = target.get("id") or cls._target_id(
            target.get("target_type", "actor"),
            target.get("scene_name", ""),
            target.get("actor_name", ""),
        )
        ctx = corona_engine_scratch.create_context(
            context_id=context_id,
            target_type=target.get("target_type", "actor"),
            scene_name=target.get("scene_name", ""),
            actor_name=target.get("actor_name", ""),
        )
        resolved = target.get("_resolved") if isinstance(target.get("_resolved"), dict) else {}
        if ctx.target_type == "project" and resolved.get("scene") is not None:
            # Keep project-global semantics while attaching the active scene so
            # explicit object blocks can resolve native editor actors by name.
            ctx.scene = resolved.get("scene")
            ctx.target_scene = ctx.scene
            ctx.scene_name = str(resolved.get("scene_name") or ctx.scene_name or "")
            ctx.target_scene_name = ctx.scene_name
        corona_engine_scratch.bind_context(ctx)

        module_name = f"blockly_runtime_{cls._target_digest(context_id)}_{int(time.time() * 1000)}"
        outcome = "completed"
        try:
            spec = importlib.util.spec_from_file_location(module_name, code_path)
            if spec is None or spec.loader is None:
                raise RuntimeError(f"无法加载积木脚本: {code_path}")
            module = importlib.util.module_from_spec(spec)
            sys.modules[module_name] = module
            spec.loader.exec_module(module)
            if single_exec:
                cls._replace_exec_state(status="running", outcome="", error="")
            if hasattr(module, "run"):
                module.run()
            if corona_engine_scratch.has_runtime_handlers(ctx) and not ctx.stop_requested:
                while not ctx.stop_requested:
                    corona_engine_scratch.wait(0.1)
            requested_state = ctx.game_state or ctx.variables.get("game_state")
            if requested_state == "stop_restore":
                outcome = "stop_restore"
                logger.info("[ScratchTool] stop-and-restore requested: %s", context_id)
            elif requested_state == "restart":
                outcome = "restart"
                logger.info("[ScratchTool] level restart requested: %s", context_id)
            elif requested_state in ("win", "over"):
                outcome = "game_end"
                logger.info("[ScratchTool] game ended (%s): %s", requested_state, context_id)
            elif ctx.stop_requested:
                outcome = "stopped"
                logger.info("[ScratchTool] script stopped: %s", context_id)
        except SystemExit:
            requested_state = ctx.game_state or ctx.variables.get("game_state")
            if requested_state == "restart":
                outcome = "restart"
                logger.info("[ScratchTool] level restart requested: %s", context_id)
            elif requested_state == "stop_restore":
                outcome = "stop_restore"
                logger.info("[ScratchTool] stop-and-restore requested: %s", context_id)
            elif requested_state in ("win", "over"):
                outcome = "game_end"
                logger.info("[ScratchTool] game ended (%s): %s", requested_state, context_id)
            else:
                outcome = "stopped"
                logger.info("[ScratchTool] script stopped: %s", context_id)
        except Exception as exc:
            outcome = "error"
            logger.exception("[ScratchTool] script failed: %s", context_id)
            if single_exec:
                # Keep the public status running until the wrapper has released the
                # runtime context, snapshot and input lock.  Otherwise the frontend
                # can offer Run again while the failed worker is still cleaning up.
                cls._replace_exec_state(
                    outcome="error",
                    error=str(exc),
                    finishedAt=time.time(),
                )
            else:
                with cls._preview_lock:
                    message = f"{context_id}: {exc}"
                    cls._preview_errors.append(message)
                    result = cls._preview_results.get(context_id)
                    if result is not None:
                        result.update(status="error", outcome="error", error=str(exc), finished_at=time.time())
        finally:
            snapshot = corona_engine_scratch.runtime_state_snapshot(ctx)
            if single_exec:
                cls._replace_exec_state(
                    currentNodeId=snapshot.get("current_node_id", ""),
                    currentNodeName=snapshot.get("current_node_name", ""),
                    waitingEdgeId=snapshot.get("waiting_edge_id", ""),
                    waitingEdgeName=snapshot.get("waiting_edge_name", ""),
                )
            sys.modules.pop(module_name, None)
            corona_engine_scratch.release_context(ctx)
        return outcome

    @classmethod
    def _prune_preview_locked(cls) -> None:
        cls._preview_threads = [
            info for info in cls._preview_threads
            if info.get("thread") and info["thread"].is_alive()
        ]
        if not cls._preview_threads and cls._preview_status in ("starting", "running"):
            # Polling may converge thread state but must not destroy the restore snapshot.
            # _restore_preview_state_snapshot() clears it only after successful restoration.
            cls._preview_status = "completed"
            cls._set_preview_input_locked(False)

    # ------------------------------------------------------------------
    # Input bridge
    # ------------------------------------------------------------------
    @classmethod
    def key_event(cls, key: str, modifiers: str = "", display_key: str = "") -> dict:
        from script_runtime.engine import corona_engine as corona_engine_scratch

        mods = [m.strip() for m in modifiers.split(",") if m.strip()] if modifiers else []
        corona_engine_scratch.handle_key_event(key, mods, display_key or key)
        return {"status": "ok"}

    @classmethod
    def key_release(cls, key: str, display_key: str = "") -> dict:
        from script_runtime.engine import corona_engine as corona_engine_scratch

        corona_engine_scratch.handle_key_release(key, display_key or key)
        return {"status": "ok"}

    @classmethod
    def mouse_event(
        cls,
        event_type: str,
        button: str = "",
        x: float = 0.0,
        y: float = 0.0,
        viewport_x: float | None = None,
        viewport_y: float | None = None,
        viewport_width: float | None = None,
        viewport_height: float | None = None,
        picked_actor: str = "",
    ) -> dict:
        from script_runtime.engine import corona_engine as corona_engine_scratch

        corona_engine_scratch.handle_mouse_event(
            event_type, button, x, y,
            viewport_x, viewport_y, viewport_width, viewport_height,
            picked_actor,
        )
        return {"status": "ok"}

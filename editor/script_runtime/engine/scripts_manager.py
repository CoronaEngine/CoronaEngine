from __future__ import annotations

import importlib.util
import logging
import os
from pathlib import Path
import sys
import threading
import time
from typing import Dict, Optional

from .entities.actor_script import ActorScript
from .entities.project_script import ProjectScript
from .entities.scene_script import SceneScript
from .entities.camera_locked_object import CameraLockedObject
from .contracts import SceneScriptTarget


class ScriptsManager:
    """Own project, scene and actor script instances and their reload lifecycle."""

    _instances: list = []

    def __init__(self):
        self.project_script: Optional[ProjectScript] = None
        self.current_scene_script: Optional[SceneScript] = None
        self.actor_scripts: Dict[str, ActorScript] = {}
        self.logger = logging.getLogger("ScriptManager")
        self.state = "created"
        self.shutdown_requested = False
        self._lifecycle_lock = threading.RLock()
        self._reload_in_progress = False
        self._script_records: list[dict] = []
        ScriptsManager._instances.append(self)

    @staticmethod
    def _mtime_ns(path: str) -> int:
        try:
            return os.stat(path).st_mtime_ns
        except OSError:
            return 0

    @staticmethod
    def _resolve_path(path: str, scene: SceneScriptTarget) -> str:
        if not path or os.path.isabs(path):
            return path
        project_dir = os.path.dirname(os.path.dirname(scene.route))
        return os.path.join(project_dir, path)

    def _load_instance(self, record: dict):
        path = record["path"]
        module_name = f"{record['module_name']}_reload_{time.monotonic_ns()}"
        spec = importlib.util.spec_from_file_location(module_name, path)
        if spec is None or spec.loader is None:
            raise RuntimeError(f"cannot load script module: {path}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        try:
            source = Path(path).read_bytes()
            exec(compile(source, path, "exec"), module.__dict__)
            base_class = record["base_class"]
            script_class = next(
                (
                    value for value in vars(module).values()
                    if isinstance(value, type)
                    and issubclass(value, base_class)
                    and value is not base_class
                ),
                None,
            )
            if script_class is None:
                raise RuntimeError(f"no {base_class.__name__} subclass in {path}")
            instance = script_class(record["instance_name"], *record["constructor_args"])
            instance.initialize()
            instance.is_initialized = True
            instance.on_start()
            instance._corona_script_module_name = module_name
            return instance
        except Exception:
            sys.modules.pop(module_name, None)
            raise

    def _add_record(
        self,
        *,
        kind: str,
        path: str,
        module_name: str,
        base_class,
        instance_name: str,
        constructor_args=(),
        actor_name: str = "",
    ):
        record = {
            "kind": kind,
            "path": path,
            "module_name": module_name,
            "base_class": base_class,
            "instance_name": instance_name,
            "constructor_args": tuple(constructor_args),
            "actor_name": actor_name,
            "mtime_ns": self._mtime_ns(path),
            "failed_mtime_ns": 0,
            "instance": None,
        }
        instance = self._load_instance(record)
        record["instance"] = instance
        self._script_records.append(record)
        self._assign_record_instance(record, instance)

    def _assign_record_instance(self, record: dict, instance) -> None:
        if record["kind"] == "project":
            self.project_script = instance
        elif record["kind"] == "scene":
            self.current_scene_script = instance
        else:
            self.actor_scripts[record["actor_name"]] = instance
            actor = record["constructor_args"][0]
            if isinstance(instance, CameraLockedObject) and hasattr(actor, "set_camera_locked_script"):
                actor.set_camera_locked_script(instance)

    def initialize_project(self, project_script_path: str, scene: SceneScriptTarget) -> bool:
        with self._lifecycle_lock:
            if self.shutdown_requested or self.state == "stopped":
                return False
            if self.state in ("initializing", "ready", "updating"):
                return self.state == "ready"
            self.state = "initializing"
            try:
                if project_script_path and os.path.exists(project_script_path):
                    self._add_record(
                        kind="project",
                        path=project_script_path,
                        module_name=f"project_script_module_{Path(project_script_path).stem}",
                        base_class=ProjectScript,
                        instance_name=f"Project_{Path(project_script_path).stem}",
                    )

                scene_script_path = self._resolve_path(getattr(scene, "script_path", ""), scene)
                if scene_script_path and os.path.exists(scene_script_path):
                    self._add_record(
                        kind="scene",
                        path=scene_script_path,
                        module_name=f"scene_script_module_{Path(scene_script_path).stem}",
                        base_class=SceneScript,
                        instance_name=f"Scene_{scene.name}",
                        constructor_args=(scene,),
                    )

                for actor in scene._actors:
                    actor_script_path = self._resolve_path(getattr(actor, "script_path", ""), scene)
                    if not actor_script_path or not os.path.exists(actor_script_path):
                        continue
                    self._add_record(
                        kind="actor",
                        path=actor_script_path,
                        module_name=f"actor_script_module_{Path(actor_script_path).stem}_{actor.name}",
                        base_class=ActorScript,
                        instance_name=f"Actor_{actor.name}",
                        constructor_args=(actor,),
                        actor_name=actor.name,
                    )
                self.state = "ready"
                self.logger.info("Script system initialized successfully")
                return True
            except Exception:
                self.logger.exception("Failed to initialize script system")
                self._shutdown_initialized_scripts()
                self.state = "stopped"
                return False

    @staticmethod
    def _service_initialization_active() -> bool:
        try:
            from runtime import registry
            return any(getattr(service, "state", "") == "initializing" for service in registry._managed_services)
        except Exception:
            return False

    def reload_changed_scripts(self) -> bool:
        with self._lifecycle_lock:
            if (
                self.shutdown_requested
                or self.state not in ("ready", "updating")
                or self._reload_in_progress
                or self._service_initialization_active()
            ):
                return False
            changed = [
                record for record in self._script_records
                if self._mtime_ns(record["path"]) > record["mtime_ns"]
                and self._mtime_ns(record["path"]) != record["failed_mtime_ns"]
            ]
            if not changed:
                return False
            self._reload_in_progress = True
            replaced = False
            try:
                for record in changed:
                    if self.shutdown_requested:
                        break
                    mtime_ns = self._mtime_ns(record["path"])
                    try:
                        candidate = self._load_instance(record)
                    except Exception:
                        record["failed_mtime_ns"] = mtime_ns
                        self.logger.exception("Script reload failed; keeping old instance: %s", record["path"])
                        continue
                    if self.shutdown_requested:
                        candidate.shutdown()
                        sys.modules.pop(
                            getattr(candidate, "_corona_script_module_name", ""), None
                        )
                        break
                    previous = record["instance"]
                    try:
                        previous.shutdown()
                    except Exception:
                        self.logger.exception("Old script cleanup failed during reload: %s", record["path"])
                    sys.modules.pop(getattr(previous, "_corona_script_module_name", ""), None)
                    record["instance"] = candidate
                    record["mtime_ns"] = mtime_ns
                    record["failed_mtime_ns"] = 0
                    self._assign_record_instance(record, candidate)
                    replaced = True
                    self.logger.info("Reloaded managed script: %s", record["path"])
                return replaced
            finally:
                self._reload_in_progress = False

    def update(self, delta_time: float):
        with self._lifecycle_lock:
            if self.shutdown_requested or self.state not in ("ready", "updating"):
                return False
            self.reload_changed_scripts()
            if self.shutdown_requested:
                return False
            self.state = "updating"
            callbacks = [self.project_script, self.current_scene_script, *self.actor_scripts.values()]
            for script in callbacks:
                if script is None or not script.is_initialized:
                    continue
                try:
                    script.update(delta_time)
                    script.on_update(delta_time)
                except Exception:
                    self.logger.exception("Script update failed: %s", script.name)
            self.state = "ready"
            return True

    def _shutdown_initialized_scripts(self):
        for record in reversed(self._script_records):
            instance = record.get("instance")
            if instance is None:
                continue
            try:
                instance.shutdown()
            except Exception:
                self.logger.exception("Script shutdown failed: %s", instance.name)
            sys.modules.pop(getattr(instance, "_corona_script_module_name", ""), None)
        self._script_records.clear()
        self.actor_scripts.clear()
        self.current_scene_script = None
        self.project_script = None

    def shutdown(self):
        with self._lifecycle_lock:
            if self.state == "stopped":
                return True
            self.shutdown_requested = True
            self.state = "stopping"
            self._shutdown_initialized_scripts()
            self.state = "stopped"
            self.logger.debug("Script system shutdown")
            return True

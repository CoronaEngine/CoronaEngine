from runtime import network_sync_policy
from api.editor_api import CoronaEditorApi
from runtime.plugin_base import PluginBase
from plugins.SceneTools.vision_import import (
    import_embedded_vision_scene_into_current_scene,
    import_vision_scene_into_current_scene,
    prepare_external_live_vision_scene,
)

import logging

logger = logging.getLogger(__name__)


@PluginBase.register_web("SceneTools")
class SceneTools(PluginBase):
    @staticmethod
    def _native_scene_tree_only(method_name: str) -> dict:
        return {
            "status": "error",
            "message": f"SceneTools.{method_name} is native-only; use the C++ native scene tree",
            "code": "native_scene_tree_only",
        }

    @staticmethod
    def _actor_sync_state(actor) -> dict:
        try:
            data = actor.to_dict()
        except Exception:
            data = {}
        data.setdefault("name", getattr(actor, "name", ""))
        data.setdefault("actor_guid", getattr(actor, "actor_guid", ""))
        data.setdefault("path", getattr(actor, "route", ""))
        data.setdefault("model", getattr(actor, "model_path", data.get("path", "")))
        data.setdefault("model_dependencies", list(getattr(actor, "model_dependencies", []) or []))
        data.setdefault("actor_type", getattr(actor, "actor_type", "model"))
        data.setdefault("visible", SceneTools._safe_actor_call(actor, "get_visible", True))
        data.setdefault("follow_camera", SceneTools._safe_actor_call(actor, "get_follow_camera", False))
        geometry = data.setdefault("geometry", {})
        geometry.setdefault("position", SceneTools._safe_actor_call(actor, "get_position", [0.0, 0.0, 0.0]))
        geometry.setdefault("rotation", SceneTools._safe_actor_call(actor, "get_rotation", [0.0, 0.0, 0.0]))
        geometry.setdefault("scale", SceneTools._safe_actor_call(actor, "get_scale", [1.0, 1.0, 1.0]))
        return {
            "actor_guid": data.get("actor_guid", ""),
            "name": data.get("name", ""),
            "actor_type": data.get("actor_type", "model"),
            "path": data.get("path") or data.get("model") or "",
            "model": data.get("model") or data.get("path") or "",
            "model_dependencies": data.get("model_dependencies", []),
            "visible": data.get("visible", True),
            "follow_camera": data.get("follow_camera", False),
            "geometry": data.get("geometry", {}),
        }

    @staticmethod
    def _canonical_actor_sync_state(actor_data) -> dict:
        data = actor_data if isinstance(actor_data, dict) else {}
        geometry = data.get("geometry") if isinstance(data.get("geometry"), dict) else {}

        def list_or_default(value, default):
            return list(value) if value is not None else list(default)

        return {
            "actor_guid": data.get("actor_guid", ""),
            "name": data.get("name", ""),
            "actor_type": data.get("actor_type", "model"),
            "path": data.get("path") or data.get("model") or "",
            "model": data.get("model") or data.get("path") or "",
            "model_dependencies": list(data.get("model_dependencies") or []),
            "visible": data.get("visible", True),
            "follow_camera": data.get("follow_camera", False),
            "geometry": {
                "position": list_or_default(geometry.get("position"), [0.0, 0.0, 0.0]),
                "rotation": list_or_default(geometry.get("rotation"), [0.0, 0.0, 0.0]),
                "scale": list_or_default(geometry.get("scale"), [1.0, 1.0, 1.0]),
            },
        }

    @staticmethod
    def _actor_sync_states_equal(local_actor, remote_actor_data) -> bool:
        local_state = SceneTools._canonical_actor_sync_state(
            SceneTools._actor_sync_state(local_actor))
        remote_state = SceneTools._canonical_actor_sync_state(remote_actor_data)
        return local_state == remote_state

    @staticmethod
    def _actor_snapshot_block_reason(actor_data) -> str | None:
        if not isinstance(actor_data, dict):
            return network_sync_policy.actor_data_sync_block_reason(actor_data)
        policy_data = dict(actor_data)
        # The receiver adds this flag to prevent rebroadcast loops; it should not
        # make an otherwise valid host snapshot actor ineligible for apply.
        policy_data.pop("_suppress_network_broadcast", None)
        return network_sync_policy.actor_data_sync_block_reason(policy_data)

    @staticmethod
    def _safe_actor_call(actor, method_name: str, default=None):
        if actor is None or not hasattr(actor, method_name):
            return default
        try:
            return getattr(actor, method_name)()
        except Exception:
            return default

    @staticmethod
    def _camera_view_payload(scene, camera) -> dict:
        payload = camera.to_dict()
        payload["scene_id"] = scene.route
        return payload

    @staticmethod
    def create_actor(scene_name: str, asset_path: str, actor_type: str = 'model', actor_data=None) -> dict:
        return SceneTools._native_scene_tree_only("create_actor")

    @staticmethod
    def create_actor_internal(scene_name: str, asset_path: str, actor_type: str = 'model', actor_data=None) -> dict:
        return SceneTools._native_scene_tree_only("create_actor_internal")

    @staticmethod
    def get_actor_sync_snapshot(scene_name: str) -> dict:
        return SceneTools._native_scene_tree_only("get_actor_sync_snapshot")

    @staticmethod
    def apply_actor_state_internal(scene_name: str, actor_guid: str, actor_data=None) -> dict:
        return SceneTools._native_scene_tree_only("apply_actor_state_internal")

    @staticmethod
    def apply_actor_sync_snapshot_internal(scene_name: str, snapshot=None) -> dict:
        return SceneTools._native_scene_tree_only("apply_actor_sync_snapshot_internal")

    @staticmethod
    def apply_actor_transform_internal(scene_name: str, actor_guid: str, actor_data=None) -> dict:
        return SceneTools._native_scene_tree_only("apply_actor_transform_internal")

    @staticmethod
    def _create_actor_impl(scene_name: str, asset_path: str, actor_type: str = 'model',
                           actor_data=None, notify_frontend: bool = True) -> dict:
        return SceneTools._native_scene_tree_only("_create_actor_impl")

    @staticmethod
    def create_scene(scene_name: str) -> dict:
        return SceneTools._native_scene_tree_only("create_scene")

    @staticmethod
    def remove_actor(scene_name: str, actor_name: str) -> dict:
        return SceneTools._native_scene_tree_only("remove_actor")

    @staticmethod
    def rename_actor(scene_name: str, actor_name: str, new_name: str) -> dict:
        return SceneTools._native_scene_tree_only("rename_actor")

    @staticmethod
    def remove_actor_internal(scene_name: str, actor_guid: str = "", actor_name: str = "") -> dict:
        return SceneTools._native_scene_tree_only("remove_actor_internal")

    @staticmethod
    def sun_direction(scene_name: str, if_enable: bool, direction: list[float]) -> dict:
        try:
            result = CoronaEditorApi.scene_tools.sun_direction(
                scene_name, bool(if_enable), direction
            )
            logger.info("Sun direction set for %s", scene_name)
            return result
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def floor_grid(scene_name: str, enabled: bool) -> dict:
        try:
            result = CoronaEditorApi.scene_tools.floor_grid(scene_name, bool(enabled))
            logger.info("Floor grid set for %s: %s", scene_name, enabled)
            return result
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def set_physics_params(scene_name: str, gravity: list = None, floor_y: float = None,
                           floor_restitution: float = None, fixed_dt: float = None) -> dict:
        """设置场景物理参数"""
        try:
            result = CoronaEditorApi.scene_tools.set_physics_params(
                scene_name, gravity, floor_y, floor_restitution, fixed_dt
            )
            logger.info("Physics params set for %s", scene_name)
            return result
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def get_physics_params(scene_name: str) -> dict:
        """获取场景物理参数"""
        try:
            return CoronaEditorApi.scene_tools.get_physics_params(scene_name)
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def save_screenshot(scene_name: str, path: str, camera_name: str = None) -> dict:
        try:
            result = CoronaEditorApi.scene_tools.save_screenshot(
                scene_name, path, camera_name
            )
            logger.info("Screenshot saved for scene %s camera %s to %s",
                        scene_name, camera_name, path)
            return result
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def set_output_mode(scene_name: str, camera_name: str = None, mode: str = "final_color") -> dict:
        try:
            result = CoronaEditorApi.scene_tools.set_output_mode(scene_name, camera_name, mode)
            logger.info("Output mode set to '%s' for scene %s camera %s",
                        mode, scene_name, camera_name)
            return result
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def get_output_mode(scene_name: str, camera_name: str = None) -> dict:
        try:
            return CoronaEditorApi.scene_tools.get_output_mode(scene_name, camera_name)
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def set_shadow_cascade_debug(scene_name: str, camera_name: str = None, enabled: bool = False) -> dict:
        try:
            return CoronaEditorApi.scene_tools.set_shadow_cascade_debug(
                scene_name, camera_name, bool(enabled)
            )
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def get_shadow_cascade_debug(scene_name: str, camera_name: str = None) -> dict:
        try:
            return CoronaEditorApi.scene_tools.get_shadow_cascade_debug(scene_name, camera_name)
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def set_ssao_enabled(scene_name: str, camera_name: str = None, enabled: bool = True) -> dict:
        try:
            return CoronaEditorApi.scene_tools.set_ssao_enabled(
                scene_name, camera_name, bool(enabled)
            )
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def get_ssao_enabled(scene_name: str, camera_name: str = None) -> dict:
        try:
            return CoronaEditorApi.scene_tools.get_ssao_enabled(scene_name, camera_name)
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def is_vision_available() -> dict:
        try:
            return CoronaEditorApi.scene_tools.is_vision_available()
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def set_render_backend(mode: str = "native", scene_name: str = None,
                           camera_name: str = None) -> dict:
        try:
            return CoronaEditorApi.scene_tools.set_render_backend(
                mode, scene_name, camera_name
            )
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def get_render_backend(scene_name: str = None, camera_name: str = None) -> dict:
        try:
            return CoronaEditorApi.scene_tools.get_render_backend(scene_name, camera_name)
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def set_vision_render_mode(scene_name: str, camera_name: str = None,
                               mode: str = "path_tracing") -> dict:
        try:
            result = CoronaEditorApi.scene_tools.set_vision_render_mode(
                scene_name, camera_name, mode
            )
            logger.info("Vision render mode set to '%s' for scene %s camera %s",
                        result.get("mode", mode), scene_name, camera_name)
            return result
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def get_vision_render_mode(scene_name: str, camera_name: str = None) -> dict:
        try:
            return CoronaEditorApi.scene_tools.get_vision_render_mode(scene_name, camera_name)
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def prepare_external_live_vision_scene(scene) -> str:
        return prepare_external_live_vision_scene(scene)

    @staticmethod
    def open_camera_view(scene_name: str, camera_name: str) -> dict:
        try:
            return CoronaEditorApi.scene_tools.open_camera_view(scene_name, camera_name)
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def close_camera_view(scene_name: str, camera_name: str) -> dict:
        try:
            return CoronaEditorApi.scene_tools.close_camera_view(scene_name, camera_name)
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def rename_camera_view(scene_name: str, camera_name: str, new_name: str) -> dict:
        try:
            return CoronaEditorApi.scene_tools.rename_camera_view(
                scene_name, camera_name, new_name
            )
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def list_camera_views(scene_name: str) -> dict:
        try:
            return CoronaEditorApi.scene_tools.list_camera_views(scene_name)
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def update_camera_view(scene_name: str, camera_name: str, state: dict) -> dict:
        try:
            return CoronaEditorApi.scene_tools.update_camera_view(
                scene_name, camera_name, state
            )
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def delete_camera(scene_name: str, camera_name: str) -> dict:
        try:
            return CoronaEditorApi.scene_tools.delete_camera(scene_name, camera_name)
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def load_vision_scene(path: str = "") -> dict:
        try:
            result = CoronaEditorApi.scene_tools.load_vision_scene(path)
            logger.info("Vision scene load requested: %s", path or "<unload>")
            return result
        except Exception as exc:
            return {"status": "error", "message": str(exc)}

    @staticmethod
    def import_vision_scene_into_current_scene(scene_name: str, path: str) -> dict:
        return import_vision_scene_into_current_scene(scene_name, path)

    @staticmethod
    def import_embedded_vision_scene_into_current_scene(scene_name: str) -> dict:
        return import_embedded_vision_scene_into_current_scene(scene_name)

    @staticmethod
    def list_actor_tree(scene_name) -> list:
        return SceneTools._native_scene_tree_only("list_actor_tree")

    @staticmethod
    def list_scene_tree(scene_name: str) -> dict:
        return SceneTools._native_scene_tree_only("list_scene_tree")

    @staticmethod
    def focus_actor(scene_name: str, actor_name: str, camera_name: str = None) -> dict:
        return SceneTools._native_scene_tree_only("focus_actor")

    @staticmethod
    def set_actor_state(scene_name: str, actor_name: str, state: dict) -> dict:
        return SceneTools._native_scene_tree_only("set_actor_state")

    @staticmethod
    def set_actor_camera_lock(scene_name: str, actor_name: str, camera_lock: dict) -> dict:
        return SceneTools._native_scene_tree_only("set_actor_camera_lock")

    @staticmethod
    def save_actor(scene_name: str, actor_name: str) -> dict:
        return SceneTools._native_scene_tree_only("save_actor")

    @staticmethod
    def select_model_file(scene_name: str, actor_name: str, file_type: str = "model") -> str:
        return SceneTools._native_scene_tree_only("select_model_file")

    @staticmethod
    def open_actor(scene_name: str, actor_name: str):
        return SceneTools._native_scene_tree_only("open_actor")

    @staticmethod
    def pick_actor_at_pixel(scene_name: str, x: float, y: float,
                            vp_width: float, vp_height: float) -> dict:
        return SceneTools._native_scene_tree_only("pick_actor_at_pixel")

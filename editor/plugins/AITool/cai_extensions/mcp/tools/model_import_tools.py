"""
模型导入 / 删除工具

提供将本地模型文件导入到当前场景以及从场景中删除模型的能力。
支持 .obj / .dae / .glb / .gltf / .fbx 格式。
"""
from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, List, Optional

from langchain_core.tools import StructuredTool
from pydantic import BaseModel, Field

from Quasar.ai_tools.response_adapter import (
    build_part,
    build_success_result,
    build_error_result,
)

DEFAULT_SCENE_NAME = ""
SUPPORTED_EXTS = {".obj", ".dae", ".glb", ".gltf", ".fbx"}


def _active_project_path(legacy_engine: Any = None) -> str:
    """Read project context from settings; keep an old-engine fallback for legacy builds."""
    from api import editor_api

    return editor_api.get_active_project_path(legacy_engine)


def _pick_model_file(path: str) -> Optional[str]:
    """
    如果 path 是目录，尝试从中挑选第一个支持的模型文件；
    如果 path 是文件，直接返回。
    """
    if os.path.isfile(path):
        if Path(path).suffix.lower() in SUPPORTED_EXTS:
            return path
        return None

    if os.path.isdir(path):
        for ext in SUPPORTED_EXTS:
            for f in sorted(os.listdir(path)):
                if f.lower().endswith(ext):
                    return os.path.join(path, f)
        for root, _dirs, files in os.walk(path):
            for ext in SUPPORTED_EXTS:
                for f in sorted(files):
                    if f.lower().endswith(ext):
                        return os.path.join(root, f)
    return None


def _actor_identity_from_native_result(native_result: dict[str, Any]) -> str:
    """Return the native actor identity required by AgentRuntime.

    A native import success without an actor id/guid is not usable for the
    RuntimeState/scene_entity_registry chain, so callers must treat an empty
    result as a tool failure instead of reporting a partial success.
    """

    actor = native_result.get("actor") if isinstance(native_result.get("actor"), dict) else {}
    actor_data = native_result.get("actor_data") if isinstance(native_result.get("actor_data"), dict) else {}
    for source in (actor, actor_data, native_result):
        for key in (
            "actor_guid",
            "actor_id",
            "guid",
            "actor_handle",
            "native_handle",
            "native_actor_id",
            "handle",
            "entity_id",
        ):
            value = source.get(key) if isinstance(source, dict) else None
            text = str(value or "").strip()
            if text:
                return text
    return ""


def _normalize_native_result(native_result_raw: Any) -> dict[str, Any]:
    """Normalize either manifest API data or the legacy JSON binding result."""

    native_result = (
        json.loads(native_result_raw)
        if isinstance(native_result_raw, str)
        else native_result_raw
    )
    if not isinstance(native_result, dict):
        raise RuntimeError(f"native actor create returned invalid data: {native_result_raw!r}")
    return native_result


def _create_native_editor_actor(
    *,
    scene_name: str,
    source_path: str,
    actor_type: str,
    actor_data: dict[str, Any],
    legacy_engine: Any = None,
) -> dict[str, Any]:
    """Create an actor through the manifest API, with an old-engine fallback.

    New editor builds expose ``SceneTools.create_actor`` through the C++
    manifest. Older F5 builds and isolated Python tests only expose the direct
    ``create_editor_actor`` binding, so that path remains a compatibility
    fallback rather than a second production entry point.
    """

    from api import editor_api

    scene_tools = editor_api.get_scene_tools_adapter(legacy_engine)
    if scene_tools is None:
        raise RuntimeError(
            "current engine exposes neither SceneTools.create_actor nor create_editor_actor"
        )
    return _normalize_native_result(
        scene_tools.create_actor(
            scene_name,
            source_path,
            actor_type,
            actor_data,
        )
    )


# ---------------------------------------------------------------------------
# Input Schema
# ---------------------------------------------------------------------------

class ImportModelInput(BaseModel):
    """将本地模型文件导入到当前场景"""

    model_path: str = Field(
        description="模型文件的路径（绝对路径或项目相对路径），支持 .obj/.dae/.glb/.gltf/.fbx。"
                    "也可以传入包含模型文件的目录路径，会自动选取其中的模型文件。",
    )
    actor_name: Optional[str] = Field(
        default=None,
        description="导入后在场景中的名称，为空则使用文件名",
    )
    model_name: Optional[str] = Field(
        default=None,
        description="兼容字段：模型语义名称，可作为导入后的场景别名",
    )
    object_id: Optional[str] = Field(
        default=None,
        description="兼容字段：规划物体 ID，可作为导入后的场景别名",
    )
    asset_id: Optional[str] = Field(
        default=None,
        description="Stable Runtime content identity for the imported model asset",
    )
    model_ref: Optional[str] = Field(
        default=None,
        description="Stable Runtime model reference retained in scene entity facts",
    )
    actor_guid: Optional[str] = Field(
        default=None,
        description="Stable Runtime actor identity used for idempotent native creation",
    )
    entity_id: Optional[str] = Field(default=None, description="Stable Runtime scene entity identity")
    entity_version: int = Field(default=1, ge=1, description="Initial Runtime entity version")
    source_plan_id: Optional[str] = Field(default=None, description="Owning Runtime scene plan")
    source_batch_id: Optional[str] = Field(default=None, description="Owning Runtime batch plan")
    plan_id: Optional[str] = Field(default=None, description="Compatibility alias for source_plan_id")
    batch_id: Optional[str] = Field(default=None, description="Compatibility alias for source_batch_id")
    skip_if_exists: bool = Field(
        default=False,
        description="Return an existing native actor with the same guid/name instead of duplicating it",
    )
    update_if_exists: bool = Field(
        default=False,
        description="Apply the requested transform when an idempotent actor already exists",
    )
    target: Optional[str] = Field(
        default=None,
        description="兼容字段：AI 规划目标名称，可作为导入后的场景别名",
    )
    position: Optional[List[float]] = Field(
        default=None,
        description="初始位置 [x, y, z]，为空默认 [0, 0, 0]",
    )
    rotation: Optional[List[float]] = Field(
        default=None,
        description="初始旋转（欧拉角）[pitch, yaw, roll]，为空默认 [0, 0, 0]",
    )
    scale: Optional[List[float]] = Field(
        default=None,
        description="初始缩放 [sx, sy, sz]，为空默认 [1, 1, 1]",
    )
    scene_name: str = Field(
        default=DEFAULT_SCENE_NAME,
        description="目标场景名称，为空则使用当前场景",
    )


class RemoveModelInput(BaseModel):
    """从场景中删除指定模型"""

    actor_name: str = Field(
        description="要删除的模型（Actor）名称",
    )
    scene_name: str = Field(
        default=DEFAULT_SCENE_NAME,
        description="目标场景名称，为空则使用当前场景",
    )


class ImportEnvironmentComponentInput(BaseModel):
    """Create a runtime-owned environment/substrate placeholder actor."""

    component_id: str = Field(description="Runtime environment component id")
    name: Optional[str] = Field(default=None, description="User-facing component name")
    component_type: str = Field(default="environment", description="terrain/room_box/room_floor/skybox/boundary")
    entity_type: str = Field(default="environment", description="Runtime entity domain")
    semantic_role: Optional[str] = Field(default=None, description="Runtime semantic role")
    handler: Optional[str] = Field(default=None, description="Runtime handler hint")
    object_id: Optional[str] = Field(default=None, description="Runtime object id")
    asset_id: Optional[str] = Field(default=None, description="Runtime asset id")
    model_ref: Optional[str] = Field(default=None, description="Optional environment asset/model reference")
    actor_guid: Optional[str] = Field(default=None, description="Stable Runtime actor identity")
    entity_id: Optional[str] = Field(default=None, description="Stable Runtime scene entity identity")
    entity_version: int = Field(default=1, ge=1, description="Initial Runtime entity version")
    source_plan_id: Optional[str] = Field(default=None, description="Owning Runtime scene plan")
    source_batch_id: Optional[str] = Field(default=None, description="Owning Runtime batch plan")
    plan_id: Optional[str] = Field(default=None, description="Compatibility alias for source_plan_id")
    batch_id: Optional[str] = Field(default=None, description="Compatibility alias for source_batch_id")
    position: Optional[List[float]] = Field(default=None, description="Initial position [x,y,z]")
    rotation: Optional[List[float]] = Field(default=None, description="Initial rotation [x,y,z]")
    scale: Optional[List[float]] = Field(default=None, description="Initial scale [x,y,z]")
    aabb: Optional[dict] = Field(default=None, description="Runtime AABB {min:[x,y,z], max:[x,y,z]}")
    surface: Optional[str] = Field(default=None, description="Surface semantic hint")
    terrain_profile: Optional[str] = Field(default=None, description="Terrain semantic hint")
    sky_mode: Optional[str] = Field(default=None, description="Sky semantic hint")
    boundary_style: Optional[str] = Field(default=None, description="Boundary semantic hint")
    scene_name: str = Field(default=DEFAULT_SCENE_NAME, description="Target scene name")


# ---------------------------------------------------------------------------
# Tool Builder
# ---------------------------------------------------------------------------

def _build_import_model_tool(scene_manager=None) -> StructuredTool:
    """构建模型导入工具"""

    def _import_model(
        *,
        model_path: str,
        actor_name: str | None = None,
        model_name: str | None = None,
        object_id: str | None = None,
        asset_id: str | None = None,
        model_ref: str | None = None,
        actor_guid: str | None = None,
        entity_id: str | None = None,
        entity_version: int = 1,
        source_plan_id: str | None = None,
        source_batch_id: str | None = None,
        plan_id: str | None = None,
        batch_id: str | None = None,
        skip_if_exists: bool = False,
        update_if_exists: bool = False,
        target: str | None = None,
        position: List[float] | None = None,
        rotation: List[float] | None = None,
        scale: List[float] | None = None,
        scene_name: str = DEFAULT_SCENE_NAME,
    ) -> str:
        try:
            # 1. 解析模型路径（支持绝对路径和项目相对路径）
            if os.path.isabs(model_path):
                resolved_path = model_path
            else:
                project_path = _active_project_path()
                if not project_path:
                    return build_error_result(
                        error_message="未设置活跃项目路径，无法解析相对路径"
                    ).to_envelope(interface_type="scene")
                resolved_path = os.path.join(project_path, model_path)

            # 2. 如果是目录，尝试挑选模型文件
            final_path = _pick_model_file(resolved_path)
            if final_path is None:
                return build_error_result(
                    error_message=f"找不到支持的模型文件: {resolved_path}，"
                                  f"支持格式: {', '.join(sorted(SUPPORTED_EXTS))}"
                ).to_envelope(interface_type="scene")

            if not os.path.exists(final_path):
                return build_error_result(
                    error_message=f"模型文件不存在: {final_path}"
                ).to_envelope(interface_type="scene")

            # 3. 交给 C++ native editor scene 创建 actor；Python 不再维护普通 actor runtime。
            preferred_name = next(
                (
                    str(value).strip()
                    for value in (actor_name, model_name, object_id, target)
                    if value and str(value).strip()
                ),
                Path(final_path).stem,
            )
            actor_data = {
                "actor_name": preferred_name,
                "model_name": model_name or preferred_name,
                "object_id": object_id or preferred_name,
                "asset_id": asset_id or object_id or model_name or preferred_name,
                "model_ref": model_ref or asset_id or model_name or object_id or preferred_name,
                "actor_guid": actor_guid or "",
                "entity_id": entity_id or "",
                "actor_version": max(1, int(entity_version or 1)),
                "source_plan_id": source_plan_id or plan_id or "",
                "source_batch_id": source_batch_id or batch_id or "",
                "skip_if_exists": bool(skip_if_exists),
                "update_if_exists": bool(update_if_exists),
                "target": target or preferred_name,
                "geometry": {},
                "mechanics": {"physics_enabled": False},
                "physics_enabled": False,
            }
            if position is not None:
                actor_data["position"] = position
                actor_data["geometry"]["position"] = position
            if rotation is not None:
                actor_data["rotation"] = rotation
                actor_data["geometry"]["rotation"] = rotation
            if scale is not None:
                actor_data["scale"] = scale
                actor_data["geometry"]["scale"] = scale

            native_result = _create_native_editor_actor(
                scene_name=scene_name,
                source_path=final_path,
                actor_type="model",
                actor_data=actor_data,
                legacy_engine=None,
            )
            if native_result.get("status") == "error":
                return build_error_result(
                    error_message=native_result.get("message") or native_result.get("error") or "native actor 创建失败"
                ).to_envelope(interface_type="scene")

            actor_id = _actor_identity_from_native_result(native_result)
            if not actor_id:
                return build_error_result(
                    error_message="native actor 创建成功但未返回 actor identity"
                ).to_envelope(interface_type="scene")

            actor = native_result.get("actor") if isinstance(native_result.get("actor"), dict) else {}
            scene_out = native_result.get("scene") or scene_name or ""
            geometry = actor.get("geometry") if isinstance(actor.get("geometry"), dict) else {}
            actor_aabb = actor.get("world_aabb") or actor.get("aabb") or actor.get("bounds")
            result_data = {
                "status": "success",
                "actor_id": actor_id,
                "entity_id": actor.get("entity_id") or entity_id or "",
                "actor_version": int(actor.get("actor_version") or actor.get("version") or entity_version or 1),
                "actor_name": actor.get("name", preferred_name),
                "asset_id": asset_id or object_id or model_name or preferred_name,
                "model_ref": model_ref or asset_id or model_name or object_id or preferred_name,
                "model_path": final_path,
                "position": geometry.get("position", position or [0, 0, 0]),
                "rotation": geometry.get("rotation", rotation or [0, 0, 0]),
                "scale": geometry.get("scale", scale or [1, 1, 1]),
                "scene": scene_out,
                "bounds_ready": bool(actor.get("bounds_ready")),
                "world_aabb": actor_aabb,
                "sync_status": "engine_imported",
                "sync_lifecycle_status": "engine_imported",
                "actor_data": {
                    "actor_id": actor_id,
                    "entity_id": actor.get("entity_id") or entity_id or "",
                    "actor_version": int(actor.get("actor_version") or actor.get("version") or entity_version or 1),
                    "name": actor.get("name") or preferred_name,
                    "asset_id": asset_id or object_id or model_name or preferred_name,
                    "model_ref": model_ref or asset_id or model_name or object_id or preferred_name,
                    "sync_status": "engine_imported",
                    "sync_lifecycle_status": "engine_imported",
                    "geometry": {
                        "position": geometry.get("position", position or [0, 0, 0]),
                        "rotation": geometry.get("rotation", rotation or [0, 0, 0]),
                        "scale": geometry.get("scale", scale or [1, 1, 1]),
                        "aabb": actor_aabb,
                    },
                },
                "actor": actor,
            }
            part = build_part(
                content_type="text",
                content_text=json.dumps(result_data, ensure_ascii=False),
            )
            return build_success_result(parts=[part]).to_envelope(
                interface_type="scene"
            )

        except FileNotFoundError as e:
            return build_error_result(
                error_message=str(e)
            ).to_envelope(interface_type="scene")
        except Exception as e:
            return build_error_result(
                error_message=f"模型导入失败: {e}"
            ).to_envelope(interface_type="scene")

    return StructuredTool(
        name="import_model",
        description="将本地 3D 模型文件导入到当前场景中。"
                    "支持 .obj/.dae/.glb/.gltf/.fbx 格式。"
                    "可指定名称、位置、旋转、缩放等参数。"
                    "也可传入包含模型文件的目录路径，会自动选取其中的模型文件。",
        args_schema=ImportModelInput,
        func=_import_model,
    )


def _build_import_environment_component_tool(scene_manager=None) -> StructuredTool:
    """Build the minimal Runtime environment import tool.

    This is intentionally a small native bridge adapter, not a legacy workflow.
    It creates a lightweight engine actor so Runtime can receive actor_id /
    transform / sync facts. Visual primitive fidelity remains a later C++/F5
    concern.
    """

    def _import_environment_component(
        *,
        component_id: str,
        name: str | None = None,
        component_type: str = "environment",
        entity_type: str = "environment",
        semantic_role: str | None = None,
        handler: str | None = None,
        object_id: str | None = None,
        asset_id: str | None = None,
        model_ref: str | None = None,
        actor_guid: str | None = None,
        entity_id: str | None = None,
        entity_version: int = 1,
        source_plan_id: str | None = None,
        source_batch_id: str | None = None,
        plan_id: str | None = None,
        batch_id: str | None = None,
        position: List[float] | None = None,
        rotation: List[float] | None = None,
        scale: List[float] | None = None,
        aabb: dict | None = None,
        surface: str | None = None,
        terrain_profile: str | None = None,
        sky_mode: str | None = None,
        boundary_style: str | None = None,
        scene_name: str = DEFAULT_SCENE_NAME,
    ) -> str:
        try:
            safe_component_id = str(component_id or "").strip()
            if not safe_component_id:
                return build_error_result(
                    error_message="environment component_id is required"
                ).to_envelope(interface_type="scene")

            component_name = str(name or safe_component_id).strip()
            component_type_value = str(component_type or "environment").strip() or "environment"
            asset_ref = str(model_ref or asset_id or safe_component_id).strip()
            source_path = asset_ref or f"runtime/environment/{safe_component_id}.component"
            semantic_role_value = str(semantic_role or "environment_component").strip()
            if component_type_value in {
                "room_box",
                "room_floor",
                "terrain",
                "ground",
                "boundary",
                "terrain_boundary",
                "sky",
                "skybox",
                "transition_zone",
            }:
                try:
                    from plugins.AITool.services.agent_runtime.environment_primitives import (
                        build_environment_primitive,
                    )
                except ModuleNotFoundError:
                    from editor.plugins.AITool.services.agent_runtime.environment_primitives import (
                        build_environment_primitive,
                    )

                primitive = build_environment_primitive(
                    component_type=component_type_value,
                    component_id=safe_component_id,
                    scale=scale,
                )
                source_path = primitive.model_path
                position = list(position or primitive.position)
                scale = list(primitive.scale)
                semantic_role_value = primitive.semantic_role
            actor_data = {
                "actor_name": component_name,
                "name": component_name,
                "model_name": component_name,
                "object_id": object_id or safe_component_id,
                "target": component_name,
                "component_id": safe_component_id,
                "component_type": component_type_value,
                "entity_type": str(entity_type or "environment"),
                "semantic_role": semantic_role_value,
                "handler": handler or "",
                "asset_id": asset_id or safe_component_id,
                "model_ref": model_ref or asset_ref,
                "actor_guid": actor_guid or "",
                "entity_id": entity_id or "",
                "actor_version": max(1, int(entity_version or 1)),
                "source_plan_id": source_plan_id or plan_id or "",
                "source_batch_id": source_batch_id or batch_id or "",
                "surface": surface or "",
                "terrain_profile": terrain_profile or "",
                "sky_mode": sky_mode or "",
                "boundary_style": boundary_style or "",
                "geometry": {},
                "mechanics": {"physics_enabled": False},
                "physics_enabled": False,
                "skip_if_exists": True,
                "update_if_exists": True,
            }
            if position is not None:
                actor_data["position"] = position
                actor_data["geometry"]["position"] = position
            if rotation is not None:
                actor_data["rotation"] = rotation
                actor_data["geometry"]["rotation"] = rotation
            if scale is not None:
                actor_data["scale"] = scale
                actor_data["geometry"]["scale"] = scale

            # Room framework components are real model actors.  Their semantic
            # identity remains environment-owned in RuntimeState.
            native_result = _create_native_editor_actor(
                scene_name=scene_name,
                source_path=source_path,
                actor_type="model",
                actor_data=actor_data,
                legacy_engine=None,
            )
            if native_result.get("status") == "error":
                return build_error_result(
                    error_message=native_result.get("message") or native_result.get("error") or "native environment actor 创建失败"
                ).to_envelope(interface_type="scene")

            actor_id = _actor_identity_from_native_result(native_result)
            if not actor_id:
                return build_error_result(
                    error_message="native environment actor 创建成功但未返回 actor identity"
                ).to_envelope(interface_type="scene")

            actor = native_result.get("actor") if isinstance(native_result.get("actor"), dict) else {}
            geometry = actor.get("geometry") if isinstance(actor.get("geometry"), dict) else {}
            native_aabb = actor.get("world_aabb") or actor.get("aabb") or actor.get("bounds")
            actor_aabb = native_aabb or aabb
            bounds_ready = bool(actor.get("bounds_ready") and native_aabb)
            bounds_source = "engine_actual" if bounds_ready else "estimated"
            engine_lifecycle_status = "bounds_ready" if bounds_ready else "engine_loading"
            actor_size = actor.get("size") or actor.get("dimensions") or actor.get("aabb_size")
            result_data = {
                "status": "success",
                "component_id": safe_component_id,
                "component_type": component_type_value,
                "entity_type": str(entity_type or "environment"),
                "semantic_role": semantic_role_value,
                "handler": handler or "",
                "name": component_name,
                "asset_id": asset_id or safe_component_id,
                "model_ref": model_ref or asset_ref,
                "surface": surface or "",
                "terrain_profile": terrain_profile or "",
                "sky_mode": sky_mode or "",
                "boundary_style": boundary_style or "",
                "scene_name": native_result.get("scene") or scene_name or "",
                "actor_id": actor_id,
                "entity_id": actor.get("entity_id") or entity_id or "",
                "actor_version": int(actor.get("actor_version") or actor.get("version") or entity_version or 1),
                "bounds_ready": bounds_ready,
                "bounds_source": bounds_source,
                "engine_lifecycle_status": engine_lifecycle_status,
                "world_aabb": actor_aabb,
                "size": actor_size,
                "actor_data": {
                    "actor_id": actor_id,
                    "actor_guid": actor_id,
                    "entity_id": actor.get("entity_id") or entity_id or "",
                    "actor_version": int(actor.get("actor_version") or actor.get("version") or entity_version or 1),
                    "name": actor.get("name") or component_name,
                    "component_id": safe_component_id,
                    "component_type": component_type_value,
                    "entity_type": str(entity_type or "environment"),
                    "semantic_role": semantic_role_value,
                    "asset_id": asset_id or safe_component_id,
                    "model_ref": model_ref or asset_ref,
                    "sync_status": "engine_imported",
                    "sync_lifecycle_status": "engine_imported",
                    "bounds_ready": bounds_ready,
                    "bounds_source": bounds_source,
                    "engine_lifecycle_status": engine_lifecycle_status,
                    "size": actor_size,
                    "geometry": {
                        "position": geometry.get("position", position or [0, 0, 0]),
                        "rotation": geometry.get("rotation", rotation or [0, 0, 0]),
                        "scale": geometry.get("scale", scale or [1, 1, 1]),
                        "aabb": actor_aabb,
                    },
                },
                "actor": actor,
                "sync_status": "engine_imported",
                "sync_lifecycle_status": "engine_imported",
            }
            part = build_part(
                content_type="text",
                content_text=json.dumps(result_data, ensure_ascii=False),
            )
            return build_success_result(parts=[part]).to_envelope(interface_type="scene")

        except Exception as e:
            return build_error_result(
                error_message=f"environment component import failed: {e}"
            ).to_envelope(interface_type="scene")

    return StructuredTool(
        name="import_environment_component",
        description="Create a native Runtime terrain/room/sky/boundary component actor.",
        args_schema=ImportEnvironmentComponentInput,
        func=_import_environment_component,
    )


def _build_remove_model_tool(scene_manager=None) -> StructuredTool:
    """构建模型删除工具"""

    def _remove_model(
        *,
        actor_name: str,
        scene_name: str = DEFAULT_SCENE_NAME,
    ) -> str:
        try:
            from api import editor_api

            scene_tools = editor_api.get_scene_tools_adapter()
            if scene_tools is None:
                return build_error_result(
                    error_message="当前引擎缺少 SceneTools.remove_actor 或兼容删除接口"
                ).to_envelope(interface_type="scene")

            native_result_raw = scene_tools.remove_actor(scene_name, actor_name)
            native_result = (
                json.loads(native_result_raw)
                if isinstance(native_result_raw, str)
                else native_result_raw
            )
            if not isinstance(native_result, dict):
                return build_error_result(
                    error_message=f"native actor 删除返回无效: {native_result_raw!r}"
                ).to_envelope(interface_type="scene")
            if native_result.get("status") == "error":
                return build_error_result(
                    error_message=native_result.get("message") or native_result.get("error") or "native actor 删除失败"
                ).to_envelope(interface_type="scene")

            result_data = {
                "status": "success",
                "removed_actor": native_result.get("actor", actor_name),
                "actor_guid": native_result.get("actor_guid", ""),
                "scene": native_result.get("scene", scene_name),
            }
            part = build_part(
                content_type="text",
                content_text=json.dumps(result_data, ensure_ascii=False),
            )
            return build_success_result(parts=[part]).to_envelope(
                interface_type="scene"
            )

        except Exception as e:
            return build_error_result(
                error_message=f"模型删除失败: {e}"
            ).to_envelope(interface_type="scene")

    return StructuredTool(
        name="remove_model", 
        description="从当前场景中删除指定的模型（Actor）。"
                    "需要提供模型名称，支持模糊匹配（忽略引号、扩展名）。",
        args_schema=RemoveModelInput,
        func=_remove_model,
    )


# ---------------------------------------------------------------------------
# Public Loader
# ---------------------------------------------------------------------------

def load_model_import_tools() -> List[StructuredTool]:
    return [
        _build_import_model_tool(),
        _build_import_environment_component_tool(),
        _build_remove_model_tool(),
    ]


__all__ = ["load_model_import_tools"]

import math
import os
import logging
import tempfile
import time
import uuid
from pathlib import Path
from typing import Any, Dict, List

from CoronaCore.core.managers import scene_manager
from Quasar.ai_workflow.streaming import stream_output_node
from .temp_capture_storage import (
    build_temp_capture_root,
    cleanup_temp_capture_dir,
    make_temp_capture_path,
    save_to_temp_then_move,
)
from .formatters import NO_OUTPUT
from .helpers import get_tool, wait_mesh_then_resolve_model_file

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# 白色平面 OBJ 资源（懒生成，全进程复用）
# ---------------------------------------------------------------------------
_WHITE_PLANE_OBJ_PATH: str = ""


def _get_white_plane_obj() -> str:
    """返回一个 1x1 白色平面 OBJ 的绝对路径（写入系统临时目录，全程复用）。

    平面中心在原点，朝向 +Y（法线向上），尺寸 1x1。
    调用方用 set_scale 把它撑开到合适大小。
    """
    global _WHITE_PLANE_OBJ_PATH
    if _WHITE_PLANE_OBJ_PATH and os.path.exists(_WHITE_PLANE_OBJ_PATH):
        return _WHITE_PLANE_OBJ_PATH

    tmp_dir = Path(tempfile.gettempdir()) / "corona_white_plane"
    tmp_dir.mkdir(parents=True, exist_ok=True)

    mtl_path = tmp_dir / "white.mtl"
    obj_path = tmp_dir / "white_plane.obj"

    mtl_path.write_text(
        "newmtl white\n"
        "Ka 1.0 1.0 1.0\n"
        "Kd 1.0 1.0 1.0\n"
        "Ks 0.0 0.0 0.0\n"
        "Ns 0.0\n"
        "d 1.0\n",
        encoding="ascii",
    )

    # 1x1 quad，法线 +Y，UV 覆盖 [0,1]²
    obj_path.write_text(
        "mtllib white.mtl\n"
        "usemtl white\n"
        "v -0.5  0.0 -0.5\n"
        "v  0.5  0.0 -0.5\n"
        "v  0.5  0.0  0.5\n"
        "v -0.5  0.0  0.5\n"
        "vn  0.0  1.0  0.0\n"
        "vt  0.0  0.0\n"
        "vt  1.0  0.0\n"
        "vt  1.0  1.0\n"
        "vt  0.0  1.0\n"
        "f 1/1/1 2/2/1 3/3/1 4/4/1\n",
        encoding="ascii",
    )

    _WHITE_PLANE_OBJ_PATH = str(obj_path)
    logger.debug("白色平面 OBJ 已生成: %s", _WHITE_PLANE_OBJ_PATH)
    return _WHITE_PLANE_OBJ_PATH


def _create_white_box_actors(scene, aabb: List[float], margin: float = 1.2) -> List[Any]:
    """在场景中创建 6 个纯白平面 Actor，围成包裹物体 AABB 的封闭白盒。

    Args:
        scene: CoronaCore Scene 实例
        aabb: [min_x, min_y, min_z, max_x, max_y, max_z]
        margin: AABB 尺寸的扩展倍率（默认 1.2 倍，留出拍摄余量）

    Returns:
        创建的 Actor 列表，由调用方负责销毁。
    """
    from CoronaCore.core.entities.actor import Actor

    min_x, min_y, min_z, max_x, max_y, max_z = aabb
    cx = (min_x + max_x) / 2.0
    cy = (min_y + max_y) / 2.0
    cz = (min_z + max_z) / 2.0
    dx = (max_x - min_x) * margin
    dy = (max_y - min_y) * margin
    dz = (max_z - min_z) * margin
    # 保证最小尺寸，避免退化
    dx = max(dx, 0.1)
    dy = max(dy, 0.1)
    dz = max(dz, 0.1)

    # 半尺寸偏移
    hx, hy, hz = dx / 2.0, dy / 2.0, dz / 2.0

    plane_obj = _get_white_plane_obj()

    # 六个面的配置: (name, position, rotation_deg_xyz, scale_xyz)
    # 平面默认法线 +Y，通过旋转对齐到各个面
    face_configs = [
        # 底面 (-Y，法线朝内 +Y，不旋转)
        ("__wb_bottom", [cx, cy - hy, cz], [0.0, 0.0, 0.0],   [dx, 1.0, dz]),
        # 顶面 (+Y，旋转 180° 使法线朝内 -Y)
        ("__wb_top",    [cx, cy + hy, cz], [180.0, 0.0, 0.0], [dx, 1.0, dz]),
        # 前面 (-Z，旋转 90° around X → 法线朝 +Z → 内朝 +Z)
        ("__wb_front",  [cx, cy, cz - hz], [-90.0, 0.0, 0.0], [dx, 1.0, dy]),
        # 后面 (+Z，旋转 90° 反向)
        ("__wb_back",   [cx, cy, cz + hz], [90.0, 0.0, 0.0],  [dx, 1.0, dy]),
        # 左面 (-X，旋转 90° around Z)
        ("__wb_left",   [cx - hx, cy, cz], [0.0, 0.0, -90.0], [dz, 1.0, dy]),
        # 右面 (+X)
        ("__wb_right",  [cx + hx, cy, cz], [0.0, 0.0, 90.0],  [dz, 1.0, dy]),
    ]

    created: List[Any] = []
    for name, pos, rot, scale in face_configs:
        try:
            actor = Actor(
                name=name,
                route=plane_obj,
                actor_type="mesh",
                parent_scene=scene,
            )
            actor.set_position(pos, True)
            actor.set_rotation(rot, True)
            actor.set_scale(scale, True)
            scene.add_actor(actor)
            created.append(actor)
        except Exception as exc:
            logger.warning("创建白盒平面 %s 失败: %s", name, exc)

    logger.debug("白色封闭盒创建完成，共 %d 个平面", len(created))
    return created


def _remove_white_box_actors(scene, actors: List[Any]) -> None:
    """移除白盒 Actor。"""
    for actor in actors:
        try:
            scene.remove_actor(actor)
        except Exception as exc:
            logger.warning("移除白盒平面失败: %s", exc)


def _resolve_active_scene():
    """健壮的场景获取逻辑"""
    scene = scene_manager.get("")
    if scene is not None:
        return scene
    routes = scene_manager.list_all()
    if routes:
        return scene_manager.get(routes[0])
    return None


# ---------------------------------------------------------------------------
# 临时场景六视图拍摄（核心逻辑）
# ---------------------------------------------------------------------------

# 四视图配置：(仰角, 偏航角) —— 暂时去掉 top/bottom，仅保留水平四向
# 坐标系 X+右 Y+上 Z+前, 相机位于目标方向的反侧，朝向物体中心
_SIX_VIEW_CONFIGS = {
    "front":  (0.0,   180.0),  # 相机在 -Z，朝 +Z 看（正面）
    "back":   (0.0,     0.0),  # 相机在 +Z，朝 -Z 看（背面）
    "left":   (0.0,   270.0),  # 相机在 -X，朝 +X 看（左侧）
    "right":  (0.0,    90.0),  # 相机在 +X，朝 -X 看（右侧）
}


def _run_capture_in_temp_scene(
    active_scene: Any,
    active_camera: Any,
    actor_name: str,
    final_model_path: str,
    output_dir: str,
    temp_capture_root: Path,
) -> dict:
    """在独立临时场景中完成六视图截图，与主场景完全隔离。

    策略：
    1. 新建临时场景（禁止写磁盘）。
    2. 创建离屏截图相机（不绑定 surface），纯离屏渲染。
       同时禁用主场景渲染以节省 GPU 算力。
       主场景的相机保持不动——不做任何迁移。
    3. 在临时场景中加载模型并构建白色封闭盒背景。
       白盒边长 = 相机轨道半径 × 3，确保相机始终在盒内、背景面充满画面。
    4. 截图结束后销毁截图相机、恢复主场景，销毁临时场景。
    """
    from CoronaCore.core.managers import scene_manager as sm
    from CoronaCore.core.entities.actor import Actor
    from CoronaCore.core.entities.camera import Camera as PyCamera

    temp_route = f"__six_view_tmp_{uuid.uuid4().hex[:8]}__"
    temp_scene = None
    actor = None
    capture_camera = None
    white_box_actors: List[Any] = []
    view_dict: dict = {}

    # 读取 active_camera 参数，用于初始化截图相机
    orig_fov = float(active_camera.get_fov())

    try:
        # ── 1. 创建临时场景，禁止持久化 ─────────────────────────────────
        temp_scene = sm.create(temp_route)
        temp_scene.save_data = lambda: None  # 不写磁盘
        logger.info("[Workflow][TempScene] 创建临时场景: %s", temp_route)

        # ── 2. 创建离屏截图相机（无 surface，纯离屏渲染）────────────────
        #       禁用主场景以节省 GPU 算力
        active_scene.set_enabled(False)

        # 移除 ensure_default_camera 自动创建的默认相机
        for cam in list(temp_scene._cameras):
            temp_scene.engine_scene.remove_camera(getattr(cam, "engine_obj", cam))
        temp_scene._cameras.clear()
        temp_scene._main_camera = None

        # 创建截图专用相机（离屏，不绑定默认 viewport surface）
        capture_camera = PyCamera(
            position=[0.0, 0.0, 0.0],
            forward=[0.0, 0.0, 1.0],
            world_up=[0.0, 1.0, 0.0],
            fov=orig_fov,
            name="__six_view_capture_cam__",
        )
        capture_camera.set_surface(0)
        capture_camera.set_output_mode("base_color")
        temp_scene.engine_scene.add_camera(capture_camera.engine_obj)
        temp_scene._cameras = [capture_camera]
        temp_scene._main_camera = capture_camera

        # ── 3. 加载目标模型到临时场景 ────────────────────────────────────
        logger.info("[Workflow][TempScene] 加载模型: %s => %s", actor_name, final_model_path)
        actor = Actor(
            name=actor_name,
            route=final_model_path,
            actor_type="mesh",
            parent_scene=temp_scene,
        )
        temp_scene.add_actor(actor)
        time.sleep(1.0)  # 等待 GPU 资源（Mesh / Texture）加载完毕

        # ── 4. 计算 AABB、拍摄中心与相机轨道半径 ────────────────────────
        aabb      = actor._geometry.get_aabb()
        actor_pos = actor.get_position()
        actor_scl = actor.get_scale()

        model_center = [
            (aabb[0] + aabb[3]) / 2.0,
            (aabb[1] + aabb[4]) / 2.0,
            (aabb[2] + aabb[5]) / 2.0,
        ]
        center = [
            actor_pos[0] + model_center[0] * actor_scl[0],
            actor_pos[1] + model_center[1] * actor_scl[1],
            actor_pos[2] + model_center[2] * actor_scl[2],
        ]
        dx = (aabb[3] - aabb[0]) * actor_scl[0]
        dy = (aabb[4] - aabb[1]) * actor_scl[1]
        dz = (aabb[5] - aabb[2]) * actor_scl[2]
        # 相机轨道半径 = 对角线 × 1.5，最小 1.0
        distance = max(math.sqrt(dx * dx + dy * dy + dz * dz) * 1.5, 1.0)
        fov = orig_fov

        # ── 5. 构建白色封闭盒背景 ────────────────────────────────────────
        # 白盒边长基于相机轨道半径而非 AABB（避免各向异性 AABB 导致某面太小）：
        #   box_half = distance × 3.0
        # 验证（fov=60°）：
        #   背景面距相机 = box_half + distance = 4.0 × distance
        #   画面覆盖宽度 = 2 × 4.0d × tan(30°) ≈ 4.62d < 6.0d（白盒面宽）✓
        box_half = distance * 3.0
        cubic_aabb = [
            center[0] - box_half, center[1] - box_half, center[2] - box_half,
            center[0] + box_half, center[1] + box_half, center[2] + box_half,
        ]
        white_box_actors = _create_white_box_actors(temp_scene, cubic_aabb, margin=1.0)
        if white_box_actors:
            time.sleep(0.3)  # 等待白盒网格加载

        # ── 6. 六视图截图 ─────────────────────────────────────────────────
        for view_name, (elev_deg, az_deg) in _SIX_VIEW_CONFIGS.items():
            elev_rad = math.radians(elev_deg)
            az_rad   = math.radians(az_deg)
            cos_elev, sin_elev = math.cos(elev_rad), math.sin(elev_rad)
            cos_az,   sin_az   = math.cos(az_rad),   math.sin(az_rad)

            offset_x = distance * cos_elev * sin_az
            offset_y = distance * sin_elev
            offset_z = distance * cos_elev * cos_az

            position = [
                center[0] + offset_x,
                center[1] + offset_y,
                center[2] + offset_z,
            ]
            fwd_raw = [center[i] - position[i] for i in range(3)]
            fwd_len = math.sqrt(sum(f * f for f in fwd_raw))
            fwd = [f / fwd_len for f in fwd_raw] if fwd_len > 1e-6 else [0.0, 0.0, -1.0]

            # 防万向锁：正上/正下视图需重置 Up 向量
            if elev_deg >= 89.0:
                up = [0.0, 0.0, 1.0]
            elif elev_deg <= -89.0:
                up = [0.0, 0.0, -1.0]
            else:
                up = [0.0, 1.0, 0.0]

            capture_camera.set(position, fwd, up, fov)
            time.sleep(0.3)  # 等待渲染管线刷新

            filepath      = os.path.join(output_dir, f"{view_name}.png")
            temp_filepath = make_temp_capture_path(temp_capture_root, str(actor_name), view_name)
            saved = save_to_temp_then_move(
                capture_camera,
                temp_path=temp_filepath,
                final_path=filepath,
                actor_name=str(actor_name),
                view_name=view_name,
            )
            if saved:
                view_dict[view_name] = saved

        if view_dict:
            logger.info("[Workflow][TempScene] %s 六视图完成: %s/6", actor_name, len(view_dict))
        else:
            logger.error("[Workflow][TempScene] %s 六视图失败: 0/6", actor_name)

    except Exception as exc:
        logger.error("[Workflow][TempScene] 截图崩溃: %s", exc, exc_info=True)

    finally:
        # 清理白色封闭盒
        if white_box_actors and temp_scene is not None:
            _remove_white_box_actors(temp_scene, white_box_actors)

        # 清理模型 Actor
        if actor is not None and temp_scene is not None:
            try:
                temp_scene.remove_actor(actor)
            except Exception as exc:
                logger.warning("[Workflow][TempScene] 清理临时模型失败: %s", exc)

        # 清理截图相机
        if capture_camera is not None and temp_scene is not None:
            try:
                temp_scene.engine_scene.remove_camera(capture_camera.engine_obj)
                temp_scene._cameras.clear()
                temp_scene._main_camera = None
            except Exception as exc:
                logger.warning("[Workflow][TempScene] 移除截图相机失败: %s", exc)

        # 恢复主场景渲染（主场景相机从未被移动，无需归还）
        try:
            active_scene.set_enabled(True)
        except Exception as exc:
            logger.error("[Workflow][TempScene] 恢复主场景失败 (严重): %s", exc)

        # 销毁临时场景（从 scene_manager 注销，GC 回收 C++ 资源）
        if temp_scene is not None:
            sm.remove(temp_route)
            logger.info("[Workflow][TempScene] 临时场景已销毁: %s", temp_route)

    return view_dict


def capture_single_result(
    result: Dict[str, Any],
    active_scene: Any,
    temp_capture_root: Path,
) -> dict | None:
    """对单个生成结果执行六视图拍摄，返回 view_dict 或 None。

    必须串行调用——共用主场景相机，不支持并发。
    """
    if result.get("error"):
        return None
    if result.get("source") != "generation":
        return None
    if result.get("review_passed"):
        return None
    if result.get("six_views_dict"):
        return None

    actor_name = result.get("object_id") or result.get("item_name")
    raw_model_path = result.get("model_path")
    parameter = (
        result.get("parameter", {})
        if isinstance(result.get("parameter"), dict)
        else {}
    )
    has_mesh_pending = bool(parameter.get("has_mesh_pending", False))
    wait_object_id = str(parameter.get("object_id") or actor_name or "")

    if not actor_name or not raw_model_path:
        return None

    final_model_path = wait_mesh_then_resolve_model_file(
        raw_model_path=str(raw_model_path),
        wait_object_id=wait_object_id,
        has_mesh_pending=has_mesh_pending,
    )

    if not final_model_path:
        logger.warning(
            "[Workflow][capture] %s 截图前模型未就绪，在 %s 及其子目录下未找到 .glb/.obj，跳过。",
            actor_name,
            raw_model_path,
        )
        return None

    output_dir = os.path.dirname(final_model_path)
    os.makedirs(output_dir, exist_ok=True)

    active_camera = active_scene.find_camera(None)
    if not active_camera:
        logger.error(
            "[Workflow][capture] 场景中没有可用相机，跳过 %s 截图",
            actor_name,
        )
        return None

    logger.info("[Workflow][capture] 正在为 %s 新开临时场景进行六视图截图...", actor_name)
    view_dict = _run_capture_in_temp_scene(
        active_scene=active_scene,
        active_camera=active_camera,
        actor_name=actor_name,
        final_model_path=final_model_path,
        output_dir=output_dir,
        temp_capture_root=temp_capture_root,
    )

    if view_dict:
        logger.info(
            "[Workflow][capture] %s 六视图完成: %s/6",
            actor_name,
            len(view_dict),
        )
    else:
        logger.error("[Workflow][capture] %s 六视图失败: 0/6", actor_name)

    return view_dict if view_dict else None


@stream_output_node("integrated", NO_OUTPUT)
def six_view_capture_tool_node(state: Dict[str, Any]) -> Dict[str, Any]:
    """六视图截图节点：为每个生成结果在独立临时场景中拍摄标准六视图。"""
    # 并行工作流中 Hunyuan3D 已提供多视角预览图，跳过截图以节省 GPU
    if state.get("metadata", {}).get("skip_six_view_capture"):
        existing_views = dict(state.get("six_view_images", {}) or {})
        for result in state.get("model_results", []):
            if not result.get("error") and result.get("source") == "generation":
                name = result.get("object_id") or result.get("item_name") or ""
                if name:
                    existing_views[name] = result.get("six_views_dict", {})
        return {"six_view_images": existing_views}

    model_results = state.get("model_results", [])
    if not model_results:
        return {"six_view_images": {}}

    multiview_tool = get_tool("camera_multiview_capture")
    if not multiview_tool:
        return {"six_view_images": {}}

    active_scene = _resolve_active_scene()
    if active_scene is None:
        logger.warning("[Workflow] 未加载任何场景，无法执行截图")
        return {"six_view_images": {}}

    all_saved_views = dict(state.get("six_view_images", {}) or {})
    temp_capture_root = build_temp_capture_root()

    for result in model_results:
        if result.get("error"):
            continue
        if result.get("source") != "generation":
            continue
        if result.get("review_passed"):
            continue
        if result.get("six_views_dict"):
            actor_name = result.get("object_id") or result.get("item_name")
            if actor_name:
                all_saved_views[actor_name] = result["six_views_dict"]
            continue

        actor_name = result.get("object_id") or result.get("item_name")
        raw_model_path = result.get("model_path")
        parameter = result.get("parameter", {}) if isinstance(result.get("parameter"), dict) else {}
        has_mesh_pending = bool(parameter.get("has_mesh_pending", False))
        wait_object_id = str(parameter.get("object_id") or actor_name or "")

        if not actor_name or not raw_model_path:
            continue

        # Mesh 完成门禁，解析真实 3D 文件路径
        final_model_path = wait_mesh_then_resolve_model_file(
            raw_model_path=str(raw_model_path),
            wait_object_id=wait_object_id,
            has_mesh_pending=has_mesh_pending,
        )

        if not final_model_path:
            logger.warning(
                "[Workflow] %s 截图前模型未就绪，在 %s 及其子目录下未找到 .glb/.obj，跳过。",
                actor_name, raw_model_path,
            )
            continue

        # final_model_path 已是绝对路径，直接取其父目录作为截图输出目录
        output_dir = os.path.dirname(final_model_path)
        os.makedirs(output_dir, exist_ok=True)

        # 获取主场景相机（持有渲染 surface，是截图的必要条件）
        active_camera = active_scene.find_camera(None)
        if not active_camera:
            logger.error("[Workflow] 场景中没有可用相机，跳过 %s 截图", actor_name)
            continue

        logger.info("[Workflow] 正在为 %s 新开临时场景进行六视图截图...", actor_name)
        view_dict = _run_capture_in_temp_scene(
            active_scene=active_scene,
            active_camera=active_camera,
            actor_name=actor_name,
            final_model_path=final_model_path,
            output_dir=output_dir,
            temp_capture_root=temp_capture_root,
        )

        if view_dict:
            all_saved_views[actor_name] = view_dict
            result["six_views_dict"] = view_dict
            logger.info("[Workflow] %s 六视图生成完成: 成功 %s/6", actor_name, len(view_dict))
        else:
            logger.error("[Workflow] %s 六视图生成失败: 0/6", actor_name)

    cleanup_temp_capture_dir(temp_capture_root)
    return {
        "model_results": model_results,
        "six_view_images": all_saved_views,
    }

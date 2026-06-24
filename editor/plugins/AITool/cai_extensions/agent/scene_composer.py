"""场景组合器 — 从设计描述/物品清单批量生成并布局多个 3D 物体。

区别于单步编辑（加一个物体）和 multi-step（泛泛分解为几步），
SceneComposer 处理「根据这份清单/方案组合整个场景」类需求：

  1. 用 LLM 从文字中提取结构化物体清单
  2. 为每个物体获取 3D 模型（复用 ModelProvider：搜索→生成→下载）
  3. 用 Constraint Solver / 默认网格布局算出每个物体位置
  4. 返回组合结果（供上层导入引擎 + 广播）

全程详细日志便于验收。
"""

from __future__ import annotations

import json
import logging
import os
import re
import time
import uuid
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

logger = logging.getLogger(__name__)


def _safe_generated_asset_name(value: str, fallback: str = "scene") -> str:
    text = re.sub(r"[^0-9A-Za-z_.\-\u4e00-\u9fff]+", "_", str(value or "").strip())
    text = text.strip("._-")
    return (text or fallback)[:48]


def _build_room_box_obj(width: float, height: float, depth: float,
                        door: Optional[Dict[str, float]] = None,
                        open_face: str = "front",
                        floor_mtl: str = "floor",
                        wall_mtl: str = "wall",
                        ceiling_mtl: str = "ceiling") -> str:
    """构建房间盒子 OBJ 字符串（单位立方体，缩放由 Actor 完成）。

    M2 步骤 14b-i：纯函数，可离线几何验证（共面 + 法向内向）。
    - open_face="front" → 默认 5 面展示盒：前墙开放，便于 F5/VLM/用户观察室内。
    - open_face="none" + door=None → 闭合六面盒子（旧几何回退）。
    - open_face="none" + door={width, height} → 前墙(z=+0.5)拆成左柱+右柱+门楣，包住一个落地门洞，
      camera 可从门洞走进去。门洞横向居中、底边贴地。
    - open_face="front_and_ceiling" → 去前墙和顶面，作为 demo/debug 展示模式。

    单位立方体边长 1（中心原点），door 尺寸以"米"给出，按 width/height 归一化到
    单位坐标。所有面法向指向盒内（背面剔除后摄像机能看进内部）。
    """
    # 8 基础顶点 + 6 法向（与旧表一致）
    floor_mtl = (floor_mtl or "floor").strip() or "floor"
    wall_mtl = (wall_mtl or "wall").strip() or "wall"
    ceiling_mtl = (ceiling_mtl or "ceiling").strip() or "ceiling"
    head = (
        "mtllib box.mtl\n"
        "# 8 vertices of a 1x1x1 cube centered at origin\n"
        "v -0.5 -0.5 -0.5\nv  0.5 -0.5 -0.5\nv  0.5  0.5 -0.5\nv -0.5  0.5 -0.5\n"
        "v -0.5 -0.5  0.5\nv  0.5 -0.5  0.5\nv  0.5  0.5  0.5\nv -0.5  0.5  0.5\n"
        "vn  0.0  0.0 -1.0\nvn  1.0  0.0  0.0\nvn  0.0  0.0  1.0\nvn -1.0  0.0  0.0\n"
        "vn  0.0  1.0  0.0\nvn  0.0 -1.0  0.0\n"
    )
    # 背/左/右/底/顶 5 面（门洞只改前墙，这 5 面恒定，法向内向）
    open_face = (open_face or "front").strip().lower()
    if open_face not in ("front", "none", "front_and_ceiling"):
        open_face = "front"

    side_faces = (
        f"usemtl {wall_mtl}\n"
        "f 1//3 2//3 3//3 4//3\n"   # back   z=-0.5 inward +Z
        "f 1//2 4//2 8//2 5//2\n"   # left   x=-0.5 inward +X
        "f 2//4 6//4 7//4 3//4\n"   # right  x=+0.5 inward -X
        f"usemtl {floor_mtl}\n"
        "f 1//5 5//5 6//5 2//5\n"   # bottom y=-0.5 inward +Y
    )
    top_face = f"usemtl {ceiling_mtl}\nf 4//6 3//6 7//6 8//6\n"   # top y=+0.5 inward -Y
    common_faces = side_faces + ("" if open_face == "front_and_ceiling" else top_face)

    if open_face in ("front", "front_and_ceiling"):
        label = "5 faces" if open_face == "front" else "4 faces"
        return head + (
            f"# {label}: open front for editor/VLM visibility.\n"
            "# room boundary is still enforced by Zone/AABB checks.\n"
        ) + common_faces

    common_faces = side_faces + top_face

    if not door:
        # 闭合：前墙整面一块（旧 6 面回退）
        front = f"usemtl {wall_mtl}\nf 5//1 8//1 7//1 6//1\n"   # front z=+0.5 inward -Z
        return head + (
            "# 6 faces (quads): each is a planar quad, normals inward.\n"
            "# verified by cross-product: 4 coplanar verts + inward normal.\n"
        ) + common_faces + front

    # 门洞：归一化到单位坐标，横向居中、底边贴地，留 5% 余量防止退化
    dw = max(0.05, min(0.9, float(door.get("width", 1.0)) / width))
    dh = max(0.05, min(0.9, float(door.get("height", 2.0)) / height))
    dl = -dw / 2.0          # door left  x
    dr = dw / 2.0           # door right x
    dt = -0.5 + dh          # door top   y（底边在 y=-0.5 地面）
    # 前墙新增 6 顶点（z=+0.5）：v9..v14
    extra_v = (
        "# front-wall door frame verts (z=+0.5)\n"
        f"v {dl:.4f} -0.5 0.5\n"    # v9  door bottom-left
        f"v {dr:.4f} -0.5 0.5\n"    # v10 door bottom-right
        f"v {dr:.4f} {dt:.4f} 0.5\n"  # v11 door top-right
        f"v {dl:.4f} {dt:.4f} 0.5\n"  # v12 door top-left
        f"v {dl:.4f} 0.5 0.5\n"     # v13 wall-top at door-left x
        f"v {dr:.4f} 0.5 0.5\n"     # v14 wall-top at door-right x
    )
    # 前墙拆 3 块（全在 z=+0.5，法向 -Z 内向，缠绕 bl->tl->tr->br）
    front_frame = (
        "# front wall as frame around door hole (normal -Z inward)\n"
        f"usemtl {wall_mtl}\n"
        "f 5//1 8//1 13//1 9//1\n"     # left strip   x in [-0.5, dl]
        "f 10//1 14//1 7//1 6//1\n"    # right strip  x in [dr, 0.5]
        "f 12//1 13//1 14//1 11//1\n"  # top lintel   y in [dt, 0.5]
    )
    return head + extra_v + (
        "# walls: back/left/right/bottom/top + front-frame (door hole).\n"
    ) + common_faces + front_frame


def _build_floor_obj(mtl_lib: str = "grass.mtl", mtl_name: str = "grass") -> str:
    """构建一块朝上的地面 quad（单位 1×1，XZ 平面，缩放由 Actor 完成）。

    M2 步骤 14b-ii：enclosure=terrain 的最简实现——一片平地，无墙无顶。
    M2 步骤 15c：引用独立 grass.mtl（草地绿），不再借用 box 的灰墙材质。
    M2 步骤 15b：材质参数化（mtl_lib/mtl_name），terrain 用草地、shell 内皮用地毯，共用此几何。
    法向 +Y（朝上），camera 站在上面。Actor 缩放成 [w, 1, d]、置于 y=0。
    """
    return (
        f"mtllib {mtl_lib}\nusemtl {mtl_name}\n"
        "# unit floor quad in XZ plane, normal +Y (up)\n"
        "v -0.5 0.0 -0.5\nv  0.5 0.0 -0.5\nv  0.5 0.0  0.5\nv -0.5 0.0  0.5\n"
        "vn  0.0  1.0  0.0\n"
        "f 1//1 4//1 3//1 2//1\n"   # top face, CCW from above → normal +Y up
    )


def _build_disc_obj(segments: int = 48, mtl_lib: str = "carpet.mtl",
                    mtl_name: str = "carpet") -> str:
    """构建一块朝上的圆盘 disc（单位半径 0.5，XZ 平面，法向 +Y）。

    M2：shell 主建筑（蒙古包）底是圆形，方形地板四角露在外面/不贴合——内皮地面
    改用圆盘，跟圆底建筑一致。三角扇：中心点 + 环上 segments 个点。
    单位半径 0.5（与 _build_floor_obj 同尺度，缩放语义一致：Actor scale=[w,1,d]
    → footprint 方形时 w≈d 得圆，非方形退化成椭圆，通用不写死圆）。
    """
    import math
    lines = [f"mtllib {mtl_lib}", f"usemtl {mtl_name}",
             f"# unit disc r=0.5 in XZ plane, normal +Y, segments={segments}",
             "vn 0.0 1.0 0.0",
             "v 0.0 0.0 0.0"]   # v1 = center
    seg = max(8, int(segments))
    for i in range(seg):
        a = 2.0 * math.pi * i / seg
        # +Y 朝上、从上看 CCW：x=cos, z=-sin（与 floor quad 同向）
        lines.append(f"v {0.5 * math.cos(a):.5f} 0.0 {-0.5 * math.sin(a):.5f}")
    # 扇形三角：中心(1) → 环点(2+i) → 环点(2+next)，CCW 保证法向 +Y
    for i in range(seg):
        v1 = 2 + i
        v2 = 2 + (i + 1) % seg
        lines.append(f"f 1//1 {v1}//1 {v2}//1")
    return "\n".join(lines) + "\n"


def _select_interior_floor_shape(width: float, depth: float,
                                 surface_params: Dict[str, Any] = None) -> str:
    """Choose shell interior floor mesh shape without scene-name inference."""
    params = dict(surface_params or {})
    explicit = str(params.get("floor_shape") or params.get("shape") or "").strip().lower()
    if explicit in ("disc", "round", "circle", "circular"):
        return "disc"
    if explicit in ("quad", "rect", "rectangle", "square"):
        return "quad"
    try:
        w = abs(float(width))
        d = abs(float(depth))
    except Exception:
        return "disc"
    if min(w, d) > 1e-6 and max(w, d) / min(w, d) >= 1.2:
        return "quad"
    return "disc"


def _float_param(params: Dict[str, Any], key: str, default: float) -> float:
    try:
        value = params.get(key, default)
        if value is None:
            return float(default)
        return float(value)
    except Exception:
        return float(default)


def _optional_float_param(params: Dict[str, Any], key: str):
    try:
        value = params.get(key)
        if value is None or value == "":
            return None
        return float(value)
    except Exception:
        return None


def _resolve_terrain_extent(width: float, depth: float, building_extent: float,
                            profile_params: Dict[str, Any]) -> Tuple[float, float, Dict[str, float]]:
    """Resolve terrain size from aspect params without scene-name inference."""
    params = dict(profile_params or {})
    extent_factor = _float_param(params, "extent_factor", 3.0)
    padding = _float_param(params, "padding", 0.0)
    min_extent = _optional_float_param(params, "min_extent")
    max_extent = _optional_float_param(params, "max_extent")

    base_extent = max(float(width or 0.0), float(depth or 0.0))
    if building_extent > 1e-6:
        base_extent = max(base_extent, float(building_extent) * max(0.0, extent_factor))
    extent = max(1.0, base_extent + padding)
    if min_extent is not None:
        extent = max(extent, min_extent)
    if max_extent is not None:
        extent = min(extent, max_extent)
    extent = max(1.0, extent)
    return extent, extent, {
        "extent_factor": float(extent_factor),
        "padding": float(padding),
        "min_extent": float(min_extent) if min_extent is not None else 0.0,
        "max_extent": float(max_extent) if max_extent is not None else 0.0,
        "building_extent": float(building_extent or 0.0),
        "extent": float(extent),
    }


def _terrain_height(x: float, z: float, profile, platform_radius: float) -> float:
    """通用高度场 h(x,z)：terrain zone 的参数化外皮（M2 步骤 15c-ii）。

    type 是参数不是 if 分支（代码不写 if 草原）。中心平台强制 h=0（保护建筑/家具落点），
    平台外按 type 起伏，过渡用 smoothstep 避免硬棱。确定性：seed 派生相位，不用 random。
    """
    import math
    r = math.hypot(x, z)
    if r <= platform_radius:
        return 0.0  # 中心平台：主建筑/地毯/家具落点全在 y=0，零回归

    typ = getattr(profile, "type", "flat") if profile is not None else "flat"
    amp = float(getattr(profile, "amplitude", 0.0) or 0.0) if profile is not None else 0.0
    freq = float(getattr(profile, "frequency", 1.0) or 1.0) if profile is not None else 1.0
    seed = float(getattr(profile, "seed", 0) or 0) if profile is not None else 0.0

    # 平台边缘 → 全起伏 的 smoothstep 过渡（带宽 ≈ 平台半径 0.4，至少 0.5m）
    band = max(0.5, platform_radius * 0.4)
    t = min(1.0, (r - platform_radius) / band)
    ramp = t * t * (3.0 - 2.0 * t)

    # seed → 确定性相位（fract(sin)，无随机源）
    s = math.sin(seed * 12.9898) * 43758.5453
    phase = (s - math.floor(s)) * 6.2831853

    if typ == "rolling":
        h = amp * (0.5 * math.sin(freq * x + phase) + 0.5 * math.sin(freq * z + phase * 1.3))
    elif typ == "dunes":
        h = amp * math.sin(freq * r + phase)
    elif typ == "terraced":
        ring = max(1.0, 1.0 / max(freq, 0.1))   # 每环宽度
        steps = math.floor((r - platform_radius) / ring)
        h = min(steps * max(0.2, amp), amp * 4.0)
    elif typ == "noise":
        h = amp * (0.4 * math.sin(freq * x + phase)
                   + 0.3 * math.sin(freq * 1.7 * z + phase)
                   + 0.3 * math.sin(freq * 2.3 * (x + z) + phase))
    else:  # flat 或未知 → 平
        h = 0.0
    # clamp 非负：平台(h=0)即全局地面 y=0，山丘只向上凸（不挖坑）。
    # 修"底面不对齐穿模"根因——terrain 抬地按"最低点→y=0"，若有负谷底，
    # 平台会被抬到 y≈+amp 而高于 shell/地毯/木栏(都吸附 y=0)，地形拱穿它们。
    # 非负后最低点恒为平台 → 平台落在 y=0，与所有元素共面对齐。
    return max(0.0, h * ramp)


def _terrain_detail_material(x: float, z: float, i: int, j: int, profile) -> str:
    """Choose terrain/detail material procedurally from params, not scene names."""
    import math
    pattern = str(getattr(profile, "detail_pattern", "none") or "none").strip().lower()
    strength = float(getattr(profile, "detail_strength", 0.0) or 0.0)
    if pattern == "none" or strength <= 0.0:
        return "terrain"
    if pattern == "paving_grid":
        return "terrain_detail" if (i % 3 == 0 or j % 3 == 0) else "terrain"
    if pattern == "radial_plaza":
        r = math.hypot(x, z)
        angle_band = int((math.atan2(z, x) + math.pi) / (math.pi / 8.0))
        return "terrain_detail" if (angle_band % 2 == 0 or int(r) % 4 == 0) else "terrain"
    if pattern == "cracked_stone":
        return "terrain_detail" if ((i * 7 + j * 11) % 13 in (0, 1)) else "terrain"
    if pattern == "noise_patches":
        v = _hash01(i * 101 + j * 37 + 17)
        return "terrain_detail" if v < min(0.55, max(0.05, strength)) else "terrain"
    return "terrain"


def _build_terrain_mesh_obj(width: float, depth: float, profile,
                            platform_radius: float, grid: int = 32,
                            mtl_lib: str = "grass.mtl", mtl_name: str = "grass") -> str:
    """构建带坡度的地形 grid mesh（世界坐标，M2 步骤 15c-ii）。

    纯函数，可离线验证（顶点共面性无意义，验法向朝上 + ASCII + 平台 h=0）。关键：
    顶点用【世界坐标】（x,z,h 真实米），Actor 不再缩放（scale=[1,1,1]）——否则 x/z
    缩放但 h 不缩放会比例错乱。中心 platform_radius 内 h=0（平台），平台外按 profile 起伏。
    """
    N = max(2, int(grid))
    hw, hd = width / 2.0, depth / 2.0
    lines = [f"mtllib {mtl_lib}", f"usemtl {mtl_name}",
             f"# terrain grid {N}x{N} world-coords type="
             f"{getattr(profile,'type','flat') if profile is not None else 'flat'} "
             f"platform_r={platform_radius:.2f}"]
    # 顶点 (N+1)×(N+1)，行优先（j 行 z，i 列 x）
    for j in range(N + 1):
        z = -hd + depth * j / N
        for i in range(N + 1):
            x = -hw + width * i / N
            y = _terrain_height(x, z, profile, platform_radius)
            lines.append(f"v {x:.3f} {y:.3f} {z:.3f}")
    lines.append("vn 0.0 1.0 0.0")  # 简化：统一上法向（坡度靠轮廓显形；不够再升顶点法向）

    def vid(i, j):
        return j * (N + 1) + i + 1   # 1-based

    # 每格两三角，缠绕保证 +Y 朝上（与 _build_floor_obj 同向）
    cur_mtl = mtl_name
    for j in range(N):
        for i in range(N):
            cx = -hw + width * (i + 0.5) / N
            cz = -hd + depth * (j + 0.5) / N
            mat = _terrain_detail_material(cx, cz, i, j, profile)
            if mat != cur_mtl:
                lines.append(f"usemtl {mat}")
                cur_mtl = mat
            a, b = vid(i, j), vid(i + 1, j)
            c, d = vid(i + 1, j + 1), vid(i, j + 1)
            lines.append(f"f {a}//1 {d}//1 {c}//1")
            lines.append(f"f {a}//1 {c}//1 {b}//1")
    return "\n".join(lines) + "\n"


def _hash01(n: int) -> float:
    """确定性伪随机 [0,1)：fract(sin(n)*K)。不用 random（resume 安全 + 可复现）。"""
    import math
    s = math.sin(n * 12.9898 + 78.233) * 43758.5453
    return s - math.floor(s)


def _build_grass_obj(width: float, depth: float, profile, platform_radius: float,
                     count: int = 160, mtl_lib: str = "grass_blade.mtl",
                     scatter: str = "grass") -> str:
    """散布地表覆盖物（草/花/岩石/灌木/碎屑/铺装标记）。

    纯函数，确定性散布（hash 自 index，不用 random）。每簇 = 交叉双竖直 quad（billboard），
    落在平台外（草不长在建筑下）、底部贴 _terrain_height（随坡起伏）。少量(~1/6)用花色。
    与坡度几何分离：geometry 是 15c-ii 的 terrain mesh，这是叠加的散布层。
    """
    import math
    hw, hd = width / 2.0, depth / 2.0
    scatter = (scatter or "grass").strip().lower()
    lines = [f"mtllib {mtl_lib}",
             f"# ground scatter kind={scatter} count={count} platform_r={platform_radius:.2f}"]
    vbase = 0          # 已写顶点数（face 索引用）
    faces = []
    placed = 0
    if scatter in ("rocks", "debris", "paving_marks"):
        BLADE_H = 0.08
        BLADE_W = 0.22
    elif scatter == "shrubs":
        BLADE_H = 0.55
        BLADE_W = 0.28
    else:
        BLADE_H = 0.35
        BLADE_W = 0.18
    attempts = 0
    idx = 0
    while placed < count and attempts < count * 4:
        attempts += 1
        idx += 1
        # 散布位置（确定性 hash）
        x = (_hash01(idx * 2 + 1) - 0.5) * width
        z = (_hash01(idx * 2 + 2) - 0.5) * depth
        # 平台内不长草（建筑脚下留空），留 0.3m 缓冲
        if math.hypot(x, z) <= platform_radius + 0.3:
            continue
        y = _terrain_height(x, z, profile, platform_radius)
        flower_ratio = 0.55 if scatter == "flowers" else 0.16
        is_flower = (_hash01(idx * 7 + 3) < flower_ratio)
        mat = "flower" if is_flower else "blade"
        # 交叉双 quad（billboard）：两片竖直面互相垂直，从任意角度都看得见
        # quad1 沿 X，quad2 沿 Z
        v = [
            (x - BLADE_W, y, z), (x + BLADE_W, y, z),
            (x + BLADE_W, y + BLADE_H, z), (x - BLADE_W, y + BLADE_H, z),
            (x, y, z - BLADE_W), (x, y, z + BLADE_W),
            (x, y + BLADE_H, z + BLADE_W), (x, y + BLADE_H, z - BLADE_W),
        ]
        lines.append(f"usemtl {mat}")
        for vx, vy, vz in v:
            lines.append(f"v {vx:.3f} {vy:.3f} {vz:.3f}")
        b = vbase
        # 双面（正反都画，billboard 无背面剔除问题）
        faces.append((mat, [b+1, b+2, b+3, b+4]))
        faces.append((mat, [b+4, b+3, b+2, b+1]))
        faces.append((mat, [b+5, b+6, b+7, b+8]))
        faces.append((mat, [b+8, b+7, b+6, b+5]))
        vbase += 8
        placed += 1
    # face 行（已按 usemtl 分组写在顶点前，这里统一补 f；OBJ 允许 f 在文件后段）
    cur_mat = None
    for mat, quad in faces:
        if mat != cur_mat:
            lines.append(f"usemtl {mat}")
            cur_mat = mat
        lines.append("f " + " ".join(str(i) for i in quad))
    return "\n".join(lines) + "\n"


def _terrain_mtl_text(material: str, secondary_material: str = "") -> str:
    """terrain material -> MTL text. 颜色保守，优先保证 demo 可读。"""
    palette = {
        "neutral": ("terrain", "0.24 0.24 0.22", "0.48 0.47 0.42"),
        "grass": ("terrain", "0.20 0.32 0.12", "0.36 0.55 0.22"),
        "dirt": ("terrain", "0.22 0.16 0.10", "0.45 0.34 0.22"),
        "sand": ("terrain", "0.36 0.31 0.20", "0.78 0.67 0.42"),
        "snow": ("terrain", "0.62 0.66 0.70", "0.86 0.90 0.92"),
        "stone": ("terrain", "0.20 0.20 0.19", "0.42 0.42 0.39"),
        "marble": ("terrain", "0.48 0.48 0.46", "0.78 0.76 0.70"),
        "concrete": ("terrain", "0.26 0.26 0.24", "0.52 0.51 0.47"),
        "pavement": ("terrain", "0.22 0.22 0.21", "0.46 0.45 0.42"),
        "slate": ("terrain", "0.16 0.17 0.18", "0.34 0.36 0.38"),
    }
    name, ka, kd = palette.get(material, palette["neutral"])
    _, ska, skd = palette.get(secondary_material or "", palette.get(material, palette["neutral"]))
    return (
        f"newmtl {name}\nKa {ka}\nKd {kd}\nKs 0.02 0.02 0.02\nNs 4.0\nd 1.0\n"
        f"newmtl terrain_detail\nKa {ska}\nKd {skd}\nKs 0.02 0.02 0.02\nNs 4.0\nd 1.0\n"
    )


def _scatter_mtl_text(scatter: str, material: str) -> str:
    """scatter style -> billboard material. OBJ 仍用 blade/flower 两材质名以复用几何。"""
    if scatter == "rocks":
        blade_ka, blade_kd = "0.16 0.15 0.14", "0.36 0.35 0.32"
        flower_ka, flower_kd = "0.22 0.20 0.18", "0.48 0.45 0.40"
    elif scatter == "debris":
        blade_ka, blade_kd = "0.18 0.16 0.13", "0.42 0.36 0.28"
        flower_ka, flower_kd = "0.12 0.11 0.10", "0.30 0.28 0.24"
    elif scatter == "paving_marks":
        blade_ka, blade_kd = "0.18 0.18 0.17", "0.62 0.60 0.54"
        flower_ka, flower_kd = "0.10 0.10 0.10", "0.36 0.35 0.33"
    elif scatter == "shrubs":
        blade_ka, blade_kd = "0.10 0.18 0.07", "0.24 0.38 0.16"
        flower_ka, flower_kd = "0.18 0.12 0.08", "0.42 0.30 0.18"
    elif scatter == "snow_patches":
        blade_ka, blade_kd = "0.62 0.66 0.70", "0.86 0.90 0.92"
        flower_ka, flower_kd = "0.50 0.54 0.58", "0.72 0.78 0.82"
    elif material == "sand":
        blade_ka, blade_kd = "0.24 0.22 0.14", "0.58 0.50 0.30"
        flower_ka, flower_kd = "0.18 0.20 0.10", "0.35 0.42 0.18"
    else:
        blade_ka, blade_kd = "0.10 0.22 0.06", "0.30 0.52 0.16"
        flower_ka, flower_kd = "0.30 0.10 0.20", "0.85 0.35 0.55"
    return (
        f"newmtl blade\nKa {blade_ka}\nKd {blade_kd}\n"
        "Ks 0.0 0.0 0.0\nNs 1.0\nd 1.0\n"
        f"newmtl flower\nKa {flower_ka}\nKd {flower_kd}\n"
        "Ks 0.0 0.0 0.0\nNs 1.0\nd 1.0\n"
    )


def _surface_mtl_text(material: str) -> str:
    """interior surface material -> MTL text. 缺省保持中性，不写死地毯。"""
    palette = {
        "neutral": ("0.28 0.27 0.24", "0.58 0.56 0.50"),
        "stone": ("0.22 0.22 0.21", "0.50 0.50 0.46"),
        "marble": ("0.48 0.48 0.46", "0.78 0.76 0.70"),
        "concrete": ("0.26 0.26 0.24", "0.52 0.51 0.47"),
        "pavement": ("0.22 0.22 0.21", "0.46 0.45 0.42"),
        "dirt": ("0.20 0.15 0.10", "0.44 0.32 0.20"),
        "wood": ("0.22 0.13 0.06", "0.50 0.32 0.16"),
        "carpet": ("0.20 0.10 0.08", "0.55 0.28 0.20"),
    }
    ka, kd = palette.get(material, palette["neutral"])
    return f"newmtl {material}\nKa {ka}\nKd {kd}\nKs 0.02 0.02 0.02\nNs 6.0\nd 1.0\n"


def _surface_palette(material: str) -> Tuple[str, str]:
    palette = {
        "neutral": ("0.28 0.27 0.24", "0.58 0.56 0.50"),
        "stone": ("0.22 0.22 0.21", "0.50 0.50 0.46"),
        "marble": ("0.48 0.48 0.46", "0.78 0.76 0.70"),
        "concrete": ("0.26 0.26 0.24", "0.52 0.51 0.47"),
        "pavement": ("0.22 0.22 0.21", "0.46 0.45 0.42"),
        "dirt": ("0.20 0.15 0.10", "0.44 0.32 0.20"),
        "wood": ("0.22 0.13 0.06", "0.50 0.32 0.16"),
        "carpet": ("0.20 0.10 0.08", "0.55 0.28 0.20"),
        "wallpaper": ("0.34 0.30 0.26", "0.68 0.60 0.52"),
        "plaster": ("0.42 0.40 0.36", "0.76 0.72 0.64"),
        "fabric": ("0.26 0.18 0.16", "0.56 0.38 0.34"),
    }
    return palette.get(str(material or "neutral"), palette["neutral"])


_SURFACE_MATERIAL_ALIASES = {
    "wood": "wood",
    "wooden": "wood",
    "timber": "wood",
    "木": "wood",
    "木质": "wood",
    "木地板": "wood",
    "stone": "stone",
    "rock": "stone",
    "slate": "stone",
    "石": "stone",
    "石材": "stone",
    "石板": "stone",
    "marble": "marble",
    "大理石": "marble",
    "concrete": "concrete",
    "cement": "concrete",
    "混凝土": "concrete",
    "pavement": "pavement",
    "paving": "pavement",
    "tile": "pavement",
    "tiles": "pavement",
    "砖": "pavement",
    "地砖": "pavement",
    "carpet": "carpet",
    "rug": "carpet",
    "地毯": "carpet",
    "dirt": "dirt",
    "earth": "dirt",
    "土": "dirt",
    "wallpaper": "wallpaper",
    "壁纸": "wallpaper",
    "plaster": "plaster",
    "灰泥": "plaster",
    "抹灰": "plaster",
    "fabric": "fabric",
    "布艺": "fabric",
}


def _canonical_surface_material(value: Any) -> str:
    text = str(value or "").strip().lower()
    if not text:
        return ""
    if text in _SURFACE_MATERIAL_ALIASES:
        return _SURFACE_MATERIAL_ALIASES[text]
    for key, material in _SURFACE_MATERIAL_ALIASES.items():
        if key and key in text:
            return material
    return text if text in {"neutral", "stone", "marble", "concrete", "pavement", "dirt", "wood", "carpet", "wallpaper", "plaster", "fabric"} else ""


def _style_material_tokens(style_context: Dict[str, Any]) -> List[str]:
    tokens: List[str] = []
    if not isinstance(style_context, dict):
        return tokens
    for key in ("material_palette", "materials", "surface_palette"):
        values = style_context.get(key)
        if isinstance(values, list):
            tokens.extend(str(v) for v in values)
        elif isinstance(values, str):
            tokens.append(values)
    for key in ("terrain_mood", "interior_mood", "style", "detail_pattern"):
        value = style_context.get(key)
        if isinstance(value, str):
            tokens.append(value)
    return tokens


def _derive_room_skin_materials(
    surface_params: Dict[str, Any],
    style_context: Optional[Dict[str, Any]] = None,
) -> Dict[str, str]:
    """Derive floor/wall/ceiling skin from open material params, not scene names."""

    params = dict(surface_params or {})
    style_context = style_context or {}
    candidates: List[str] = []
    for key in ("floor_material", "wall_material", "ceiling_material", "accent_material", "material"):
        value = params.get(key)
        if value:
            candidates.append(str(value))
    candidates.extend(_style_material_tokens(style_context))
    materials: List[str] = []
    for value in candidates:
        material = _canonical_surface_material(value)
        if material and material not in materials:
            materials.append(material)

    def explicit(key: str) -> str:
        return _canonical_surface_material(params.get(key))

    floor = explicit("floor_material")
    if not floor:
        for material in materials:
            if material in {"wood", "marble", "stone", "concrete", "pavement", "carpet", "dirt"}:
                floor = material
                break
    floor = floor or "neutral"

    wall = explicit("wall_material")
    if not wall:
        for material in materials:
            if material in {"wallpaper", "plaster", "fabric"}:
                wall = material
                break
    if not wall:
        for material in materials:
            if material in {"wood", "stone"}:
                wall = material
                break
    if not wall:
        if floor in {"marble", "stone", "concrete", "pavement"}:
            wall = "plaster"
        elif floor in {"wood", "carpet"}:
            wall = "wallpaper"
        elif floor == "dirt":
            wall = "wood"
        else:
            wall = "plaster"

    ceiling = explicit("ceiling_material")
    if not ceiling:
        for material in materials:
            if material in {"plaster", "fabric"}:
                ceiling = material
                break
    if not ceiling:
        ceiling = "plaster" if wall in {"wallpaper", "plaster", "fabric", "stone"} else ""
    if not ceiling:
        ceiling = "wood" if "wood" in materials else ""
    if not ceiling:
        ceiling = "plaster" if wall in {"wallpaper", "plaster", "fabric", "stone"} else "neutral"

    accent = explicit("accent_material")
    if not accent:
        for material in materials:
            if material not in {floor, wall, ceiling, "neutral"}:
                accent = material
                break

    return {
        "floor_material": floor,
        "wall_material": wall,
        "ceiling_material": ceiling,
        "accent_material": accent or wall,
    }


def _room_box_mtl_text(
    *,
    floor_material: str = "neutral",
    wall_material: str = "neutral",
    ceiling_material: str = "neutral",
    accent_material: str = "",
) -> str:
    """MTL for room skin: floor/walls/ceiling have separate material slots."""

    fka, fkd = _surface_palette(floor_material)
    wka, wkd = _surface_palette(wall_material)
    cka, ckd = _surface_palette(ceiling_material)
    aka, akd = _surface_palette(accent_material or wall_material or "neutral")
    return (
        f"newmtl floor\nKa {fka}\nKd {fkd}\nKs 0.02 0.02 0.02\nNs 6.0\nd 1.0\n"
        f"newmtl wall\nKa {wka}\nKd {wkd}\nKs 0.02 0.02 0.02\nNs 5.0\nd 1.0\n"
        f"newmtl ceiling\nKa {cka}\nKd {ckd}\nKs 0.01 0.01 0.01\nNs 3.0\nd 1.0\n"
        f"newmtl accent\nKa {aka}\nKd {akd}\nKs 0.02 0.02 0.02\nNs 5.0\nd 1.0\n"
    )


def _boundary_mtl_text(kind: str, material: str) -> str:
    """boundary material -> MTL text. kind/material 都来自 boundary aspect。"""
    key = (material or "").strip() or (kind or "").strip() or "neutral"
    palette = {
        "wood": ("0.18 0.10 0.05", "0.45 0.28 0.14"),
        "stone": ("0.20 0.20 0.19", "0.45 0.45 0.40"),
        "greenery": ("0.08 0.18 0.06", "0.22 0.42 0.16"),
        "hedge": ("0.08 0.18 0.06", "0.22 0.42 0.16"),
        "wall": ("0.20 0.20 0.19", "0.45 0.45 0.40"),
        "fence": ("0.18 0.10 0.05", "0.45 0.28 0.14"),
        "neutral": ("0.24 0.23 0.20", "0.46 0.44 0.38"),
    }
    ka, kd = palette.get(key, palette["neutral"])
    return f"newmtl boundary\nKa {ka}\nKd {kd}\nKs 0.02 0.02 0.02\nNs 4.0\nd 1.0\n"


def _build_fence_obj(platform_radius: float, gap_center_angle: float = 1.5708,
                     gap_half_angle: float = 0.5, mtl_lib: str = "fence.mtl",
                     kind: str = "fence", height: float = 1.1) -> str:
    """环形 boundary dressing。

    纯函数、世界坐标、确定性——跟 terrain/草簇同源，Actor scale=[1,1,1]。
    kind/material/height 来自 boundary aspect；没有 boundary aspect 时调用方不应调用它。
    """
    import math
    R = max(2.0, platform_radius + 1.2)   # 栏在平台外一圈
    N = 28                                # 木桩总数（含 gap 内被跳过的）
    boundary_kind = (kind or "fence").strip().lower()
    POST_H = max(0.25, float(height or 1.1))
    POST_T = 0.06                         # 桩半宽（细方柱）
    RAIL_T = 0.05                         # 横档半厚
    RAIL_YS = [POST_H * 0.36, POST_H * 0.77]
    step = 2.0 * math.pi / N

    lines = [f"mtllib {mtl_lib}", "usemtl boundary",
             f"# boundary kind={boundary_kind} R={R:.2f} gap_at={gap_center_angle:.2f} half={gap_half_angle:.2f} height={POST_H:.2f}"]
    verts, faces = [], []

    def add_box(cx, cy, cz, hx, hy, hz, dirx=1.0, dirz=0.0):
        """加一个盒子（中心 c，半尺寸 hx/hy/hz）；dir 为横档朝向（仅横档用，桩用默认）。"""
        # 桩=轴对齐方柱（dir 默认 +X）；横档=沿 dir 的细长盒，perp 为水平垂向
        px, pz = -dirz * hz, dirx * hz      # 垂直于 dir 的水平向（厚度方向）
        lx, lz = dirx * hx, dirz * hx       # 沿 dir 的长度向
        b = len(verts)
        for sy in (-hy, hy):
            verts.append((cx + lx + px, cy + sy, cz + lz + pz))
            verts.append((cx + lx - px, cy + sy, cz + lz - pz))
            verts.append((cx - lx - px, cy + sy, cz - lz - pz))
            verts.append((cx - lx + px, cy + sy, cz - lz + pz))
        # 6 面（底4 顶4 + 四壁），缠绕一致即可，背景物不严格剔除
        faces.append([b+1, b+2, b+3, b+4])
        faces.append([b+8, b+7, b+6, b+5])
        faces.append([b+1, b+5, b+6, b+2])
        faces.append([b+2, b+6, b+7, b+3])
        faces.append([b+3, b+7, b+8, b+4])
        faces.append([b+4, b+8, b+5, b+1])

    def in_gap(theta):
        d = abs(((theta - gap_center_angle + math.pi) % (2.0 * math.pi)) - math.pi)
        return d < gap_half_angle

    kept = []
    for i in range(N):
        theta = i * step
        cx, cz = R * math.cos(theta), R * math.sin(theta)
        if in_gap(theta):
            kept.append(None)
            continue
        kept.append((cx, cz))
        if boundary_kind == "fence":
            add_box(cx, POST_H / 2.0, cz, POST_T, POST_H / 2.0, POST_T)

    # 相邻保留点之间加几何段；跨 gap 的不连 → 自然留口。
    for i in range(N):
        a = kept[i]
        nb = kept[(i + 1) % N]
        if a is None or nb is None:
            continue
        ax, az = a
        bx, bz = nb
        mx, mz = (ax + bx) / 2.0, (az + bz) / 2.0
        dx, dz = bx - ax, bz - az
        seg = math.hypot(dx, dz)
        if seg < 1e-6:
            continue
        ux, uz = dx / seg, dz / seg
        if boundary_kind == "wall":
            add_box(mx, POST_H / 2.0, mz, seg / 2.0, POST_H / 2.0, 0.08, dirx=ux, dirz=uz)
        elif boundary_kind == "hedge":
            add_box(mx, POST_H / 2.0, mz, seg / 2.0, POST_H / 2.0, 0.14, dirx=ux, dirz=uz)
        else:
            for ry in RAIL_YS:
                add_box(mx, ry, mz, seg / 2.0, RAIL_T, RAIL_T, dirx=ux, dirz=uz)

    for vx, vy, vz in verts:
        lines.append(f"v {vx:.3f} {vy:.3f} {vz:.3f}")
    for quad in faces:
        lines.append("f " + " ".join(str(i) for i in quad))
    return "\n".join(lines) + "\n"


# M2 步骤 14b-ii：把任意场景描述分解成一棵 Zone 树。开放性在这一步——
# LLM 读 prompt 成空间结构，代码不枚举场景类型（不写 if 教堂/if 蒙古包）。
# 退化：纯室内 → 返回单 box（走旧路径，零回归）；只有真·室内外混合才建两层树。
_ZONE_DECOMPOSE_SYSTEM_PROMPT = """你是空间场景分解器。把用户的场景描述分解成一棵"空间区域(Zone)树"。
不要枚举场景类型，只忠实描述空间结构。

输出 JSON：
{
  "zones": [
    {
      "id": "z0",
	      "name": "区域名(中文)",
		      "role": "outdoor" | "indoor",
		      "enclosure": "terrain" | "box" | "shell",
		      "shell_asset": null | "建筑模型名(中文)",
		      "style_context": {
		        "main_building": "主建筑/核心对象",
		        "terrain_mood": "地形与环境气质",
		        "material_palette": ["wood", "grass", "stone"],
		        "functional_intent": "camp|courtyard|defense|ritual|research 等用途"
		      },
		      "aspects": [
		        {"capability": "ground_profile", "params": {"type": "flat|rolling|dunes|noise", "amplitude": 0.0, "frequency": 1.0, "material": "neutral", "secondary_material": "neutral", "extent_factor": 3.0, "min_extent": null, "max_extent": null, "padding": 0.0, "openness": 0.5, "detail_pattern": "none|paving_grid|radial_plaza|cracked_stone|noise_patches", "detail_strength": 0.0}},
		        {"capability": "ground_cover", "params": {"kind": "none|grass|flowers|rocks|shrubs|debris|paving_marks", "scatter": "none|grass|flowers|rocks|shrubs|debris|paving_marks", "density": 0.0}},
		        {"capability": "boundary", "params": {"kind": "fence|wall|hedge", "material": "wood|stone|greenery|neutral", "height": 1.1, "style": "边界外观提示", "radius": null, "margin": 1.0}},
	        {"capability": "interior_surface", "params": {"floor_material": "neutral|stone|wood|carpet", "floor_shape": "disc|quad", "wall_material": "neutral|stone|wood|wallpaper|plaster|fabric", "ceiling_material": "neutral|wood|plaster|concrete", "accent_material": "neutral|wood|stone|fabric", "detail_pattern": "none|paneling|trim|subtle_grid"}},
	        {"capability": "foundation_surface", "params": {"material": "neutral|stone|wood|carpet|dirt|marble", "shape": "auto|disc|quad", "padding": 0.6, "height_offset": 0.02, "follow_shell_footprint": true}},
	        {"capability": "entrance", "params": {"style": "door|curtain|archway", "hint": "入口生成提示"}},
	        {"capability": "shell_dressing", "params": {"asset_id": "建筑模型名", "style": "外观提示"}},
	        {"capability": "unsupported", "params": {"requested": "water_moat", "reason": "场景强烈需要但 manifest 未覆盖"}}
	      ],
	      "terrain_profile": null,
	      "size": [宽, 深, 高],
	      "parent": null | "父zone的id",
	      "has_door": true | false
	    }
	  ]
	}

字段说明：
- enclosure:
  * terrain = 开放平地(无墙无顶)
  * box = 合成的中性盒子(客厅/卧室/教堂内部这种"房间"，没有特定外观的建筑模型，由墙地顶围合)
  * shell = 由一个生成的建筑模型包裹(蒙古包/帐篷/小木屋这种有标志性外观的建筑，模型本身就是外壳)
- shell_asset: 仅当 enclosure=shell 时填，是那个建筑模型的名字(如"蒙古包")；其它情况填 null
- style_context: 规划层上下文，用于解释为什么选择某些 aspect params。代码不会按 main_building/terrain_mood 写 if；它只保存上下文供 GM/VLM/后续 prompt 使用。
  * main_building: 当前 zone 关联的主建筑或核心对象
  * terrain_mood: 地形/环境气质，如 open grassland、stone courtyard、volcanic observation site
  * material_palette: 推荐材质调性数组，如 wood/felt/grass、stone/slate、metal/concrete
  * functional_intent: 功能意图，如 camp、courtyard、defense、research、market
- aspects: 半开放能力切面。优先使用 manifest 中已有 capability：
  * ground_profile: 地形起伏/材质/范围，params 可含 type/amplitude/frequency/material/secondary_material/extent_factor/min_extent/max_extent/padding/openness/detail_pattern/detail_strength
  * ground_cover: 地表覆盖，params 可含 kind/density/scatter；只用于草、花、岩石、灌木、碎屑、铺装标记等覆盖/散布物，不用于 stone/marble/slate/pavement/tile/concrete 这类基础铺装材质
  * boundary: 边界物，params 可含 kind/material/height/style/radius/margin；没有 boundary aspect 时不要生成任何围栏/墙/绿篱
  * interior_surface: 内皮地面/墙面，params 可含 floor_material/floor_shape
  * foundation_surface: 主建筑外部基座/铺装垫层，params 可含 material/shape/padding/height_offset/follow_shell_footprint；它用于建筑底部与地形/广场衔接，不替代 interior_surface
  * entrance: 入口，params 可含 style/hint
  * shell_dressing: 建筑外壳，params 可含 asset_id/style
  * unsupported: 场景强烈需要但 manifest 未覆盖时使用，params 写 requested/reason；unsupported 不会执行
- size: 单位米 [宽, 深, 高]; terrain 的高填 0
- parent: 顶层 zone 填 null; 嵌套在某区域内填父 zone 的 id
- has_door: box 是否朝父区域开门洞(仅 box 有意义; shell 用模型自带入口, terrain 无, 都填 false)

规则：
- 纯室内房间（客厅/卧室/办公室/教堂内部）→ 1 个 box，role=indoor，parent=null，has_door=false，shell_asset=null。
- 室内外混合且内层是【标志性外观建筑】（草原上的蒙古包 / 院子里的小木屋 / 雪地里的帐篷）→ 2 个：
  外层 terrain(role=outdoor, parent=null) + 内层 shell(role=indoor, parent=外层id, shell_asset="建筑名", has_door=false)。
- 室内外混合但内层只是【普通房间】（院子里的一间客厅）→ 外层 terrain + 内层 box(has_door=true)。
- 纯室外（一片草原 / 广场，无可进入建筑）→ 1 个 terrain，role=outdoor，parent=null。
- 最多 2 层。内层(box/shell)是"人活动空间"(宽深 4~6 米、高 2.5~3 米)；外层 terrain 一大片(15~25 米)。
- 如果用户消息里包含 "RoleAgent 软偏好" 段落，它只表示角色风格倾向，不是用户新增物体清单；object_bias 里的物件不要自动加入 aspects 或 items，除非用户正文明确要求。
- 动态参数选择规则：根据用户需求、主建筑、地形气质、时代/文化/功能意图填写 aspects.params。
  * 需要边界时才输出 boundary；不需要边界时不要输出 boundary。
  * boundary 的 kind/material/height/style/radius/margin 必须与 style_context 和 zone size 一致，例如营地可低矮木栏，庭院可石墙；纯室外如需要控制围栏范围可给 radius，或用 margin 表示距 zone 边缘的内缩距离；研究/工业设施可 unsupported metal_railing。
  * ground_profile 的 extent_factor/min_extent/max_extent/padding/openness 控制地形大小；开放自然环境可更大，庭院/广场/建筑前场应更克制。不要让所有 terrain 都默认超大草原。
  * ground_profile 的 detail_pattern/detail_strength 用于程序化地表细节：石板网格、放射广场纹理、裂纹、岩石斑块、草色变化等。
  * ground_cover 的 kind/density/scatter 必须与 ground_profile.material 和 terrain_mood 一致；没有覆盖物需求时不要输出 ground_cover。铺装广场不要输出 grass/flowers。
  * entrance 的 style/hint 必须来自主建筑风格；不要把毡帘、拱门、木门当成全局默认。
  * interior_surface 的 floor_material/floor_shape 必须来自主建筑/功能意图；圆形/帐篷类可用 disc，矩形/教堂/房间类可用 quad；缺少明确风格时 material 用 neutral、shape 可省略。
- foundation_surface 用于主建筑外部基座。圆底/帐篷类可用 disc，矩形/教堂/房屋类可用 quad；材质应与地形/主建筑底部协调。
- 对重要普通物体可在 item/metadata 中标注 layout_role 或 scale_relation；喷泉/雕像/广场装饰应属于室外活动区，不要放进主建筑内部，除非用户明确要求。
- 草原蒙古包可输出 rolling/grass/fence/curtain/carpet/disc/foundation_surface 等 aspects；欧式教堂不要输出 grass/fence/curtain，室内地面可用 stone/quad，外部 foundation_surface 可用 stone 或 marble/quad。
- 火山口观测站如需要 lava_flow 且 manifest 不支持，应输出 unsupported，不要编造新 capability。
只输出 JSON，不要解释。"""


def _terrain_profile_from_spec(spec: Dict[str, Any], context_specs: List[Dict[str, Any]]):
    """兼容 legacy terrain_profile；缺失时只返回中性默认，不做场景关键词推断。"""
    from ..data_model.zone_tree import TerrainProfile

    raw = spec.get("terrain_profile") if isinstance(spec, dict) else None
    raw = raw if isinstance(raw, dict) else {}
    typ = str(raw.get("type") or "flat")
    material = str(raw.get("material") or "neutral")
    scatter = str(raw.get("scatter") or "none")
    style_tags = raw.get("style_tags") or []
    if not isinstance(style_tags, list):
        style_tags = [str(style_tags)]

    return TerrainProfile(
        type=typ,
        amplitude=float(raw.get("amplitude", 0.0)),
        frequency=float(raw.get("frequency", 1.0)),
        seed=int(raw.get("seed", 0)),
        material=material,
        scatter=scatter,
        style_tags=[str(t) for t in style_tags if str(t).strip()],
    )


def _normalize_aspect_dict(raw: Any):
    from ..data_model.zone_tree import CAPABILITY_MANIFEST, ZoneAspect

    if not isinstance(raw, dict):
        return None
    capability = str(raw.get("capability") or "").strip()
    params = raw.get("params") if isinstance(raw.get("params"), dict) else {}
    if not capability:
        return None
    if capability == "unsupported":
        return ZoneAspect(capability="unsupported", params=dict(params))
    if capability not in CAPABILITY_MANIFEST:
        return ZoneAspect(
            capability="unsupported",
            params={
                "requested": capability,
                "reason": "capability not in manifest",
                "params": dict(params),
            },
        )
    return ZoneAspect(capability=capability, params=dict(params))


_GROUND_SURFACE_MATERIALS = {
    "stone", "marble", "slate", "pavement", "paved", "tile", "tiles",
    "concrete", "brick", "cobblestone", "asphalt", "neutral",
    "石材", "石板", "大理石", "板岩", "铺装", "瓷砖", "地砖", "混凝土", "砖",
}


def _is_surface_material_cover(params: Dict[str, Any]) -> bool:
    """Return True when ground_cover is actually a base surface material."""
    kind = str(params.get("kind") or params.get("scatter") or "").strip().lower()
    if not kind:
        return False
    return kind in _GROUND_SURFACE_MATERIALS


def _looks_same_scene_asset(a: str, b: str) -> bool:
    """Conservative name match for shell asset de-duplication.

    Exact shell names are already handled. This catches cases like "欧式教堂"
    vs "教堂" without treating unrelated short props as duplicates.
    """
    left = re.sub(r"[\s_\-·,，。:：]+", "", str(a or "")).lower()
    right = re.sub(r"[\s_\-·,，。:：]+", "", str(b or "")).lower()
    if not left or not right:
        return False
    if left == right:
        return True
    min_len = min(len(left), len(right))
    return min_len >= 2 and (left in right or right in left)


def _add_legacy_aspect(zone, capability: str, params: Dict[str, Any]) -> None:
    from ..data_model.zone_tree import ZoneAspect

    if any(a.capability == capability for a in getattr(zone, "aspects", [])):
        return
    zone.aspects.append(ZoneAspect(capability=capability, params=dict(params)))


def normalize_zone_aspects(zone) -> None:
    """Normalize LLM aspects + legacy fields into zone.aspects.

    Explicit aspects win. This is the only place that reads legacy terrain_profile,
    interior_skin, primary_shell_asset_id, dressing_assets, and connector hints.
    """
    raw_aspects = list(getattr(zone, "metadata", {}).get("raw_aspects") or [])
    normalized = []
    for raw in raw_aspects:
        aspect = _normalize_aspect_dict(raw)
        if aspect is not None:
            normalized.append(aspect)
    # LLM 有时会把石板/大理石/铺装这类“基础地面材质”误放进
    # ground_cover，导致广场生成散布簇。这里把它收口到 ground_profile
    # 的 material，ground_cover 本身移除；草/花/雪/碎石等真正覆盖物不受影响。
    surface_cover_material = None
    cleaned = []
    for aspect in normalized:
        if aspect.capability == "ground_cover" and _is_surface_material_cover(aspect.params):
            surface_cover_material = str(
                aspect.params.get("kind") or aspect.params.get("scatter") or ""
            ).strip()
            logger.info("[SceneComposer] ground_cover=%s 识别为基础地面材质，转入 ground_profile.material",
                        surface_cover_material)
            continue
        cleaned.append(aspect)
    normalized = cleaned
    zone.aspects = normalized

    if getattr(zone, "primary_shell_asset_id", None):
        _add_legacy_aspect(
            zone,
            "shell_dressing",
            {"asset_id": zone.primary_shell_asset_id, "style": zone.name},
        )

    terrain_profile = getattr(zone, "terrain_profile", None)
    if terrain_profile is not None:
        terrain_material = terrain_profile.material
        if surface_cover_material and (not terrain_material or terrain_material == "neutral"):
            terrain_material = surface_cover_material
        _add_legacy_aspect(
            zone,
            "ground_profile",
            {
                "type": terrain_profile.type,
                "amplitude": terrain_profile.amplitude,
                "frequency": terrain_profile.frequency,
                "seed": terrain_profile.seed,
                "material": terrain_material,
                "extent_factor": getattr(terrain_profile, "extent_factor", 3.0),
            },
        )
        if getattr(terrain_profile, "scatter", "none") != "none":
            _add_legacy_aspect(
                zone,
                "ground_cover",
                {
                    "kind": terrain_profile.scatter,
                    "scatter": terrain_profile.scatter,
                    "density": 1.0,
                },
            )

    skin = getattr(zone, "interior_skin", None)
    if skin is not None:
        _add_legacy_aspect(
            zone,
            "interior_surface",
            {"floor_material": getattr(skin, "floor_material", "neutral")},
        )

    if any(getattr(c, "type", "") == "door" for c in getattr(zone, "connectors", []) or []):
        _add_legacy_aspect(zone, "entrance", {"style": "door"})


def apply_scene_semantic_terrain_profile(zone, scene_text: str, scene_type: str = "") -> None:
    """Apply conservative scene-derived terrain/boundary defaults.

    Explicit LLM aspects still win; this only fills gaps for known F5 scene
    classes where the old generic boundary made the scene look wrong.
    """
    try:
        from plugins.AITool.services.terrain_component_resolver import TerrainComponentResolver
    except Exception:  # noqa: BLE001
        try:
            from services.terrain_component_resolver import TerrainComponentResolver  # type: ignore
        except Exception:
            return
    profile = TerrainComponentResolver().derive(scene_text, scene_type=scene_type)
    if not _has_aspect(zone, "ground_profile"):
        terrain = dict(profile.terrain_spec)
        material = str(terrain.get("surface") or "neutral")
        _add_legacy_aspect(
            zone,
            "ground_profile",
            {
                "type": "flat",
                "material": material,
                "detail_pattern": terrain.get("detail_pattern", "none"),
                "openness": 0.65,
            },
        )
    boundary = dict(profile.boundary_spec)
    if boundary.get("type") in {"low_decorative_boundary", "camp_boundary"}:
        existing_boundary = _aspect(zone, "boundary")
        should_override_generic = (
            profile.scene_key == "fantasy_night_market"
            and existing_boundary is not None
            and str(getattr(existing_boundary, "params", {}).get("kind") or "").lower() in {"", "wall", "fence"}
            and str(getattr(existing_boundary, "params", {}).get("material") or "").lower() in {"", "stone", "wood"}
        )
        boundary_params = {
            "kind": boundary.get("kind", "fence"),
            "material": boundary.get("material", "wood"),
            "height": boundary.get("height", 0.8),
            "style": boundary.get("style", ""),
            "coverage": boundary.get("coverage", "partial"),
            "shape": boundary.get("shape", ""),
        }
        if existing_boundary is not None and should_override_generic:
            existing_boundary.params.update(boundary_params)
            logger.info(
                "[SceneComposer] semantic terrain profile overrides generic boundary: scene=%s params=%s",
                profile.scene_key,
                boundary_params,
            )
        elif not _has_aspect(zone, "boundary"):
            _add_legacy_aspect(
                zone,
                "boundary",
                boundary_params,
            )


def _aspect(zone, capability: str):
    for aspect in getattr(zone, "aspects", []) or []:
        if aspect.capability == capability:
            return aspect
    return None


def _aspect_params(zone, capability: str) -> Dict[str, Any]:
    aspect = _aspect(zone, capability)
    return dict(getattr(aspect, "params", {}) or {}) if aspect else {}


def _has_aspect(zone, capability: str) -> bool:
    return _aspect(zone, capability) is not None


def resolve_zone_anchor(composer, zone=None, capability: str = "boundary",
                        params: Dict[str, Any] = None) -> Dict[str, Any]:
    """Resolve the geometric anchor used by open-scene generators.

    Priority is measured shell footprint, generated terrain platform, then the
    abstract zone volume. The function is deliberately scene-name agnostic so
    pure outdoor zones can generate boundaries without a shell AABB.
    """
    params = dict(params or {})
    zone_id = getattr(zone, "zone_id", None)
    center = [0.0, 0.0, 0.0]
    volume = getattr(zone, "volume", None)
    if volume is not None:
        raw_center = getattr(volume, "center", None) or center
        if len(raw_center) >= 3:
            center = [float(raw_center[0]), float(raw_center[1]), float(raw_center[2])]

    shell_aabbs = getattr(composer, "_shell_aabb", {}) or {}
    shell_aabb = shell_aabbs.get(zone_id) if zone_id else None
    if shell_aabb is None and shell_aabbs:
        shell_aabb = max(
            shell_aabbs.values(),
            key=lambda a: max(
                float(a.get("half_x", 0.0) or 0.0),
                float(a.get("half_z", 0.0) or 0.0),
            ),
        )
    if shell_aabb is not None:
        shell_r = max(
            float(shell_aabb.get("half_x", 0.0) or 0.0),
            float(shell_aabb.get("half_z", 0.0) or 0.0),
        )
        if shell_r > 1e-6:
            return {
                "anchor_type": "shell",
                "capability": capability,
                "zone_id": zone_id,
                "center": [
                    float(shell_aabb.get("center_x", shell_aabb.get("x", 0.0)) or 0.0),
                    float(shell_aabb.get("center_y", shell_aabb.get("y", 0.0)) or 0.0),
                    float(shell_aabb.get("center_z", shell_aabb.get("z", 0.0)) or 0.0),
                ],
                "ring_radius": shell_r * 1.15 + 0.5,
                "half_x": float(shell_aabb.get("half_x", shell_r) or shell_r),
                "half_z": float(shell_aabb.get("half_z", shell_r) or shell_r),
            }

    platform_radius = float(getattr(composer, "_platform_radius", 0.0) or 0.0)
    if platform_radius > 1e-6:
        return {
            "anchor_type": "platform",
            "capability": capability,
            "zone_id": zone_id,
            "center": center,
            "ring_radius": platform_radius * 1.05 + 0.5,
            "half_x": platform_radius,
            "half_z": platform_radius,
        }

    size = getattr(volume, "size", None) if volume is not None else None
    if size and len(size) >= 2:
        width = float(size[0] or 0.0)
        depth = float(size[1] or 0.0)
        if width > 1e-6 and depth > 1e-6:
            try:
                explicit_radius = float(params.get("radius", 0.0) or 0.0)
            except Exception:
                explicit_radius = 0.0
            if explicit_radius > 1e-6:
                ring_r = explicit_radius
            else:
                try:
                    margin = float(params.get("margin", 1.0) or 1.0)
                except Exception:
                    margin = 1.0
                ring_r = max(2.0, min(width, depth) / 2.0 - max(0.0, margin))
            return {
                "anchor_type": "zone_volume",
                "capability": capability,
                "zone_id": zone_id,
                "center": center,
                "ring_radius": ring_r,
                "half_x": width / 2.0,
                "half_z": depth / 2.0,
            }

    return {
        "anchor_type": "none",
        "capability": capability,
        "zone_id": zone_id,
        "center": center,
        "ring_radius": 0.0,
        "half_x": 0.0,
        "half_z": 0.0,
    }


def _shell_generation_hint(zone) -> str:
    """Build shell prompt hint from aspects; no entrance aspect means no door/curtain bias."""
    hints = ["完整建筑外观，入口与建筑风格一体，不要生成剖面或露出内部"]
    shell_params = _aspect_params(zone, "shell_dressing")
    entrance_params = _aspect_params(zone, "entrance")
    style_context = getattr(zone, "style_context", {}) or {}
    style = str(shell_params.get("style") or "").strip()
    if style:
        hints.append(style)
    palette = style_context.get("material_palette")
    if isinstance(palette, list) and palette:
        hints.append("材质调性: " + ", ".join(str(p) for p in palette[:4] if str(p).strip()))
    terrain_mood = str(style_context.get("terrain_mood") or "").strip()
    if terrain_mood:
        hints.append("环境气质: " + terrain_mood)
    if entrance_params:
        entrance_hint = str(entrance_params.get("hint") or "").strip()
        entrance_style = str(entrance_params.get("style") or "").strip()
        if entrance_hint:
            hints.append(entrance_hint)
        elif entrance_style:
            hints.append(f"入口形式: {entrance_style}")
    return "，".join(hints)



# 触发场景组合的关键词
_COMPOSE_PATTERNS = [
    r"物品清单", r"清单", r"组合(?:场景|这个|整个)", r"布置(?:整个|这个|好这)",
    r"根据.{0,10}(?:方案|清单|设计|效果图).{0,6}(?:生成|布置|组合|搭建)",
    r"按.{0,8}清单",
    r"把.{0,10}(?:都|全部|所有).{0,6}(?:生成|放|布置|导入)",
    r"一键(?:生成|布置|组合)",
    # 直接生成类：'生成欧式卧室' / '生成一个现代客厅' 等
    r"生成.{0,9}(?:卧室|客厅|厨房|书房|房间|浴室|场景|办公室|餐厅)",
]

_EXTRACT_SYSTEM_PROMPT = """你是场景物品清单解析器。从用户文字中提取需要放入 3D 场景的【主要独立家具/物件】。
输出 JSON 数组，每项: {"name":"物体名","quantity":数量,"keywords":"英文关键词(用于3D生成prompt)"}
规则:
- 只提取能独立建模、有体积的【大件物体】：如 床、衣柜、沙发、桌、椅、柜、台灯、地毯、绿植、挂画、镜子等
- 必须合并/忽略以下琐碎项，不要单独列出：
  * 床的附属：床垫、被子、枕头、靠枕、床旗、床品 → 都并入"床"，不单列
  * 墙面/背景：背景墙、软包、护墙板、石膏线、墙面 → 不是物体，忽略
  * 建筑设施：空调出风口、新风口、筒灯、灯带、顶灯（嵌入式）→ 忽略
  * 小件杂物：收纳篮、收纳盒、护肤品 → 忽略
  * 同类合并：左右床头柜=1种"床头柜"(quantity:2)，台灯/壁灯择一
- 忽略所有尺寸/颜色/风格/材质描述
- name 用简洁中文（2-6字），quantity 默认1
- keywords 给出适合 3D 生成的英文描述
- 控制在 9 个以内，只保留最重要的大件
只输出 JSON 数组，不要解释。"""

# 不该单独建模的物体（关键词黑名单，双保险过滤 LLM 漏网项）
_ITEM_BLACKLIST = [
    # 床品/附属
    "床垫", "被子", "被褥", "枕头", "靠枕", "抱枕", "床旗", "床品", "床单", "被",
    # 墙面/天花板
    "背景墙", "软包", "护墙板", "石膏线", "墙面", "墙", "天花", "踢脚线",
    # 建筑设施 + 天花板物品
    "空调", "新风", "出风口", "筒灯", "灯带", "顶灯", "嵌灯", "吊灯", "吸顶灯",
    # 墙面附着物（应在盒子上，不单独建模）
    "窗帘", "壁画", "挂画", "装饰画", "卷帘", "百叶",
    # 杂物
    "收纳篮", "收纳盒", "收纳", "护肤品", "摆件杂物",
]


def _is_blacklisted(name: str) -> bool:
    """判断物体名是否在黑名单（不该单独建模的琐碎/附属/建筑项）。"""
    n = (name or "").strip()
    if not n:
        return True
    return any(bad in n for bad in _ITEM_BLACKLIST)


def is_compose_request(text: str) -> bool:
    """判断是否为场景组合类请求。"""
    t = (text or "").strip()
    if not t:
        return False
    # 含数字列表（如 "1. xx 2. xx"）也视为清单
    if len(re.findall(r"^\s*\d+[\.、)]", t, re.MULTILINE)) >= 3:
        return True
    for pat in _COMPOSE_PATTERNS:
        if re.search(pat, t):
            return True
    return False


_RESOLVED_PLAN_MARKERS = (
    "用户确认执行 @",
    "最近方案",
    "严格围绕下列方案",
    "resolved_intent_text",
)

_GENERIC_INVENTORY_NAMES = {
    "现代主体建筑",
    "入口门厅",
    "铺装广场",
    "指示牌",
    "休息长椅",
    "景观花池",
    "低矮围栏",
    "入口台阶",
    "功能支撑物件",
    "导视牌",
    "灯光装饰",
    "储物道具",
    "活动区装饰",
    "小型展示物",
}


def _has_resolved_plan_context(text: str) -> bool:
    return any(marker in str(text or "") for marker in _RESOLVED_PLAN_MARKERS)


def _looks_generic_inventory(items: List[Dict[str, Any]]) -> bool:
    names = [str(item.get("name") or "").strip() for item in items]
    if not names:
        return False
    generic_hits = sum(1 for name in names if name in _GENERIC_INVENTORY_NAMES)
    return generic_hits >= 3 or generic_hits >= max(1, len(names) - 1)


class SceneComposer:
    """场景组合器。"""

    # 单次场景生成的物体数量上限（防止一次生成过多 3D 模型，耗时/占用过大）
    DEFAULT_MAX_ITEMS = 8
    _INDOOR_ROOM_MIN_SIZE = [5.0, 5.0, 3.0]
    _INDOOR_ROOM_MAX_SIZE = [8.5, 8.5, 3.4]
    _LARGE_ROOM_ITEM_TERMS = (
        "床", "沙发", "衣柜", "柜", "书桌", "桌", "长桌", "餐桌", "大件", "大型",
        "bed", "sofa", "cabinet", "wardrobe", "table", "desk", "large",
    )
    _INDOOR_ITEM_FOOTPRINTS = (
        (("床", "bed"), (2.2, 1.8, 1.1)),
        (("沙发", "sofa"), (2.0, 1.0, 1.0)),
        (("衣柜", "wardrobe"), (1.6, 0.7, 2.2)),
        (("书柜", "书架", "bookshelf"), (1.2, 0.45, 2.0)),
        (("柜", "cabinet"), (1.2, 0.55, 1.6)),
        (("长桌", "餐桌", "table"), (1.7, 0.9, 0.8)),
        (("书桌", "desk"), (1.4, 0.75, 0.8)),
        (("椅", "chair"), (0.65, 0.65, 0.9)),
        (("箱", "宝箱", "木箱", "chest", "box"), (0.9, 0.65, 0.7)),
        (("酒桶", "桶", "barrel"), (0.7, 0.7, 1.0)),
        (("武器架", "架", "rack"), (1.1, 0.45, 1.8)),
        (("雕像", "statue"), (0.9, 0.9, 1.8)),
        (("地毯", "rug", "carpet"), (1.6, 1.2, 0.05)),
        (("灯", "台灯", "火把", "烛台", "lamp", "torch"), (0.35, 0.35, 1.2)),
    )
    _IMAGE_TO_3D_PROMPT_GUARDRAIL = (
        "single standalone physical 3D object, centered full object, plain white background, "
        "no text, no labels, no watermark, not a flat poster, not a texture sheet, "
        "not a card, not a billboard, visible thickness and depth"
    )

    def __init__(self, room_size: List[float] = None, scene_name: str = "lanchat_scene",
                 max_items: int = DEFAULT_MAX_ITEMS, zone_tree=None) -> None:
        self.room_size = room_size or [5.0, 3.0, 3.0]
        self.scene_name = scene_name
        self.max_items = max(1, int(max_items))
        self._provider = None
        # M2 步骤 14a：ZoneTree（可选）。为 None 时退化成单 Zone + enclosure=box，
        # 几何与旧 room_size 逻辑完全一致，零功能损失。
        self.zone_tree = zone_tree
        self._last_zone_decompose_snapshot = None
        self._last_zone_decompose_spec = None
        self._last_zone_decompose_text = ""
        self._fallback_room_aspects = []
        self._fallback_room_style_context = {}
        self._fallback_room_zone_spec = {}
        self._last_element_classification = []
        self._last_element_classification_summary = ""
        self._generated_asset_abs_dir = None
        self._generated_asset_rel_dir = None
        self._last_room_budget_summary = {}
        self._last_indoor_bounds_report = {}

    def _generated_asset_dir(self) -> Tuple[Path, str]:
        if self._generated_asset_abs_dir is not None and self._generated_asset_rel_dir:
            return self._generated_asset_abs_dir, self._generated_asset_rel_dir

        try:
            from Quasar.ai_config.paths_config import _get_active_project_path
            project_root = Path(_get_active_project_path()).resolve()
        except Exception:
            project_root = Path(os.getcwd()).resolve()

        scene_slug = _safe_generated_asset_name(self.scene_name, "scene")
        unique = f"{scene_slug}_{int(time.time() * 1000)}_{os.getpid()}_{uuid.uuid4().hex[:8]}"
        abs_dir = project_root / "Resource" / "generated" / "scene_composer" / unique
        abs_dir.mkdir(parents=True, exist_ok=True)
        rel_dir = abs_dir.relative_to(project_root).as_posix()
        self._generated_asset_abs_dir = abs_dir
        self._generated_asset_rel_dir = rel_dir
        return abs_dir, rel_dir

    def _get_room_zone(self):
        """返回用于"物体布局"的 Zone（物体摆进它的体积里）。

        无 zone_tree 时按 room_size 构造默认单 Zone（退化形态）。
        有 zone_tree 时返回第一个 indoor/box/shell Zone（物体进室内），无则 root。
        注意：indoor 盒子恒在原点（center=[0,h/2,0]），所以现有"原点布局 + room_size"
        逻辑无需改动就落在盒内——这是 14b-ii 不动布局代码的关键。
        """
        if self.zone_tree is not None and self.zone_tree.root is not None:
            for z in self.zone_tree.list_all_zones():
                if (z.enclosure or "") in ("box", "shell") or z.role == "indoor":
                    return z
            return self.zone_tree.root
        # 退化：构造默认单 Zone（center/size 与旧 _generate_room_box 完全一致）
        from ..data_model.zone_tree import Zone, Volume
        w, d, h = self.room_size[0], self.room_size[1], self.room_size[2]
        fallback_spec = dict(getattr(self, "_fallback_room_zone_spec", {}) or {})
        zone = Zone(
            zone_id=str(fallback_spec.get("zone_id") or "zone_root"),
            name=str(fallback_spec.get("name") or self.scene_name),
            role="indoor",
            volume=Volume(center=[0.0, h / 2.0, 0.0], size=[w, d, h]),
            enclosure="box",
        )
        zone.metadata.update(dict(fallback_spec.get("metadata") or {}))
        zone.metadata["raw_aspects"] = list(getattr(self, "_fallback_room_aspects", []) or [])
        zone.style_context = dict(getattr(self, "_fallback_room_style_context", {}) or {})
        normalize_zone_aspects(zone)
        return zone

    @classmethod
    def _build_model_image_prompt(cls, name: str, keywords: str = "") -> str:
        base = (keywords or "").strip()
        if not base or len(base) <= 6:
            base = (
                f"high quality 3D model of {name}, standalone, white background, "
                f"photorealistic, product photography, {name}"
            )
        guardrail = cls._IMAGE_TO_3D_PROMPT_GUARDRAIL
        lower = base.lower()
        if "not a flat poster" not in lower:
            base = f"{base}, {guardrail}"
        return base

    @classmethod
    def _estimate_indoor_room_size(
        cls,
        items: List[Dict[str, Any]],
        base_size: Optional[List[float]] = None,
    ) -> List[float]:
        current = list(base_size or [5.0, 3.0, 3.0])
        while len(current) < 3:
            current.append(cls._INDOOR_ROOM_MIN_SIZE[len(current)])
        width = max(float(current[0]), cls._INDOOR_ROOM_MIN_SIZE[0])
        depth = max(float(current[1]), cls._INDOOR_ROOM_MIN_SIZE[1])
        height = max(float(current[2]), cls._INDOOR_ROOM_MIN_SIZE[2])

        footprints = [cls._indoor_item_footprint(item) for item in (items or [])]
        count = len(footprints)
        total_area = sum(max(0.15, fp[0] * fp[1]) for fp in footprints)
        max_item_w = max([fp[0] for fp in footprints] + [1.0])
        max_item_d = max([fp[1] for fp in footprints] + [1.0])
        max_item_h = max([fp[2] for fp in footprints] + [1.0])
        count_pressure = 5.5 + max(0, count - 4) * 0.35
        area_span = (max(1.0, total_area) * 1.35) ** 0.5 + 1.2
        target_width = max(cls._INDOOR_ROOM_MIN_SIZE[0], count_pressure, area_span, max_item_w + 2.2)
        target_depth = max(cls._INDOOR_ROOM_MIN_SIZE[1], count_pressure, area_span, max_item_d + 2.2)
        width = max(width, target_width)
        depth = max(depth, target_depth)

        names = " ".join(str((it or {}).get("name") or "") for it in (items or [])).lower()
        has_large_item = any(term.lower() in names for term in cls._LARGE_ROOM_ITEM_TERMS)
        if has_large_item:
            width = max(width, max_item_w + 2.8)
            depth = max(depth, max_item_d + 2.8)
            height = max(height, 3.2)
        height = max(height, min(cls._INDOOR_ROOM_MAX_SIZE[2], max_item_h + 0.8))

        max_width = max(cls._INDOOR_ROOM_MAX_SIZE[0], float(current[0]))
        max_depth = max(cls._INDOOR_ROOM_MAX_SIZE[1], float(current[1]))
        max_height = max(cls._INDOOR_ROOM_MAX_SIZE[2], float(current[2]))

        return [
            round(min(width, max_width), 2),
            round(min(depth, max_depth), 2),
            round(min(height, max_height), 2),
        ]

    @classmethod
    def _indoor_item_footprint(cls, item: Dict[str, Any]) -> tuple[float, float, float]:
        data = item or {}
        for key in ("size", "dimensions", "aabb_size"):
            size = data.get(key)
            if isinstance(size, (list, tuple)) and len(size) >= 3:
                try:
                    return (
                        max(0.1, abs(float(size[0]))),
                        max(0.1, abs(float(size[2]))),
                        max(0.1, abs(float(size[1]))),
                    )
                except Exception:
                    pass
        half = data.get("half_extents") or data.get("half_extent")
        if isinstance(half, (list, tuple)) and len(half) >= 3:
            try:
                return (
                    max(0.1, abs(float(half[0])) * 2.0),
                    max(0.1, abs(float(half[2])) * 2.0),
                    max(0.1, abs(float(half[1])) * 2.0),
                )
            except Exception:
                pass
        name = str(data.get("name") or data.get("label") or "").lower()
        for keywords, footprint in cls._INDOOR_ITEM_FOOTPRINTS:
            if any(word.lower() in name for word in keywords):
                return footprint
        return (0.65, 0.65, 0.8)

    def _apply_indoor_room_budget(
        self,
        items: List[Dict[str, Any]],
        prompt: str = "",
        progress_sink: Optional[Any] = None,
    ) -> None:
        if not self._detect_scene_indoor(prompt):
            self._last_room_budget_summary = {}
            return

        target_zone = None
        if self.zone_tree is not None and self.zone_tree.root is not None:
            for zone in self.zone_tree.list_all_zones():
                if (getattr(zone, "enclosure", "") or "") == "box":
                    target_zone = zone
                    break
            if target_zone is None:
                self._last_room_budget_summary = {}
                return

        base_size = (
            list(getattr(target_zone.volume, "size", []) or [])
            if target_zone is not None else list(self.room_size)
        )
        target_size = self._estimate_indoor_room_size(items, base_size)
        old_size = list(base_size)
        changed = any(abs(float(target_size[i]) - float(old_size[i])) > 1e-6 for i in range(3))
        if target_zone is not None:
            target_zone.volume.size = list(target_size)
            target_zone.volume.center = [0.0, target_size[2] / 2.0, 0.0]
        self.room_size = list(target_size)
        self._last_room_budget_summary = {
            "room_size": list(target_size),
            "previous_size": old_size,
            "item_count": len(items or []),
            "changed": changed,
        }
        if changed:
            logger.info(
                "[SceneComposer] 室内空间预算: %s -> %s items=%s",
                old_size,
                target_size,
                len(items or []),
            )
            if progress_sink:
                try:
                    progress_sink(
                        f"室内空间预算：房间尺寸 {target_size[0]:.1f} x {target_size[1]:.1f} x {target_size[2]:.1f}，后续会做边界检查。"
                    )
                except Exception:  # noqa: BLE001
                    pass

    def decompose_zone_tree(self, text: str):
        """M2 步骤 14b-ii：把场景描述分解成 ZoneTree。开放性在这一步（LLM 读结构）。

        返回 ZoneTree 或 None：
        - None → 纯室内单房间（走旧 _generate_room_box 单盒路径，零回归）
        - ZoneTree → 真·室内外混合（terrain + box + 门洞）或纯室外（单 terrain）

        代码不枚举场景类型——分解判断全在 LLM，本函数只把 LLM 输出实例化成树。
        失败/格式错误 → 返回 None（保守退化到单盒，不影响主链路）。
        """
        zones_spec = self._llm_decompose(text)
        if not zones_spec and self._looks_like_outdoor_with_indoor_structure(text):
            zones_spec = self._fallback_mixed_zone_specs(text)
        elif zones_spec and self._looks_like_outdoor_with_indoor_structure(text):
            zones_spec = self._ensure_mixed_zone_specs(zones_spec, text)
        if not zones_spec:
            return None
        self._last_zone_decompose_text = str(text or "")
        self._last_zone_decompose_spec = zones_spec
        try:
            tree = self._build_zone_tree(zones_spec)
        except Exception as e:
            logger.warning("[SceneComposer] ZoneTree 构建失败，退化单盒: %s", e)
            return None
        if tree is None:
            return None
        # 纯单室内盒（1 个 box、无 terrain、无门洞）→ 等价旧路径，返回 None 省一层
        zones = tree.list_all_zones()
        if len(zones) == 1 and zones[0].enclosure == "box" and not zones[0].connectors:
            self._fallback_room_aspects = list(getattr(zones[0], "metadata", {}).get("raw_aspects") or [])
            self._fallback_room_style_context = dict(getattr(zones[0], "style_context", {}) or {})
            size = list(getattr(getattr(zones[0], "volume", None), "size", []) or [])
            if len(size) >= 3:
                self.room_size = [float(size[0]), float(size[1]), float(size[2])]
            self._fallback_room_zone_spec = {
                "zone_id": getattr(zones[0], "zone_id", "") or "zone_root",
                "name": getattr(zones[0], "name", "") or self.scene_name,
                "role": getattr(zones[0], "role", "") or "indoor",
                "enclosure": "box",
                "metadata": dict(getattr(zones[0], "metadata", {}) or {}),
                "size": list(self.room_size),
            }
            self._save_zone_decompose_snapshot(text, zones_spec, tree)
            return None
        logger.info("[SceneComposer] 场景分解为 %d 个 Zone: %s",
                    len(zones), [f"{z.name}({z.enclosure})" for z in zones])
        self._save_zone_decompose_snapshot(text, zones_spec, tree)
        return tree

    def _save_zone_decompose_snapshot(self, text: str, zones_spec: List[Dict[str, Any]], tree) -> None:
        """Persist the latest decompose JSON for F5 review without touching repo files."""
        import tempfile as _tf, time as _time

        try:
            zones = []
            if tree is not None:
                for z in tree.list_all_zones():
                    zones.append({
                        "zone_id": z.zone_id,
                        "name": z.name,
                        "role": z.role,
                        "enclosure": z.enclosure,
                        "volume": {
                            "center": list(getattr(z.volume, "center", []) or []),
                            "size": list(getattr(z.volume, "size", []) or []),
                        },
                        "primary_shell_asset_id": z.primary_shell_asset_id,
                        "style_context": dict(getattr(z, "style_context", {}) or {}),
                        "aspects": [
                            {
                                "capability": a.capability,
                                "params": dict(getattr(a, "params", {}) or {}),
                            }
                            for a in (getattr(z, "aspects", []) or [])
                        ],
                        "parent": z.metadata.get("parent"),
                    })
            payload = {
                "prompt": text,
                "raw_zones": zones_spec,
                "normalized_zones": zones,
            }
            out_dir = os.path.join(_tf.gettempdir(), "corona_m2_f5_decompose")
            os.makedirs(out_dir, exist_ok=True)
            safe_scene = re.sub(r"[^A-Za-z0-9_.-]+", "_", self.scene_name or "scene")[:48]
            out_path = os.path.join(out_dir, f"{safe_scene}_{int(_time.time())}.json")
            with open(out_path, "w", encoding="utf-8") as f:
                json.dump(payload, f, ensure_ascii=False, indent=2)
            self._last_zone_decompose_snapshot = out_path
            logger.info("[SceneComposer] Zone decompose snapshot saved: %s", out_path)
        except Exception as e:
            logger.warning("[SceneComposer] Zone decompose snapshot 保存失败（忽略）: %s", e)

    def _llm_decompose(self, text: str) -> List[Dict[str, Any]]:
        """调 LLM 把场景拆成 zones 列表。失败返回 []。"""
        from concurrent.futures import ThreadPoolExecutor, TimeoutError as FTimeout
        from Quasar.ai_models.base_pool.registry import get_chat_model
        from langchain_core.messages import HumanMessage, SystemMessage

        def _call():
            llm = get_chat_model(temperature=0, request_timeout=60.0)
            return llm.invoke([
                SystemMessage(content=_ZONE_DECOMPOSE_SYSTEM_PROMPT),
                HumanMessage(content=text[:2000]),
            ])

        try:
            ex = ThreadPoolExecutor(max_workers=1)
            fut = ex.submit(_call)
            try:
                resp = fut.result(timeout=65.0)
            except FTimeout:
                ex.shutdown(wait=False, cancel_futures=True)
                logger.warning("[SceneComposer] Zone 分解超时，退化单盒")
                return []
            finally:
                ex.shutdown(wait=False)

            raw = (resp.content if hasattr(resp, "content") else str(resp)).strip()
            if "```" in raw:
                s = raw.find("{"); e = raw.rfind("}")
                if s != -1 and e != -1:
                    raw = raw[s:e + 1]
            data = json.loads(raw)
            zones = data.get("zones") if isinstance(data, dict) else None
            return zones if isinstance(zones, list) else []
        except Exception as e:
            logger.warning("[SceneComposer] Zone 分解失败，退化单盒: %s", e)
            return []

    def _build_zone_tree(self, zones_spec: List[Dict[str, Any]]):
        """把 LLM 的 zones 列表实例化成 ZoneTree。

        坐标约定（14b-ii 不动布局代码的关键）：
        - indoor box/shell 恒置原点 → 现有"原点布局"逻辑无需改动就落在体积内。
        - outdoor terrain 也置原点（一大片，内层嵌在中间）。
        - box 朝父 terrain 开门洞（has_door）→ 写进 connector，_generate_room_box 读它。
        - shell（15a）：用生成的建筑模型当围合体，模型自带入口，不开矩形门洞、不生成白盒。
        """
        from ..data_model.zone_tree import Zone, ZoneTree, Volume, Connector

        if not zones_spec:
            return None

        nodes: Dict[str, Zone] = {}
        order: List[str] = []
        for i, spec in enumerate(zones_spec[:2]):  # 最多两层
            zid = str(spec.get("id") or f"z{i}")
            enclosure = spec.get("enclosure") or "box"
            if enclosure not in ("box", "terrain", "shell"):
                enclosure = "box"
            role = spec.get("role") or ("outdoor" if enclosure == "terrain" else "indoor")
            size = spec.get("size") or ([20.0, 20.0, 0.0] if enclosure == "terrain"
                                        else list(self.room_size))
            try:
                w, d, h = float(size[0]), float(size[1]), float(size[2] if len(size) > 2 else 0.0)
            except Exception:
                w, d, h = (20.0, 20.0, 0.0) if enclosure == "terrain" else tuple(self.room_size)
            center = [0.0, h / 2.0 if enclosure in ("box", "shell") else 0.0, 0.0]
            zone = Zone(
                zone_id=zid, name=str(spec.get("name") or zid),
                role=role, enclosure=enclosure,
                volume=Volume(center=center, size=[w, d, h]),
            )
            # 15a：shell 记主外壳 asset 名（建筑模型），由 _place_shell 确定性包裹体积
            if enclosure == "shell":
                shell_name = (spec.get("shell_asset") or spec.get("name") or "").strip()
                zone.primary_shell_asset_id = shell_name or None
            if enclosure == "terrain":
                zone.terrain_profile = _terrain_profile_from_spec(spec, zones_spec)
            zone.metadata["has_door"] = bool(spec.get("has_door"))
            zone.metadata["parent"] = spec.get("parent")
            zone.metadata["raw_aspects"] = spec.get("aspects") if isinstance(spec.get("aspects"), list) else []
            zone.style_context = spec.get("style_context") if isinstance(spec.get("style_context"), dict) else {}
            nodes[zid] = zone
            order.append(zid)

        # 挂树 + 门洞（仅 box；shell 用模型自带入口）
        root = None
        for zid in order:
            z = nodes[zid]
            parent_id = z.metadata.get("parent")
            if parent_id and parent_id in nodes and parent_id != zid:
                nodes[parent_id].sub_zones.append(z)
                # box 朝父开门洞：宽 = min(房宽*0.5, 1.2)，高 = min(房高*0.8, 2.2)
                if z.enclosure == "box" and z.metadata.get("has_door"):
                    dw = min(z.volume.size[0] * 0.5, 1.2)
                    dh = min((z.volume.size[2] or 2.5) * 0.8, 2.2)
                    z.connectors.append(Connector(
                        connector_id=f"door_{zid}", type="door",
                        position=[0.0, 0.0, z.volume.size[1] / 2.0],
                        size=[dw, dh], target_zone_id=zid,
                    ))
            else:
                if root is None:
                    root = z
        if root is None:
            root = nodes[order[0]]
        for zone in nodes.values():
            normalize_zone_aspects(zone)
            apply_scene_semantic_terrain_profile(
                zone,
                getattr(self, "_last_zone_decompose_text", ""),
                getattr(zone, "role", "") or "",
            )
        return ZoneTree(root=root)

    @staticmethod
    def _looks_like_outdoor_with_indoor_structure(text: str) -> bool:
        value = str(text or "").lower()
        outdoor_terms = (
            "室外", "户外", "草原", "森林", "营地", "山坡", "野外",
            "outdoor", "grassland", "forest", "camp",
        )
        structure_terms = (
            "蒙古包", "帐篷", "木屋", "小屋", "房子", "建筑", "商铺", "屋子",
            "yurt", "tent", "cabin", "hut", "building",
        )
        inside_terms = ("里面", "内部", "里有", "内有", "可进入", "inside", "interior")
        return (
            any(term in value for term in outdoor_terms)
            and any(term in value for term in structure_terms)
            and any(term in value for term in inside_terms)
        )

    @staticmethod
    def _fallback_mixed_zone_specs(text: str) -> List[Dict[str, Any]]:
        value = str(text or "")
        shell_asset = "可进入建筑"
        for term in ("蒙古包", "帐篷", "木屋", "小屋", "商铺", "建筑", "房子"):
            if term in value:
                shell_asset = term
                break
        return [
            {
                "id": "outdoor_ground",
                "name": "室外地形",
                "role": "outdoor",
                "enclosure": "terrain",
                "size": [20.0, 20.0, 0.0],
                "aspects": [{"capability": "ground_profile", "params": {"type": "flat", "material": "grass"}}],
            },
            {
                "id": "indoor_structure",
                "name": shell_asset,
                "role": "indoor",
                "enclosure": "box",
                "parent": "outdoor_ground",
                "has_door": True,
                "size": [5.0, 5.0, 3.0],
                "aspects": [{"capability": "interior_surface", "params": {"floor_material": "woven_rug"}}],
                "style_context": {"shell_asset": shell_asset},
            },
        ]

    @classmethod
    def _ensure_mixed_zone_specs(cls, zones_spec: List[Dict[str, Any]], text: str) -> List[Dict[str, Any]]:
        specs = [dict(spec) for spec in (zones_spec or []) if isinstance(spec, dict)]
        if not specs:
            return cls._fallback_mixed_zone_specs(text)
        first_two = specs[:2]
        has_terrain = any(
            str(spec.get("enclosure") or "").lower() == "terrain"
            or str(spec.get("role") or "").lower() == "outdoor"
            for spec in first_two
        )
        has_indoor_structure = any(
            str(spec.get("enclosure") or "").lower() in {"box", "shell"}
            and str(spec.get("role") or "").lower() in {"", "indoor"}
            for spec in first_two
        )
        if has_terrain and has_indoor_structure:
            return specs

        fallback = cls._fallback_mixed_zone_specs(text)
        terrain = next(
            (
                spec for spec in first_two
                if str(spec.get("enclosure") or "").lower() == "terrain"
                or str(spec.get("role") or "").lower() == "outdoor"
            ),
            None,
        )
        indoor = next(
            (
                spec for spec in first_two
                if str(spec.get("enclosure") or "").lower() in {"box", "shell"}
                and str(spec.get("role") or "").lower() in {"", "indoor"}
            ),
            None,
        )
        if terrain is None:
            terrain = fallback[0]
        if indoor is None:
            indoor = fallback[1]

        terrain = dict(terrain)
        indoor = dict(indoor)
        terrain_id = str(terrain.get("id") or "outdoor_ground")
        terrain["id"] = terrain_id
        terrain.setdefault("role", "outdoor")
        terrain.setdefault("enclosure", "terrain")
        indoor.setdefault("id", "indoor_structure")
        indoor.setdefault("role", "indoor")
        indoor.setdefault("enclosure", "box")
        indoor["parent"] = terrain_id
        if str(indoor.get("enclosure") or "").lower() == "box":
            indoor["has_door"] = True
        return [terrain, indoor]

    def _collect_shell_assets(self) -> set:
        """15a：从 zone_tree 收集所有 shell zone 的主外壳 asset 名（用于从家具清单剔除）。"""
        names = set()
        if self.zone_tree is None or self.zone_tree.root is None:
            return names
        for z in self.zone_tree.list_all_zones():
            if (z.enclosure or "") == "shell" and z.primary_shell_asset_id:
                names.add(z.primary_shell_asset_id.strip())
        return names

    def _degrade_failed_shells_to_box(self, failed_asset_names: set) -> List[str]:
        """15a 优雅降级：shell 模型生成失败的 zone → 退回 enclosure=box + 门洞。

        文档哲学：volume 是事实源，建筑只是外壳 dressing。dressing（文生3D）失败时，
        volume 仍需要一个兜底围合——box 是那个万能退化围合（=14b-i 的干净几何）。
        没了圆模型后，方盒单独存在不撕裂，且仍可从门洞走进去。

        返回被降级的 zone 名列表（供汇报）。
        """
        from ..data_model.zone_tree import Connector
        degraded = []
        if self.zone_tree is None or self.zone_tree.root is None:
            return degraded
        for z in self.zone_tree.list_all_zones():
            if (z.enclosure or "") != "shell":
                continue
            asset = (z.primary_shell_asset_id or "").strip()
            if asset not in failed_asset_names:
                continue
            # shell → box：清外壳 asset，补门洞（朝父区域），让 _generate_room_box 接管
            z.enclosure = "box"
            z.primary_shell_asset_id = None
            if not z.connectors and z.metadata.get("parent"):
                dw = min(z.volume.size[0] * 0.5, 1.2)
                dh = min((z.volume.size[2] or 2.5) * 0.8, 2.2)
                z.connectors.append(Connector(
                    connector_id=f"door_{z.zone_id}", type="door",
                    position=[0.0, 0.0, z.volume.size[1] / 2.0],
                    size=[dw, dh], target_zone_id=z.zone_id,
                ))
            degraded.append(asset)
            logger.info("[SceneComposer] 外壳 %s 模型生成失败 → 退回盒子兜底围合（带门洞）", asset)
        return degraded

    @property
    def provider(self):
        if self._provider is None:
            from .model_provider import ModelProvider
            self._provider = ModelProvider()
        return self._provider

    # ── 步骤1: 提取物体清单 ──────────────────────────────────────

    def extract_items(self, text: str) -> List[Dict[str, Any]]:
        """从文字中提取物体清单。优先 LLM，失败回退正则，再过滤黑名单。"""
        logger.info("[SceneComposer] 提取物体清单, 文本长度=%d", len(text))
        items = self._llm_extract(text)
        if not items:
            logger.info("[SceneComposerTrace] phase=extract_fallback_regex reason=llm_empty text_len=%d", len(text))
            items = self._regex_extract(text)

        # 黑名单过滤 + 去重（剔除床品/背景墙/建筑设施等不该单独建模的琐碎项）
        filtered: List[Dict[str, Any]] = []
        seen = set()
        for it in items:
            name = (it.get("name") or "").strip()
            if _is_blacklisted(name):
                logger.info("[SceneComposer] 过滤琐碎项: %s", name)
                continue
            if name in seen:
                continue
            seen.add(name)
            filtered.append(it)

        expanded_candidates = self._ensure_minimum_scene_inventory(text, filtered)
        classification_candidates: List[Dict[str, Any]] = []
        classified_seen = set()
        for it in list(filtered) + list(expanded_candidates):
            name = str(it.get("name") or "").strip()
            if not name or name in classified_seen:
                continue
            classified_seen.add(name)
            classification_candidates.append(it)
        try:
            try:
                from .scene_element_classifier import route_model_items, summarize_classification
            except Exception:
                from scene_element_classifier import route_model_items, summarize_classification  # type: ignore

            logger.info(
                "[SceneComposerTrace] phase=element_classification_start candidates=%d names=%s",
                len(classification_candidates),
                [it.get("name") for it in classification_candidates],
            )
            filtered, classified = route_model_items(text, classification_candidates)
            self._last_element_classification = [item.as_dict() for item in classified]
            self._last_element_classification_summary = summarize_classification(classified)
            for routed in classified:
                logger.info(
                    "[SceneElementClassifier] %s -> %s/%s (%.2f)",
                    routed.name,
                    routed.category,
                    routed.target_pipeline,
                    routed.confidence,
                )
            logger.info(
                "[SceneComposerTrace] phase=element_classification_done model_items=%d classified=%d",
                len(filtered),
                len(classified),
            )
        except Exception as exc:  # noqa: BLE001
            logger.debug("[SceneComposer] SceneElementClassifier skipped: %s", exc)
            filtered = expanded_candidates
            self._last_element_classification = []
            self._last_element_classification_summary = ""
        logger.info("[SceneComposer] 提取到 %d 个物体（过滤前 %d）: %s",
                    len(filtered), len(items), [it.get("name") for it in filtered])
        return filtered

    def _ensure_minimum_scene_inventory(
        self,
        text: str,
        items: List[Dict[str, Any]],
    ) -> List[Dict[str, Any]]:
        """Expand underspecified open-scene inventories to a useful demo size.

        The expansion is prompt-driven, not scene-keyword-driven. Fallback items
        are generic functional placeholders so unknown scenes do not silently
        become grassland/yurt/church-specific.
        """
        if os.getenv("CORONA_DISABLE_INVENTORY_EXPANSION", "0") == "1":
            return items
        if any(word in str(text or "") for word in ("极简", "只要", "只放", "不要太多")):
            return items
        target_min = min(max(1, self.max_items), int(os.getenv("CORONA_MIN_SCENE_ITEMS", "6") or "6"))
        if len(items) >= target_min:
            return items
        try:
            expanded = self._llm_expand_inventory(text, items, target_min)
        except Exception as exc:  # noqa: BLE001
            logger.debug("[SceneComposer] 清单补全 LLM 跳过: %s", exc)
            expanded = []
        if not expanded:
            expanded = self._fallback_expand_inventory(items, target_min)

        out: List[Dict[str, Any]] = []
        seen = set()
        for item in list(items) + list(expanded):
            name = str(item.get("name") or "").strip()
            if not name or _is_blacklisted(name) or name in seen:
                continue
            seen.add(name)
            out.append({
                "name": name,
                "quantity": int(item.get("quantity", 1) or 1),
                "keywords": str(item.get("keywords") or name).strip(),
                "layout_role": str(item.get("layout_role") or "").strip() or None,
            })
            if len(out) >= self.max_items:
                break
        if len(out) > len(items):
            logger.info("[SceneComposer] 开放场景清单补全: %d → %d", len(items), len(out))
        return out

    def _llm_expand_inventory(
        self,
        text: str,
        items: List[Dict[str, Any]],
        target_min: int,
    ) -> List[Dict[str, Any]]:
        from concurrent.futures import ThreadPoolExecutor, TimeoutError as FTimeout
        from Quasar.ai_models.base_pool.registry import get_chat_model
        from langchain_core.messages import HumanMessage, SystemMessage

        existing = [str(it.get("name") or "").strip() for it in items if it.get("name")]
        system = (
            "你是开放场景物体清单补全器。根据用户目标和已有清单，补足到 6-10 个可生成的 3D 物体。"
            "不要写死某类场景模板；只从用户文本、风格、功能和空间关系推导。"
            "不要重复主体建筑，不要输出地形/天空/光照本身。"
            "输出 JSON 数组，每项包含 name, keywords, layout_role。"
            "layout_role 只能是 main, support, landmark, furniture, decoration, surface, boundary。"
        )
        prompt = {
            "user_text": str(text or "")[:1800],
            "existing_items": existing,
            "target_min": target_min,
            "max_items": self.max_items,
        }

        def _call():
            llm = get_chat_model(temperature=0, request_timeout=35.0)
            return llm.invoke([
                SystemMessage(content=system),
                HumanMessage(content=json.dumps(prompt, ensure_ascii=False)),
            ])

        logger.info(
            "[SceneComposerTrace] phase=llm_expand_inventory_start existing=%d target_min=%d max_items=%d text_len=%d",
            len(existing),
            target_min,
            self.max_items,
            len(text),
        )
        ex = ThreadPoolExecutor(max_workers=1)
        fut = ex.submit(_call)
        try:
            resp = fut.result(timeout=40.0)
        except FTimeout:
            ex.shutdown(wait=False, cancel_futures=True)
            logger.warning(
                "[SceneComposerTrace] phase=llm_expand_inventory_timeout existing=%d target_min=%d",
                len(existing),
                target_min,
            )
            return []
        finally:
            ex.shutdown(wait=False)
        raw = (resp.content if hasattr(resp, "content") else str(resp)).strip()
        logger.info(
            "[SceneComposerTrace] phase=llm_expand_inventory_response raw_len=%d",
            len(raw),
        )
        if "```" in raw:
            s = raw.find("["); e = raw.rfind("]")
            if s != -1 and e != -1:
                raw = raw[s:e + 1]
        data = json.loads(raw)
        if not isinstance(data, list):
            return []
        out: List[Dict[str, Any]] = []
        for item in data[: max(target_min, self.max_items)]:
            if not isinstance(item, dict) or not item.get("name"):
                continue
            out.append({
                "name": str(item.get("name") or "").strip(),
                "quantity": int(item.get("quantity", 1) or 1),
                "keywords": str(item.get("keywords") or item.get("name") or "").strip(),
                "layout_role": str(item.get("layout_role") or "").strip(),
            })
        logger.info(
            "[SceneComposerTrace] phase=llm_expand_inventory_items count=%d names=%s",
            len(out),
            [item.get("name") for item in out],
        )
        return out

    @staticmethod
    def _fallback_expand_inventory(items: List[Dict[str, Any]], target_min: int) -> List[Dict[str, Any]]:
        generic = [
            ("功能支撑物件", "scene support object", "support"),
            ("导视牌", "wayfinding sign prop", "support"),
            ("灯光装饰", "decorative light prop", "decoration"),
            ("储物道具", "storage crate prop", "decoration"),
            ("活动区装饰", "activity area decoration prop", "decoration"),
            ("小型展示物", "small display prop", "decoration"),
        ]
        existing = {str(it.get("name") or "").strip() for it in items}
        out: List[Dict[str, Any]] = []
        for name, keywords, role in generic:
            if len(items) + len(out) >= target_min:
                break
            if name in existing:
                continue
            out.append({"name": name, "quantity": 1, "keywords": keywords, "layout_role": role})
        return out

    def _llm_extract(self, text: str) -> List[Dict[str, Any]]:
        # extract 是推理任务（自然语言→结构化清单），与布局同级，超时不应比布局短。
        # request_timeout 30→90（布局是 120~180），future timeout 必须 > request_timeout。
        # 超时/空结果重试 1 次：它是 compose 的第一步，单点 API 抖动不该让整链失败。
        from concurrent.futures import ThreadPoolExecutor, TimeoutError as FTimeout
        from Quasar.ai_models.base_pool.registry import get_chat_model
        from langchain_core.messages import HumanMessage, SystemMessage

        def _call():
            llm = get_chat_model(temperature=0, request_timeout=90.0)
            return llm.invoke([
                SystemMessage(content=_EXTRACT_SYSTEM_PROMPT),
                HumanMessage(content=text[:2000]),
            ])

        max_attempts = 2
        for attempt in range(1, max_attempts + 1):
            try:
                logger.info(
                    "[SceneComposerTrace] phase=llm_extract_attempt attempt=%d/%d text_len=%d prompt_len=%d",
                    attempt,
                    max_attempts,
                    len(text),
                    len(text[:2000]),
                )
                ex = ThreadPoolExecutor(max_workers=1)
                fut = ex.submit(_call)
                try:
                    resp = fut.result(timeout=95.0)
                except FTimeout:
                    ex.shutdown(wait=False, cancel_futures=True)
                    logger.warning("[SceneComposer] LLM 提取超时 (第 %d/%d 次)%s",
                                   attempt, max_attempts,
                                   "，重试" if attempt < max_attempts else "，放弃")
                    continue
                finally:
                    ex.shutdown(wait=False)

                raw = (resp.content if hasattr(resp, "content") else str(resp)).strip()
                logger.info(
                    "[SceneComposerTrace] phase=llm_extract_response attempt=%d/%d raw_len=%d",
                    attempt,
                    max_attempts,
                    len(raw),
                )
                if "```" in raw:
                    s = raw.find("["); e = raw.rfind("]")
                    if s != -1 and e != -1:
                        raw = raw[s:e + 1]
                data = json.loads(raw)
                if not isinstance(data, list):
                    logger.info(
                        "[SceneComposerTrace] phase=llm_extract_non_list attempt=%d/%d type=%s",
                        attempt,
                        max_attempts,
                        type(data).__name__,
                    )
                    return []
                items = []
                for d in data[:20]:
                    if isinstance(d, dict) and d.get("name"):
                        items.append({
                            "name": str(d["name"]).strip(),
                            "quantity": int(d.get("quantity", 1) or 1),
                            "keywords": str(d.get("keywords", "") or d["name"]).strip(),
                        })
                if attempt > 1:
                    logger.info("[SceneComposer] LLM 提取第 %d 次重试成功", attempt)
                logger.info(
                    "[SceneComposerTrace] phase=llm_extract_items attempt=%d/%d count=%d names=%s",
                    attempt,
                    max_attempts,
                    len(items),
                    [item.get("name") for item in items],
                )
                return items
            except Exception as e:
                logger.warning("[SceneComposer] LLM 提取失败 (第 %d/%d 次): %s%s",
                               attempt, max_attempts, e,
                               "，重试" if attempt < max_attempts else "，放弃")
        return []

    def _regex_extract(self, text: str) -> List[Dict[str, Any]]:
        """正则回退：抓取 "1. 双人床：..." 这类列表项。"""
        items: List[Dict[str, Any]] = []
        for m in re.finditer(r"^\s*\d+[\.、)]\s*([^\n：:，,（(]+)", text, re.MULTILINE):
            name = m.group(1).strip()
            if name and len(name) <= 20:
                items.append({"name": name, "quantity": 1, "keywords": name})
            if len(items) >= 20:
                break
        return items

    # ── 步骤2: 批量获取模型 ──────────────────────────────────────

    def acquire_models(self, items: List[Dict[str, Any]],
                       image_url: str = "") -> List[Dict[str, Any]]:
        """为每个物体获取 3D 模型路径。返回带 model_path 的物体列表。"""
        logger.info("[SceneComposer] === 批量获取 %d 个物体模型 ===", len(items))
        self._last_fail_reasons: List[str] = []
        resolved: List[Dict[str, Any]] = []
        for idx, item in enumerate(items, 1):
            name = item["name"]
            logger.info("[SceneComposer] (%d/%d) 获取模型: %s", idx, len(items), name)
            try:
                result = self.provider.acquire(
                    name=name,
                    image_url=image_url,
                    prompt_text=item.get("keywords") or f"high quality 3D model of {name}",
                )
                if result.success:
                    item = dict(item)
                    item["model_path"] = result.local_path
                    item["source"] = result.source
                    resolved.append(item)
                    logger.info("[SceneComposer] (%d/%d) ✓ %s → %s",
                                idx, len(items), name, result.local_path)
                else:
                    self._last_fail_reasons.append(f"{name}: {result.error}")
                    logger.warning("[SceneComposer] (%d/%d) ✗ %s: %s",
                                   idx, len(items), name, result.error)
            except Exception as e:
                self._last_fail_reasons.append(f"{name}: {e}")
                logger.exception("[SceneComposer] 获取模型异常 %s: %s", name, e)
        logger.info("[SceneComposer] === 模型获取完成: %d/%d 成功 ===",
                    len(resolved), len(items))
        return resolved

    # ── 步骤2(方案A): 调用原 model_retrieval workflow ──────────────
    def _run_model_retrieval(self, items: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
        """调用原 model_retrieval workflow，为每个物体「文生图→图生3D/检索」。

        完全复用原系统：构造 state.global_assets.multi_scene.approved_elements
        （只给 item_name + image_prompt，不给图，dispatch 会自动文生图补偿），
        invoke 编译好的 DAG，取回 global_assets.model_retrieval.model_results。

        不修改原 workflow 任何代码，仅作为调用方。失败时回退到 acquire_models。
        """
        self._last_fail_reasons = []
        try:
            from ..flows.model_retrieval_workflow import (
                WORKFLOWS, MODEL_RETRIEVAL_FUNCTION_ID,
            )
            graph = WORKFLOWS.get(MODEL_RETRIEVAL_FUNCTION_ID)
            if graph is None:
                raise RuntimeError("model_retrieval workflow 未注册")
        except Exception as e:
            logger.warning("[SceneComposer] 无法加载 model_retrieval workflow: %s，回退本地获取", e)
            return self.acquire_models(items)

        # 组装 approved_elements：每项 {item_name, image_prompt}
        # 强化 prompt 避免不同物品生成相似图片 → 模型检索混淆
        approved = []
        generated_images = {}
        for it in items:
            name = it["name"]
            kw = (it.get("keywords") or "").strip()
            # 用英文前缀 + 物品名构建区分度更高的 prompt，并加入反贴图板约束。
            prompt = self._build_model_image_prompt(name, kw)
            elem = {
                "item_name": name,
                "image_prompt": prompt,
            }
            mode_hint = str(it.get("generation_mode_hint") or "").strip()
            if mode_hint:
                elem["generation_mode_hint"] = mode_hint
                elem["resource_route_reason"] = str(it.get("resource_route_reason") or "")
            approved.append(elem)
            image_url = str(it.get("image_url") or it.get("generated_image_url") or "").strip()
            if image_url:
                generated_images[name] = image_url

        state = {
            "session_id": f"compose_{int(__import__('time').time())}",
            "metadata": {
                "scene_name": self.scene_name, "room_size": self.room_size,
                "skip_six_view_capture": True,  # 跳过截图，避免引擎渲染死锁导致页面卡死
                "progress_sink": getattr(self, "_model_retrieval_progress_sink", None),
            },
            "global_assets": {
                "multi_scene": {
                    "approved_elements": approved,
                    "generated_images": generated_images,  # 显式批次图片优先；缺失项由 dispatch 补偿/降级
                }
            },
            "intermediate": {},
        }

        logger.info("[SceneComposer] 调用原 model_retrieval workflow（%d 个物体，文生图→图生3D）...",
                    len(approved))
        try:
            out = graph.invoke(state)
        except Exception as e:
            logger.exception("[SceneComposer] model_retrieval workflow 执行异常: %s", e)
            self._last_fail_reasons.append(f"workflow异常: {e}")
            return self.acquire_models(items)  # 兜底

        # 取回 model_results
        model_results = (out.get("global_assets", {})
                            .get("model_retrieval", {})
                            .get("model_results", []))
        if not model_results:
            logger.warning("[SceneComposer] model_retrieval 无结果，回退本地获取")
            return self.acquire_models(items)

        # 转成 SceneComposer 内部结构（带 model_path）
        resolved: List[Dict[str, Any]] = []
        from ..flows.model_retrieval_workflow.helpers import resolve_model_file
        for row in model_results:
            name = row.get("item_name", "")
            err = row.get("error")
            if err:
                self._last_fail_reasons.append(f"{name}: {err}")
                continue
            raw_path = row.get("model_path", "")
            local_path = resolve_model_file(raw_path) if raw_path else ""
            if not local_path:
                self._last_fail_reasons.append(f"{name}: 模型路径无效({raw_path})")
                continue
            resolved.append({
                "name": name,
                "model_path": local_path,
                "source": row.get("source", "generation"),
                "object_id": row.get("object_id", name),
            })

        logger.info("[SceneComposer] model_retrieval 完成: %d/%d 成功",
                    len(resolved), len(items))
        return resolved

    # ── 步骤3+4: 复用原有 scene_composition_workflow 的布局+导入 ──
    def _build_placement_items(self, items: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
        """转换为原 workflow compose_scene 期望的 placement_items 结构。"""
        return [{
            "object_id": it["name"], "name": it["name"],
            "file_name": it["name"], "local_path": it.get("model_path", ""),
        } for it in items]

    def compose(self, text: str, image_url: str = "",
                do_import: bool = True,
                do_review: bool = False,
                progress_sink: Optional[Callable[[str], None]] = None,
                interaction_coordinator: Optional[Any] = None,
                room_id: str = "",
                plan_id: str = "",
                session_id: str = "",
                actor_id: str = "") -> Dict[str, Any]:
        """完整场景组合: 提取清单 → 获取模型 → 审查 → 布局+导入。

        三阶段:
          1. generate_all  — 纯 API 调用, 并行, 不碰引擎
          2. review_queue  — 串行, 逐个导入→截屏→VLM→修正→卸载
          3. compose       — LLM 布局 (注入审查结果) → 批量导入 → 物理沉降
        """
        logger.info("[SceneComposer] ====== 开始场景组合 (三阶段) ======")
        generation_text, memory_context = self._compose_generation_text(
            text,
            interaction_coordinator=interaction_coordinator,
            room_id=room_id,
            plan_id=plan_id,
            session_id=session_id,
            actor_id=actor_id,
        )

        def emit_stage(percent: int, label: str, detail: str) -> None:
            if not progress_sink:
                return
            pct = max(0, min(100, int(percent)))
            blocks = max(0, min(10, round(pct / 10)))
            bar = "█" * blocks + "░" * (10 - blocks)
            try:
                progress_sink(f"生成进度 {pct:>3}% [{bar}] {label}。{detail}")
            except Exception:  # noqa: BLE001
                pass

        emit_stage(5, "开始理解场景需求", "我会先识别空间、主体建筑和关键物件。你可以继续补充要求。")
        items = self.extract_items(generation_text)
        if not items:
            return {"items": [], "imported": [], "failed": [],
                    "extracted_count": 0, "model_count": 0,
                    "error": "未能从描述中提取出物体清单"}
        classification_summary = getattr(self, "_last_element_classification_summary", "")
        if classification_summary and progress_sink:
            try:
                progress_sink(classification_summary)
            except Exception:  # noqa: BLE001
                pass
        if _has_resolved_plan_context(generation_text) and _looks_generic_inventory(items):
            names = [str(item.get("name") or "") for item in items]
            logger.warning(
                "[SceneComposer] resolved plan inventory looked generic; paused before model generation: %s",
                names,
            )
            message = "方案解析不足，已暂停生成，请确认是否继续。"
            if progress_sink:
                try:
                    progress_sink(message)
                except Exception:  # noqa: BLE001
                    pass
            return {
                "items": items,
                "imported": [],
                "failed": names,
                "extracted_count": len(items),
                "model_count": 0,
                "error": message,
                "inventory_warning": "generic_inventory_from_resolved_plan",
            }

        # ── Phase 0: 场景空间分解（开放性在这一步，LLM 读结构）──
        # 纯室内单房间 → None（走旧单盒路径）；真·室内外混合 → ZoneTree。
        # 放在截断前：shell 建筑要从家具清单分离 + 保护不被截断（它是场景主体）。
        if self.zone_tree is None:
            try:
                self.zone_tree = self.decompose_zone_tree(generation_text)
            except Exception as e:
                logger.warning("[SceneComposer] 场景分解异常，退化单盒: %s", e)
                self.zone_tree = None
        if self.zone_tree is not None:
            emit_stage(18, "完成空间拆分", "已确定场地、主体和室内外关系，后续会按这些边界摆放。")
        else:
            emit_stage(18, "完成空间判断", "将按当前空间范围继续生成和摆放。")
        # 关键：建树后把 room_size 同步成 indoor box/shell 的真实体积。下游布局 prompt 与
        # 导入后钳制都读 self.room_size——同步后它们自动按真实体积工作，无需改布局代码。
        if self.zone_tree is not None:
            box = self._get_room_zone()
            if box is not None and getattr(box, "enclosure", "") in ("box", "shell"):
                self.room_size = list(box.volume.size)
                logger.info("[SceneComposer] room_size 同步为 indoor 体积: %s",
                            self.room_size)

        # 15a：shell 建筑（如蒙古包）当围合体不是家具。确保它在生成清单里（缺则补）。
        shell_names = self._collect_shell_assets()
        shell_hint_by_name = {}
        if self.zone_tree is not None and self.zone_tree.root is not None:
            for z in self.zone_tree.list_all_zones():
                if (getattr(z, "enclosure", "") or "") == "shell" and z.primary_shell_asset_id:
                    shell_hint_by_name[z.primary_shell_asset_id.strip()] = _shell_generation_hint(z)
        for sname in shell_names:
            matched = [it for it in items if (it.get("name") or "").strip() == sname]
            shell_hint = shell_hint_by_name.get(sname) or "完整建筑外观，入口与建筑风格一体，不要生成剖面或露出内部"
            if matched:
                # 已在清单 → 追加 shell/aspect 引导；入口样式只由 entrance aspect 提供。
                for it in matched:
                    kw = (it.get("keywords") or "").strip()
                    it["keywords"] = (kw + ", " if kw else "") + shell_hint
            else:
                # 不在清单 → 补进来
                items.insert(0, {"name": sname, "quantity": 1,
                                 "keywords": f"{sname}, building exterior, {shell_hint}"})

        if shell_names:
            before = len(items)
            filtered_items: List[Dict[str, Any]] = []
            removed_shell_dupes: List[str] = []
            for it in items:
                name = (it.get("name") or "").strip()
                if name in shell_names:
                    filtered_items.append(it)
                    continue
                if any(_looks_same_scene_asset(name, shell_name) for shell_name in shell_names):
                    removed_shell_dupes.append(name)
                    continue
                filtered_items.append(it)
            if removed_shell_dupes:
                logger.info("[SceneComposer] 剔除 shell 主建筑重复普通物体: %s",
                            removed_shell_dupes)
                items = filtered_items
                # 主建筑重复不应计入用户真实需求数量。
                extracted_total = max(0, before - len(removed_shell_dupes))
            else:
                extracted_total = len(items)
        else:
            extracted_total = len(items)

        truncated = 0
        if extracted_total > self.max_items:
            # 截断保护 shell 名：先留 shell，再用家具填满剩余额度
            shells_kept = [it for it in items if (it.get("name") or "").strip() in shell_names]
            furniture = [it for it in items if (it.get("name") or "").strip() not in shell_names]
            keep_furniture = max(0, self.max_items - len(shells_kept))
            items = shells_kept + furniture[:keep_furniture]
            truncated = extracted_total - len(items)
            logger.info("[SceneComposer] 物体数 %d 超过上限 %d，截断为 %d（保 shell %d，丢 %d）",
                        extracted_total, self.max_items, len(items), len(shells_kept), truncated)

        self._apply_indoor_room_budget(items, generation_text, progress_sink=progress_sink)

        # ── Phase 1: generate_all (并行, 纯 API) ──
        emit_stage(32, "准备所需模型", "正在获取主体和物件资源，界面可能需要等待一会儿。")
        self._model_retrieval_progress_sink = progress_sink
        try:
            resolved = self._run_model_retrieval(items)
        finally:
            self._model_retrieval_progress_sink = None
        if not resolved:
            reasons = getattr(self, "_last_fail_reasons", [])
            detail = ("；".join(reasons[:3]) + ("…" if len(reasons) > 3 else "")) if reasons else "未知原因"
            return {"items": items, "imported": [],
                    "failed": [it["name"] for it in items],
                    "extracted_count": extracted_total, "model_count": 0,
                    "truncated": truncated,
                    "fail_reasons": reasons,
                    "error": f"所有物体的 3D 模型获取失败（{detail}）"}

        # ── Phase 2: review_queue (串行, 全局锁保护) ──
        reviews: List[Dict[str, Any]] = []
        if do_review:
            emit_stage(48, "检查候选模型", "正在筛掉明显不合适的模型，避免后续组装返工。")
            reviews = self._review_models(resolved)
            logger.info("[SceneComposer] 审查完成: %d/%d", len(reviews), len(resolved))

        # ── Phase 3: compose (布局 + 导入, 注入审查结果) ──
        # 15a：shell 建筑从家具里分出来——它走确定性围合放置（_place_shells），
        # 绝不进布局 LLM（否则又被当家具摆中心，与白盒/terrain 双围合撕裂）。
        shell_names = self._collect_shell_assets()
        shell_models = [m for m in resolved
                        if (m.get("name") or "").strip() in shell_names]
        furniture = [m for m in resolved
                     if (m.get("name") or "").strip() not in shell_names]
        if shell_models:
            self._shell_models = shell_models
            logger.info("[SceneComposer] 分离出 %d 个外壳建筑（确定性放置，不进布局LLM）: %s",
                        len(shell_models), [m.get("name") for m in shell_models])

        # 15a：记录哪些 shell 被 decompose 识别出来了（即便模型生成失败也要汇报）。
        # shell_expected = 应该有的外壳；shell_models = 模型生成成功、待放置的外壳。
        shell_failed_gen = sorted(shell_names - {(m.get("name") or "").strip()
                                                 for m in shell_models})
        # 15a 优雅降级：shell 模型生成失败（如文生图返回空）→ 该 zone 退回 box+门洞。
        # 文档哲学：volume 是事实源，建筑只是外壳 dressing；dressing 失败时 volume
        # 仍需兜底围合——box 是那个万能退化围合。无圆模型后方盒单独存在不撕裂。
        degraded = self._degrade_failed_shells_to_box(set(shell_failed_gen)) if shell_failed_gen else []

        # 突击方案接入：渐进式工作流默认开启；如需回退旧清场式路径，
        # 显式设置 USE_PROGRESSIVE_COMPOSE=0。
        use_progressive = os.getenv("USE_PROGRESSIVE_COMPOSE", "1") != "0"
        emit_stage(62, "开始组装场景", "会先放主体和场地，再把物件摆到合理位置。")
        if use_progressive:
            from .scene_composer_progressive import run_progressive_workflow
            result = run_progressive_workflow(self, generation_text, furniture, items, do_import,
                                              reviews=reviews,
                                              progress_sink=progress_sink,
                                              interaction_coordinator=interaction_coordinator,
                                              room_id=room_id,
                                              plan_id=plan_id,
                                              session_id=session_id)
        else:
            result = self._run_original_workflow(generation_text, furniture, items, do_import,
                                                  reviews=reviews)

        result["extracted_count"] = extracted_total
        result["truncated"] = truncated
        result["reviews"] = reviews
        # shell 汇报：放置成功/失败（_place_shells 写的）+ 模型生成就失败的（注明是否已兜底）
        shell_report = getattr(self, "_shell_report", {"placed": [], "failed": []})
        result["shell_placed"] = shell_report.get("placed", [])
        result["shell_failed"] = (shell_report.get("failed", [])
                                  + [f"{n}: 模型生成失败"
                                     + ("（已退回盒子兜底）" if n in degraded else "")
                                     for n in shell_failed_gen])
        result["shell_expected"] = sorted(shell_names)
        result["shell_degraded"] = degraded
        result["zone_decompose_snapshot"] = getattr(self, "_last_zone_decompose_snapshot", None)
        result["element_classification"] = getattr(self, "_last_element_classification", [])
        result["element_classification_summary"] = classification_summary
        result["room_budget"] = getattr(self, "_last_room_budget_summary", {})
        result["indoor_bounds_check"] = getattr(self, "_last_indoor_bounds_report", {})
        result["memory_context_used"] = bool(memory_context)
        result["memory_context_entry_count"] = int(memory_context.get("entry_count") or 0) if memory_context else 0
        emit_stage(96, "完成自动检查", "已汇总摆放结果、用户介入信息和可选外观审查。")
        return result

    def _compose_generation_text(
        self,
        text: str,
        *,
        interaction_coordinator: Optional[Any] = None,
        room_id: str = "",
        plan_id: str = "",
        session_id: str = "",
        actor_id: str = "",
    ) -> tuple[str, Dict[str, Any]]:
        """Append safe shared scoped-memory summary without exposing private agent memory."""
        if interaction_coordinator is None or not room_id:
            return text, {}
        summary_fn = getattr(interaction_coordinator, "memory_summary", None)
        if not callable(summary_fn):
            return text, {}
        try:
            summary = summary_fn(
                room_id=room_id,
                plan_id=plan_id,
                actor_id=actor_id,
                visibility="shared",
                limit=8,
            )
        except Exception as exc:  # noqa: BLE001
            logger.debug("[SceneComposer] scoped memory summary skipped: %s", exc)
            return text, {}
        summary_text = str((summary or {}).get("summary_text") or "").strip()
        entries = (summary or {}).get("entries") or []
        if not summary_text:
            return text, {}
        safe_summary = summary_text[:1200]
        enhanced = (
            f"{text}\n\n"
            "[跨批次已确认上下文]\n"
            f"{safe_summary}\n"
            "[使用规则]\n"
            "以上上下文只用于保持用户意图、风格、空间约束和最近介入的一致性；"
            "不得把它当作新的额外物体清单，也不得覆盖当前明确指令。"
        )
        context = {
            "room_id": room_id,
            "plan_id": plan_id,
            "session_id": session_id,
            "actor_id": actor_id,
            "entry_count": len(entries),
        }
        logger.info(
            "[SceneComposer] 注入 scoped memory summary: room=%s plan=%s actor=%s entries=%s",
            room_id,
            plan_id,
            actor_id,
            len(entries),
        )
        return enhanced, context

    def _review_models(self, resolved: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
        """Phase 2: 串行审查队列 — 逐个导入 → 截图 → VLM → 修正 → 卸载。

        全局锁保护, 同一时刻只审查一个模型, 避免截屏竞态死锁。
        """
        reviews: List[Dict[str, Any]] = []
        try:
            from .model_reviewer import review_single_model
        except ImportError:
            logger.warning("[SceneComposer] model_reviewer 不可用, 跳过审查")
            return reviews

        total = len(resolved)
        for i, item in enumerate(resolved, 1):
            name = item.get("name", "?")
            path = item.get("model_path", "")
            if not path:
                logger.warning("[SceneComposer] review %d/%d %s: 无模型路径, 跳过", i, total, name)
                continue

            logger.info("[SceneComposer] review %d/%d: %s", i, total, name)
            review = review_single_model(
                model_path=path,
                model_name=name,
                model_type=item.get("object_id", name),
            )
            reviews.append(review)

        return reviews

    @staticmethod
    def _get_asset_size(actor_name: str, asset_meta: Dict[str, Any],
                        geo_map: Dict[str, Any]) -> List[float]:
        meta = (asset_meta.get(actor_name)
                or asset_meta.get(geo_map.get(actor_name, {}).get("name", ""))
                or {})
        size = meta.get("size") if meta else None
        if size and len(size) >= 3:
            try:
                return [float(size[0]), float(size[1]), float(size[2])]
            except Exception:
                return [0.0, 0.0, 0.0]
        return [0.0, 0.0, 0.0]

    @staticmethod
    def _clamp_position_to_room_bounds(
        position: List[float],
        half_extents: List[float],
        room_size: List[float],
        margin: float = 0.15,
    ) -> tuple[List[float], bool, str, float]:
        x, y, z = [float(v) for v in position[:3]]
        hx, hy, hz = [max(0.0, float(v)) for v in half_extents[:3]]
        w, d, h = [float(v) for v in room_size[:3]]
        fit_scale = 1.0

        max_hx = max(0.05, w / 2.0 - margin)
        max_hz = max(0.05, d / 2.0 - margin)
        max_hy = max(0.05, h - margin)
        if hx > max_hx or hz > max_hz or hy > max_hy:
            ratios = []
            if hx > 1e-6:
                ratios.append(max_hx / hx)
            if hz > 1e-6:
                ratios.append(max_hz / hz)
            if hy > 1e-6:
                ratios.append(max_hy / hy)
            fit_scale = min([1.0] + ratios)
            if fit_scale < 0.65:
                return [x, y, z], False, "object_too_large_for_room_bounds", fit_scale
            hx *= fit_scale
            hy *= fit_scale
            hz *= fit_scale

        min_x = -w / 2.0 + margin + hx
        max_x = w / 2.0 - margin - hx
        min_z = -d / 2.0 + margin + hz
        max_z = d / 2.0 - margin - hz
        min_y = margin + hy
        max_y = h - margin - hy
        if min_x > max_x or min_z > max_z or min_y > max_y:
            return [x, y, z], False, "room_bounds_have_no_safe_space", fit_scale

        nx = min(max(x, min_x), max_x)
        ny = min(max(y, min_y), max_y)
        nz = min(max(z, min_z), max_z)
        changed = abs(nx - x) > 1e-6 or abs(ny - y) > 1e-6 or abs(nz - z) > 1e-6 or fit_scale < 0.999
        return [nx, ny, nz], changed, "", fit_scale

    @staticmethod
    def _get_object_height(actor_name: str, asset_meta: Dict[str, Any],
                           geo_map: Dict[str, Any]) -> float:
        """从 AABB 或 geometry 中获取物体的高度（米）。

        优先 asset_metadata（trimesh 读出的精确 size），
        回退 compose_scene 输出的 scale（但 scale 不直接给出高度，返回 0）。
        """
        # asset_meta 的 key 是文件名 stem；用 actor_name + geo_map 三重匹配
        meta = (asset_meta.get(actor_name)
                or asset_meta.get(geo_map.get(actor_name, {}).get("name", ""))
                or {})
        if meta and meta.get("size") and len(meta["size"]) >= 2:
            return float(meta["size"][1])  # size = [width, height, depth]
        # 没法知道真实高度，返回 0 让调用方用 Y>=margin 兜底
        return 0.0

    @staticmethod
    def _get_object_depth(actor_name: str, asset_meta: Dict[str, Any],
                          geo_map: Dict[str, Any]) -> float:
        """取物体的深度（z 向厚度，米）。壁挂物贴墙时要把中心往房内拉半个深度，
        否则中心贴墙=后半截穿墙（F5 兽头穿模根因）。取不到返回 0。"""
        meta = (asset_meta.get(actor_name)
                or asset_meta.get(geo_map.get(actor_name, {}).get("name", ""))
                or {})
        if meta and meta.get("size") and len(meta["size"]) >= 3:
            return float(meta["size"][2])  # size = [width, height, depth]
        return 0.0

    @staticmethod
    def _get_placement_type(actor_name: str, asset_meta: Dict[str, Any],
                            geo_map: Dict[str, Any]) -> str:
        """读物体的放置类型（M2 步骤 15d）。

        优先 asset_metadata 的 placement_type（trimesh + 名字推断，见 _infer_placement_type），
        三路匹配兜 A-5 那种 key 撞车；再用名字关键词兜底（壁挂类）。
        """
        meta = (asset_meta.get(actor_name)
                or asset_meta.get(geo_map.get(actor_name, {}).get("name", ""))
                or {})
        pt = str(meta.get("placement_type", "") or "")
        if pt:
            return pt
        # 名字兜底：壁挂类关键词（标本/兽头/镜子/挂钟/壁灯等贴墙物）
        n = actor_name or ""
        for kw in ("标本", "兽头", "鹿头", "牛头", "镜子", "挂钟", "壁灯", "壁挂", "鹿角"):
            if kw in n:
                return "wall_hung"
        return ""

    # ── 场景框架（室内盒子 / 室外 terrain）──────────────────────

    @staticmethod
    def _detect_scene_indoor(prompt: str) -> bool:
        """从 prompt 推断是否为室内场景。默认按室内处理。"""
        text = (prompt or "").lower()
        outdoor_kw = ["室外", "户外", "森林", "山坡", "公园", "街道", "广场",
                       "outdoor", "forest", "park", "street", "garden",
                       "terrain", "mountain", "landscape"]
        indoor_kw = ["卧室", "客厅", "厨房", "室内", "房间", "书房", "浴室",
                      "藏宝室", "宝库", "密室", "库房", "地下室", "洞穴房间",
                      "treasure room", "vault", "chamber",
                      "bedroom", "living", "kitchen", "indoor", "room", "bath"]
        if any(k in text for k in indoor_kw):
            return True
        if any(k in text for k in outdoor_kw):
            return False
        return True  # 默认室内

    def _generate_room_box(self) -> None:
        """在引擎场景中生成整体房间盒子（六面体，单个空心 OBJ）。

        用单个 mesh 替代六片独立平面，物理上作为完整刚体——墙与墙锁死，
        物体怎么碰撞都撑不开。盒内空心，物体在里面自由摆放。
        可在盒子四个上顶点放置观察摄像头供 VLM 审核调整视角。
        """
        import os as _os, time as _t

        # M2 步骤 14a：尺寸从根 Zone 的 Volume 读（退化时等价于旧 room_size）。
        zone = self._get_room_zone()
        width, depth, height = zone.volume.size[0], zone.volume.size[1], zone.volume.size[2]

        # 1. 生成空心盒子 OBJ（六面体，面法向内）
        asset_dir, asset_rel_dir = self._generated_asset_dir()
        mtl_path = asset_dir / "box.mtl"
        obj_path = asset_dir / "box.obj"
        box_route = f"{asset_rel_dir}/box.obj"
        surface_params = _aspect_params(zone, "interior_surface")
        skin = _derive_room_skin_materials(
            surface_params,
            getattr(zone, "style_context", {}) or {},
        )
        floor_mat = skin["floor_material"]
        wall_mat = skin["wall_material"]
        ceiling_mat = skin["ceiling_material"]
        accent_mat = skin["accent_material"]
        with open(mtl_path, "w", encoding="ascii") as f:
            f.write(_room_box_mtl_text(
                floor_material=floor_mat,
                wall_material=wall_mat,
                ceiling_material=ceiling_mat,
                accent_material=accent_mat,
            ))
        # M2 步骤 14b-i：OBJ 由纯函数生成（支持门洞）。门洞从根 Zone 的首个
        # connector 读；当前无 Zone 声明门洞 → door=None → 闭合盒子（与现状一致）。
        door = None
        if zone.connectors:
            c = zone.connectors[0]
            sz = getattr(c, "size", None)
            if sz and len(sz) >= 2:
                door = {"width": sz[0], "height": sz[1]}
        open_face = (_os.getenv("CORONA_ROOM_BOX_OPEN_FACE", "front") or "front").strip().lower()
        if open_face not in ("front", "none", "front_and_ceiling"):
            open_face = "front"
        obj_text = _build_room_box_obj(
            width,
            height,
            depth,
            door=door,
            open_face=open_face,
            floor_mtl="floor",
            wall_mtl="wall",
            ceiling_mtl="ceiling",
        )
        with open(obj_path, "w", encoding="ascii") as f:
            f.write(obj_text)

        # 2. 场景 + Actor
        try:
            from CoronaCore.core.managers import scene_manager as _sm
            from CoronaCore.core.entities.actor import Actor
        except ImportError:
            return

        scene = _sm.get("")
        if scene is None:
            routes = _sm.list_all()
            scene = _sm.get(routes[0]) if routes else None
        if scene is None:
            return

        existing = {a.name for a in scene.get_actors()}
        if "__room_box" in existing:
            return

        # 3. 单个盒子 Actor
        try:
            actor = Actor(name="__room_box", route=box_route, actor_type="mesh",
                          parent_scene=scene)
            # 盒子中心在房间中心，底部 Y=0
            actor.set_position([0.0, height / 2.0, 0.0], True)
            actor.set_scale([width, height, depth], True)
            # 盒子作为静态碰撞体：不参与物理运动，只挡住内部物体
            mech = getattr(actor, "_mechanics", None)
            if mech is not None:
                try:
                    mech.set_physics_enabled(False)
                except Exception:
                    pass
            scene.add_actor(actor)
            _t.sleep(0.3)
            logger.info("[SceneComposer] 整体房间盒子已创建: %.1f×%.1f×%.1f m (floor=%s wall=%s ceiling=%s)",
                        width, depth, height, floor_mat, wall_mat, ceiling_mat)
        except Exception as e:
            logger.warning("[SceneComposer] 房间盒子创建失败: %s", e)

    def _generate_terrain(self, zone) -> None:
        """M2 步骤 15c-ii：为 enclosure=terrain 的 Zone 生成带坡度的地形 mesh。

        通用高度场（_terrain_height）：type 是参数不是 if 分支。中心平台（内嵌 shell/box
        的 footprint 范围）强制 h=0，保护建筑/家具落点；平台外按 terrain_profile 起伏。
        关键：mesh 用世界坐标，Actor scale=[1,1,1]（不再缩放，否则 h 不随 x/z 缩放会错乱）。
        """
        import time as _t

        size = zone.volume.size
        width = size[0] if len(size) > 0 else 20.0
        depth = size[1] if len(size) > 1 else 20.0

        from ..data_model.zone_tree import TerrainProfile
        profile_params = _aspect_params(zone, "ground_profile")
        cover_params = _aspect_params(zone, "ground_cover")
        profile = TerrainProfile(
            type=str(profile_params.get("type") or "flat"),
            amplitude=float(profile_params.get("amplitude", 0.0) or 0.0),
            frequency=float(profile_params.get("frequency", 1.0) or 1.0),
            seed=int(profile_params.get("seed", 0) or 0),
            material=str(profile_params.get("material") or "neutral"),
            scatter=str(cover_params.get("scatter") or cover_params.get("kind") or "none"),
            style_tags=[],
        )
        setattr(profile, "secondary_material", str(profile_params.get("secondary_material") or ""))
        setattr(profile, "detail_pattern", str(profile_params.get("detail_pattern") or "none"))
        setattr(profile, "detail_strength", _float_param(profile_params, "detail_strength", 0.0))

        # 平台半径：内嵌 shell/box 子 zone 的 footprint × 2.2（B 方案：宁大勿小，给足余量，
        # 保证 shell 真实脚印一定装得下；放 shell 时再夹回这个平台）。无子 zone → 无平台。
        platform_radius = 0.0
        for sub in getattr(zone, "sub_zones", []) or []:
            if (getattr(sub, "enclosure", "") or "") in ("shell", "box"):
                sw = sub.volume.size[0] if sub.volume.size else 4.0
                sd = sub.volume.size[1] if len(sub.volume.size) > 1 else 4.0
                platform_radius = max(platform_radius, max(sw, sd) / 2.0 * 2.2)
        # 锚定链-2：存平台半径，供 _place_shells 夹回 + _generate_interior_floor 派生用。
        self._platform_radius = platform_radius

        # 地形范围随主建筑放大；extent_factor 来自 ground_profile.params。
        building_extent = 0.0
        for sub in getattr(zone, "sub_zones", []) or []:
            if (getattr(sub, "enclosure", "") or "") in ("shell", "box"):
                sw = sub.volume.size[0] if sub.volume.size else 4.0
                sd = sub.volume.size[1] if len(sub.volume.size) > 1 else 4.0
                building_extent = max(building_extent, max(sw, sd))
        width, depth, extent_meta = _resolve_terrain_extent(
            width, depth, building_extent, profile_params,
        )
        self._terrain_extent = dict(extent_meta)
        self._terrain_extent.update({
            "width": width,
            "depth": depth,
            "openness": _float_param(profile_params, "openness", 0.5),
        })

        asset_dir, asset_rel_dir = self._generated_asset_dir()
        terrain_mtl_name = "terrain_style.mtl"
        grass_mtl_path = asset_dir / terrain_mtl_name
        terrain_path = asset_dir / "terrain.obj"
        terrain_route = f"{asset_rel_dir}/terrain.obj"
        material = str(getattr(profile, "material", "neutral") or "neutral")
        secondary_material = str(getattr(profile, "secondary_material", "") or "")
        scatter = str(getattr(profile, "scatter", "none") or "none")
        # 15c：材质/散布来自 aspect params；无 aspect 时中性、不猜场景身份。
        with open(grass_mtl_path, "w", encoding="ascii") as f:
            f.write(_terrain_mtl_text(material, secondary_material))
        with open(terrain_path, "w", encoding="ascii") as f:
            f.write(_build_terrain_mesh_obj(
                width, depth, profile, platform_radius, grid=32,
                mtl_lib=terrain_mtl_name, mtl_name="terrain",
            ))

        try:
            from CoronaCore.core.managers import scene_manager as _sm
            from CoronaCore.core.entities.actor import Actor
        except ImportError:
            return

        scene = _sm.get("")
        if scene is None:
            routes = _sm.list_all()
            scene = _sm.get(routes[0]) if routes else None
        if scene is None:
            return

        existing = {a.name for a in scene.get_actors()}
        if "__room_terrain" in existing:
            return

        try:
            actor = Actor(name="__room_terrain", route=terrain_route, actor_type="mesh",
                          parent_scene=scene)
            # 引擎把 mesh 归一化成单位盒（max_extent→1）→ 世界大小只认 scale，mesh 真实坐标无效。
            # 均匀 scale = mesh 真实最大边（width，因 width≫height）→ 还原真实尺寸 + 保持坡度比例。
            s = max(width, depth)
            actor.set_scale([s, s, s], True)
            # 缩放后读 local AABB（仍是 ±0.5 归一化）→ 世界最低点 = aabb_min_y × s，抬到 y=0
            # （平台是高度场最低点 h=0，抬平台到地面，坡在其上）。自动吸收引擎的居中偏移。
            min_y = 0.0
            try:
                geo = getattr(actor, "_geometry", None)
                aabb = geo.get_aabb() if geo is not None else None
                if aabb and len(aabb) >= 6:
                    min_y = float(aabb[1]) * s
            except Exception:
                pass
            actor.set_position([0.0, -min_y, 0.0], True)
            mech = getattr(actor, "_mechanics", None)
            if mech is not None:
                try:
                    mech.set_physics_enabled(False)
                except Exception:
                    pass
            scene.add_actor(actor)
            _t.sleep(0.3)
            logger.info("[SceneComposer] 地形(terrain)已创建: %.1f×%.1f m, type=%s, material=%s, scatter=%s, scale=%.1f, 抬高=%.2f",
                        width, depth, getattr(profile, "type", "?"), material, scatter, s, -min_y)
        except Exception as e:
            logger.warning("[SceneComposer] 地形创建失败: %s", e)

        # 15e：地表覆盖散布层。只有 ground_cover aspect 显式声明时才生成；
        # flat 地形也可有覆盖物，是否覆盖由 aspect 决定，不由场景关键词兜底。
        # actor 名用 __terrain_ 前缀（非 __room_，否则 _generate_room_box 的 __room_ 守卫
        # 会在 terrain+box 混合场景误挡盒子）。
        if _has_aspect(zone, "ground_cover") and scatter != "none" and "__terrain_grass" not in existing:
            try:
                grass_mtl_path = asset_dir / "grass_blade.mtl"
                grass_obj_path = asset_dir / "grass_blade.obj"
                grass_route = f"{asset_rel_dir}/grass_blade.obj"
                with open(grass_mtl_path, "w", encoding="ascii") as f:
                    f.write(_scatter_mtl_text(scatter, material))
                with open(grass_obj_path, "w", encoding="ascii") as f:
                    density = max(0.1, min(2.0, _float_param(cover_params, "density", 1.0)))
                    scatter_count = max(20, min(300, int(160 * density)))
                    f.write(_build_grass_obj(
                        width, depth, profile, platform_radius,
                        count=scatter_count, scatter=scatter,
                    ))
                gactor = Actor(name="__terrain_grass", route=grass_route,
                               actor_type="mesh", parent_scene=scene)
                # 同 terrain：引擎归一化 mesh 成单位盒 → 世界大小只认 scale。
                # 均匀 scale = max(width,depth) 还原草簇散布的真实范围（否则压成 1m）。
                gs = max(width, depth)
                gactor.set_scale([gs, gs, gs], True)
                gmin_y = 0.0
                try:
                    ggeo = getattr(gactor, "_geometry", None)
                    gaabb = ggeo.get_aabb() if ggeo is not None else None
                    if gaabb and len(gaabb) >= 6:
                        gmin_y = float(gaabb[1]) * gs
                except Exception:
                    pass
                gactor.set_position([0.0, -gmin_y, 0.0], True)
                gmech = getattr(gactor, "_mechanics", None)
                if gmech is not None:
                    try:
                        gmech.set_physics_enabled(False)
                    except Exception:
                        pass
                scene.add_actor(gactor)
                _t.sleep(0.2)
                logger.info("[SceneComposer] terrain scatter layer created: %s, %d clusters",
                            scatter, scatter_count)
            except Exception as e:
                logger.warning("[SceneComposer] 草/花散布失败（忽略）: %s", e)


    def _generate_fence(self, boundary_params: Dict[str, Any] = None,
                        anchor: Dict[str, Any] = None) -> None:
        """boundary dressing：围绕 resolved anchor 一圈，门那侧（+Z）留口。

        shell 场景优先跟随 self._shell_aabb 真实世界足迹；纯室外场景回退到
        terrain platform / zone volume，避免 boundary 被 shell-only 路径卡死。
        引擎归一化 mesh 成单位盒 → 世界大小只认 scale；scale=2*ring_r 还原环径 + 抬地。
        __terrain_ 前缀 → 已在 AI 编辑排除列表（选项 B，背景环境不可手调）。
        """
        import time as _t, math as _math

        boundary_params = dict(boundary_params or {})
        kind = str(boundary_params.get("kind") or "fence").strip().lower()
        if kind not in ("fence", "wall", "hedge"):
            kind = "fence"
        material = str(boundary_params.get("material") or "").strip().lower()
        if not material:
            material = {"wall": "stone", "hedge": "greenery"}.get(kind, "wood")
        try:
            height = float(boundary_params.get("height", 1.1) or 1.1)
        except Exception:
            height = 1.1

        anchor = dict(anchor or resolve_zone_anchor(self, None, "boundary"))
        ring_r = float(anchor.get("ring_radius", 0.0) or 0.0)
        if ring_r < 1e-6:
            return

        try:
            from CoronaCore.core.managers import scene_manager as _sm
            from CoronaCore.core.entities.actor import Actor
        except ImportError:
            return
        scene = _sm.get("")
        if scene is None:
            routes = _sm.list_all()
            scene = _sm.get(routes[0]) if routes else None
        if scene is None:
            return
        existing = {a.name for a in scene.get_actors()}
        if "__terrain_boundary" in existing or "__terrain_fence" in existing:
            return

        asset_dir, asset_rel_dir = self._generated_asset_dir()
        fence_mtl_path = asset_dir / "boundary.mtl"
        fence_obj_path = asset_dir / "boundary.obj"
        boundary_route = f"{asset_rel_dir}/boundary.obj"
        try:
            with open(fence_mtl_path, "w", encoding="ascii") as f:
                f.write(_boundary_mtl_text(kind, material))
            with open(fence_obj_path, "w", encoding="ascii") as f:
                # gap 朝 +Z（门洞默认朝向）= 角度 pi/2
                f.write(_build_fence_obj(ring_r,
                                         gap_center_angle=_math.pi / 2.0,
                                         gap_half_angle=0.5,
                                         mtl_lib="boundary.mtl",
                                         kind=kind,
                                         height=height))
            factor = Actor(name="__terrain_boundary", route=boundary_route,
                           actor_type="mesh", parent_scene=scene)
            # 归一化后环径=单位盒 max 边=1 → scale=2*ring_r 让世界环半径=ring_r。
            s = 2.0 * ring_r
            factor.set_scale([s, s, s], True)
            # 缩放后读 local AABB（±0.5）→ 世界最低点=aabb_min_y×s，抬到 y=0 贴地。
            fmin_y = 0.0
            try:
                fgeo = getattr(factor, "_geometry", None)
                faabb = fgeo.get_aabb() if fgeo is not None else None
                if faabb and len(faabb) >= 6:
                    fmin_y = float(faabb[1]) * s
            except Exception:
                pass
            center = anchor.get("center") or [0.0, 0.0, 0.0]
            factor.set_position([float(center[0]), -fmin_y, float(center[2])], True)
            fmech = getattr(factor, "_mechanics", None)
            if fmech is not None:
                try:
                    fmech.set_physics_enabled(False)
                except Exception:
                    pass
            scene.add_actor(factor)
            _t.sleep(0.2)
            logger.info("[SceneComposer] 边界已铺设: kind=%s material=%s height=%.2f ring_r=%.2f scale=%.1f anchor=%s",
                        kind, material, height, ring_r, s, anchor.get("anchor_type", "?"))
        except Exception as e:
            logger.warning("[SceneComposer] 边界铺设失败（忽略）: %s", e)


    def _generate_interior_floor(self, zone) -> None:
        """M2 步骤 15b / 锚定链-3：为 shell zone 铺内皮地面（interior_surface 的 floor）。

        shell 建筑模型是实心外观团块，没有可用内表面——进去后地面是黑的、
        物体悬空（F5 截图2）。这里程序生成一块地面（地毯），不靠外壳内表面。
        锚定链：地毯尺寸**从 shell 真实世界足迹派生**（self._shell_aabb，_place_shells 量得），
        而非抽象 volume——这是修"地毯与蒙古包不贴合、露天空缝"穿模的关键。
        无真实足迹时（shell 未放/测量失败）兜底回抽象 volume × INSCRIBE。
        """
        import time as _t

        INSCRIBE = 0.96  # 贴边：地毯铺到 shell 真实足迹近边缘。圆盘地板配圆底建筑，
        # 不再有"方角戳穿圆壳"顾虑，故从 0.85 放大到 0.96（留 4% 余量防曲面壁微穿）。
        # 锚定链：优先用 shell 真实足迹（half_x/half_z → full = 2×half）
        aabb = getattr(self, "_shell_aabb", {}).get(getattr(zone, "zone_id", ""), None)
        if aabb and aabb.get("half_x", 0) > 1e-6:
            width = aabb["half_x"] * 2.0
            depth = aabb["half_z"] * 2.0
            logger.info("[SceneComposer] 内皮地面从 shell 真实足迹派生: %.2f×%.2f m", width, depth)
        else:
            size = zone.volume.size
            width = size[0] if len(size) > 0 else 4.0
            depth = size[1] if len(size) > 1 else 4.0
            logger.info("[SceneComposer] 内皮地面回退抽象 volume（无真实足迹）: %.2f×%.2f m", width, depth)

        # interior_surface 参数化材质；缺失时中性地面，不写死 carpet。
        surface_params = _aspect_params(zone, "interior_surface")
        floor_mat = str(surface_params.get("floor_material") or "neutral")
        floor_shape = _select_interior_floor_shape(width, depth, surface_params)

        asset_dir, asset_rel_dir = self._generated_asset_dir()
        carpet_mtl_path = asset_dir / "carpet.mtl"
        carpet_obj_path = asset_dir / "carpet.obj"
        carpet_route = f"{asset_rel_dir}/carpet.obj"
        with open(carpet_mtl_path, "w", encoding="ascii") as f:
            f.write(_surface_mtl_text(floor_mat))
        with open(carpet_obj_path, "w", encoding="ascii") as f:
            if floor_shape == "quad":
                f.write(_build_floor_obj(mtl_lib="carpet.mtl", mtl_name=floor_mat))
            else:
                f.write(_build_disc_obj(mtl_lib="carpet.mtl", mtl_name=floor_mat))

        try:
            from CoronaCore.core.managers import scene_manager as _sm
            from CoronaCore.core.entities.actor import Actor
        except ImportError:
            return

        scene = _sm.get("")
        if scene is None:
            routes = _sm.list_all()
            scene = _sm.get(routes[0]) if routes else None
        if scene is None:
            return

        existing = {a.name for a in scene.get_actors()}
        if "__interior_floor" in existing:
            return

        try:
            actor = Actor(name="__interior_floor", route=carpet_route,
                          actor_type="mesh", parent_scene=scene)
            # 略抬 1cm 压在 terrain 之上，避免与地面 z-fighting
            actor.set_position([0.0, 0.01, 0.0], True)
            actor.set_scale([width * INSCRIBE, 1.0, depth * INSCRIBE], True)
            mech = getattr(actor, "_mechanics", None)
            if mech is not None:
                try:
                    mech.set_physics_enabled(False)
                except Exception:
                    pass
            scene.add_actor(actor)
            _t.sleep(0.3)
            logger.info("[SceneComposer] 内皮地面已铺设: %.1f×%.1f m (材质=%s shape=%s)",
                        width * INSCRIBE, depth * INSCRIBE, floor_mat, floor_shape)
        except Exception as e:
            logger.warning("[SceneComposer] 内皮地面铺设失败: %s", e)

    def _generate_foundation_surface(self, zone) -> None:
        """Generate an exterior foundation/paving pad from foundation_surface."""
        import time as _t

        if not _has_aspect(zone, "foundation_surface"):
            return
        params = _aspect_params(zone, "foundation_surface")
        aabb = getattr(self, "_shell_aabb", {}).get(getattr(zone, "zone_id", ""), None)
        if aabb and aabb.get("half_x", 0) > 1e-6:
            width = float(aabb.get("half_x", 0.0) or 0.0) * 2.0
            depth = float(aabb.get("half_z", 0.0) or 0.0) * 2.0
            center_x = float(aabb.get("center_x", 0.0) or 0.0)
            center_z = float(aabb.get("center_z", 0.0) or 0.0)
        else:
            size = list(getattr(getattr(zone, "volume", None), "size", []) or [])
            center = list(getattr(getattr(zone, "volume", None), "center", []) or [])
            width = float(size[0] if len(size) > 0 else 4.0)
            depth = float(size[1] if len(size) > 1 else 4.0)
            center_x = float(center[0] if len(center) > 0 else 0.0)
            center_z = float(center[2] if len(center) > 2 else 0.0)

        padding = max(0.0, _float_param(params, "padding", 0.6))
        height_offset = _float_param(params, "height_offset", 0.02)
        material = str(params.get("material") or params.get("floor_material") or "neutral")
        shape = str(params.get("shape") or "auto").strip().lower()
        if shape not in ("disc", "quad"):
            shape = _select_interior_floor_shape(width, depth, params)

        foundation_w = max(0.5, width + padding * 2.0)
        foundation_d = max(0.5, depth + padding * 2.0)

        asset_dir, asset_rel_dir = self._generated_asset_dir()
        mtl_path = asset_dir / "foundation_surface.mtl"
        obj_path = asset_dir / "foundation_surface.obj"
        foundation_route = f"{asset_rel_dir}/foundation_surface.obj"
        with open(mtl_path, "w", encoding="ascii") as f:
            f.write(_surface_mtl_text(material))
        with open(obj_path, "w", encoding="ascii") as f:
            if shape == "quad":
                f.write(_build_floor_obj(mtl_lib="foundation_surface.mtl", mtl_name=material))
            else:
                f.write(_build_disc_obj(mtl_lib="foundation_surface.mtl", mtl_name=material))

        try:
            from CoronaCore.core.managers import scene_manager as _sm
            from CoronaCore.core.entities.actor import Actor
        except ImportError:
            return

        scene = _sm.get("")
        if scene is None:
            routes = _sm.list_all()
            scene = _sm.get(routes[0]) if routes else None
        if scene is None:
            return

        existing = {a.name for a in scene.get_actors()}
        actor_name = "__foundation_surface"
        if actor_name in existing:
            return

        try:
            actor = Actor(name=actor_name, route=foundation_route, actor_type="mesh",
                          parent_scene=scene)
            actor.set_position([center_x, height_offset, center_z], True)
            actor.set_scale([foundation_w, 1.0, foundation_d], True)
            mech = getattr(actor, "_mechanics", None)
            if mech is not None:
                try:
                    mech.set_physics_enabled(False)
                except Exception:
                    pass
            scene.add_actor(actor)
            _t.sleep(0.2)
            self._foundation_extent = {
                "width": foundation_w,
                "depth": foundation_d,
                "shape": shape,
                "material": material,
                "center": [center_x, height_offset, center_z],
            }
            logger.info("[SceneComposer] foundation surface created: %.1fx%.1f material=%s shape=%s",
                        foundation_w, foundation_d, material, shape)
        except Exception as e:
            logger.warning("[SceneComposer] foundation surface skipped: %s", e)

    def _generate_scene_framework(self, prompt: str) -> None:
        """M2 步骤 14b-ii/15a：按 ZoneTree 生成场景框架。

        - terrain zone → 铺地面。
        - box zone → 合成白盒（带门洞），_generate_room_box 读它。
        - shell zone → 不生成白盒：建筑模型本身就是围合体，由 _place_shells 确定性放置。
        - 无 zone_tree（纯室内单房间）→ 走旧 _detect_scene_indoor + 单盒路径，零回归。
        """
        if self.zone_tree is not None and self.zone_tree.root is not None:
            zones = self.zone_tree.list_all_zones()
            has_terrain = any((z.enclosure or "") == "terrain" for z in zones)
            has_box = any((z.enclosure or "") == "box" for z in zones)
            has_shell = any((z.enclosure or "") == "shell" for z in zones)
            for z in zones:
                if (z.enclosure or "") == "terrain":
                    self._generate_terrain(z)
            if has_box:
                # _generate_room_box 内部用 _get_room_zone() 取 indoor 盒并读其门洞
                self._generate_room_box()
            # shell zone 的内皮地面（15b）改到 _place_shells 之后铺（锚定链-4）：
            # 地毯尺寸要从 shell 真实足迹派生，而 shell 此刻还没放、_shell_aabb 还空。
            if not has_box and not has_shell and not has_terrain:
                # 树里既无 box 又无 shell 又无 terrain（异常）→ 兜底单盒
                self._generate_room_box()
            return
        # 退化：纯室内单房间走旧路径
        if getattr(self, "_fallback_room_zone_spec", None) or self._detect_scene_indoor(prompt):
            self._generate_room_box()
        else:
            from ..data_model.zone_tree import Zone, Volume

            width = max(float(self.room_size[0]) * 4.0, 18.0)
            depth = max(float(self.room_size[1]) * 4.0, 18.0)
            zone = Zone(
                zone_id="zone_outdoor_fallback",
                name=self.scene_name or "outdoor",
                role="outdoor",
                enclosure="terrain",
                volume=Volume(center=[0.0, 0.0, 0.0], size=[width, depth, 0.0]),
            )
            apply_scene_semantic_terrain_profile(zone, prompt, "outdoor")
            self._generate_terrain(zone)

    def _place_shells(self, shell_models: List[Dict[str, Any]]) -> Dict[str, List[str]]:
        """15a：把 shell 建筑模型确定性放置成围合体（不经布局 LLM）。

        用生成的建筑模型本身当外壳：居中、落地、按 AABB 缩放到包住对应 shell zone
        的 volume。模型自带入口（如蒙古包门帘），不在它身上 punch 矩形门洞——这正是
        修掉 F5 撕裂的关键（圆壳 + 方盒矩形洞 → 撕裂）。
        几何精度（是否严丝合缝包住、入口朝向）只能 F5 目测，这里给确定性的合理缺省。

        返回 {"placed": [...], "failed": ["名: 原因", ...]}，供 compose 汇报。
        shell 不进 _run_original_workflow 的家具汇报，必须独立上报，否则成败不可见。
        """
        report = {"placed": [], "failed": []}
        # 锚定链-1（任务D）：放置后量真实世界 AABB，存为"内部基准"，供地毯/壁挂/家具派生。
        # key=zone_id → {half_x, half_z, min_y_world(贴地前), top_y_world}。修穿模根因：
        # 依赖物从真实测量边界派生，不再各自从抽象 volume × 不同系数算。
        self._shell_aabb = {}
        if not shell_models or self.zone_tree is None:
            return report
        import time as _t
        try:
            from CoronaCore.core.managers import scene_manager as _sm
            from CoronaCore.core.entities.actor import Actor
        except ImportError:
            report["failed"] = [f"{(m.get('name') or '?')}: 引擎不可用" for m in shell_models]
            return report

        # shell zone 按 asset 名索引（取各自 volume 算缩放）
        shell_zones = {}
        for z in self.zone_tree.list_all_zones():
            if (z.enclosure or "") == "shell" and z.primary_shell_asset_id:
                shell_zones[z.primary_shell_asset_id.strip()] = z

        # 读 AABB 算缩放（trimesh size = [width, height, depth]）
        asset_meta = {}
        try:
            from ..flows.shared.asset_metadata import (
                build_asset_metadata_batch,
            )
            paths = [m["model_path"] for m in shell_models if m.get("model_path")]
            asset_meta = build_asset_metadata_batch(paths)
        except Exception as e:
            logger.warning("[SceneComposer] shell AABB 读取失败（用缺省缩放）: %s", e)

        scene = _sm.get("")
        if scene is None:
            routes = _sm.list_all()
            scene = _sm.get(routes[0]) if routes else None
        if scene is None:
            report["failed"] = [f"{(m.get('name') or '?')}: 无可用场景" for m in shell_models]
            return report

        import os as _os
        WRAP = 1.25  # 外壳比 volume 略大，内部舒适包住（墙厚占比 + 留白）
        for m in shell_models:
            name = (m.get("name") or "").strip()
            path = m.get("model_path", "")
            if not path:
                report["failed"].append(f"{name}: 无模型路径")
                continue
            zone = shell_zones.get(name)
            if zone is None:
                report["failed"].append(f"{name}: 未匹配到 shell zone（asset 名不一致）")
                logger.warning("[SceneComposer] 外壳 %s 未匹配 shell zone，shell_zones=%s",
                               name, list(shell_zones.keys()))
                continue
            vw, vd, vh = zone.volume.size[0], zone.volume.size[1], zone.volume.size[2]
            scale = [1.0, 1.0, 1.0]
            # bug 修复(a)：asset_meta 的 key 是模型路径父目录名（如"蒙古包_1"，带索引），
            # 不是物体名"蒙古包"。用 path 派生 key（与 asset_metadata 完全一致），name 兜底。
            meta_key = _os.path.basename(_os.path.dirname(path)) if path else name
            meta = asset_meta.get(meta_key) or asset_meta.get(name) or {}
            size = meta.get("size")
            if size and len(size) >= 3 and all(float(s) > 1e-6 for s in size[:3]):
                scale = [vw / float(size[0]) * WRAP,
                         vh / float(size[1]) * WRAP,
                         vd / float(size[2]) * WRAP]
                logger.info("[SceneComposer] 外壳 %s AABB=%s → scale=%s (key=%s)",
                            name, size, [round(s, 2) for s in scale], meta_key)
            else:
                logger.warning("[SceneComposer] 外壳 %s 未取到 AABB（key=%s），用缺省缩放 1.0",
                               name, meta_key)
            try:
                actor = Actor(name=f"__shell_{name}", route=path, actor_type="mesh",
                              parent_scene=scene)
                actor.set_position([0.0, 0.0, 0.0], True)
                actor.set_scale(scale, True)
                # bug 修复(b)：模型 pivot 可能在几何中心 → 放 y=0 会半埋地下。
                # 缩放后读真实 AABB，把最低点抬到 y=0（pivot 无关，底部贴地）。
                # 关键：get_aabb() 返回的可能是【未缩放的局部 AABB】（min_y≈-0.38），
                # 蒙古包放大 ~6 倍后真实底部在 -2.3 → 只抬 0.38 仍穿地 ~1.9m。
                # 自适应判断：若 AABB 的 y 跨度 ≈ 局部 size_y（远小于缩放后），说明不含
                # scale，世界底部需乘 scale_y。
                try:
                    geo = getattr(actor, "_geometry", None)
                    aabb = geo.get_aabb() if geo is not None else None
                    if aabb and len(aabb) >= 6:
                        min_y = float(aabb[1])
                        span_y = float(aabb[4]) - min_y
                        sy = float(scale[1])
                        local_sy = (float(size[1]) if (size and len(size) >= 2
                                    and float(size[1]) > 1e-6) else 0.0)
                        # AABB 不含 scale 的判据：缩放 >1 且实测跨度远小于"局部尺寸×scale"
                        aabb_has_scale = not (local_sy > 1e-6 and sy > 1.05
                                              and span_y < local_sy * sy * 0.6)
                        if not aabb_has_scale:
                            min_y *= sy
                            logger.info("[SceneComposer] 外壳 %s AABB 未含 scale，min_y×%.2f", name, sy)
                        if abs(min_y) > 1e-4:
                            actor.set_position([0.0, -min_y, 0.0], True)
                            logger.info("[SceneComposer] 外壳 %s 贴地修正: 世界底=%.3f → 抬高 %.3f",
                                        name, min_y, -min_y)
                        # 锚定链-1：存真实世界 footprint（half_x/half_z），供地毯/壁挂/家具派生。
                        # x/z 与 y 同源：AABB 不含 scale 时乘 scale[0]/scale[2]。
                        sx_fac = 1.0 if aabb_has_scale else float(scale[0])
                        sz_fac = 1.0 if aabb_has_scale else float(scale[2])
                        half_x = abs(float(aabb[3]) - float(aabb[0])) / 2.0 * sx_fac
                        half_z = abs(float(aabb[5]) - float(aabb[2])) / 2.0 * sz_fac
                        # 锚定链-2（B 方案）：脚印超出给足平台则夹回——按比例缩小整体 scale，
                        # 让 shell 真实半径落在平台内（平台外是坡，shell 探出去会一边陷土一边悬空）。
                        plat_r = getattr(self, "_platform_radius", 0.0) or 0.0
                        shell_r = max(half_x, half_z)
                        if plat_r > 1e-6 and shell_r > plat_r:
                            shrink = (plat_r * 0.95) / shell_r   # 留 5% 余量，不贴平台边缘
                            scale = [s * shrink for s in scale]
                            actor.set_scale(scale, True)
                            # 缩放变了 → 重新贴地（最低点抬回 y=0）
                            try:
                                aabb2 = geo.get_aabb()
                                if aabb2 and len(aabb2) >= 6:
                                    my = float(aabb2[1])
                                    if not aabb_has_scale:
                                        my *= float(scale[1])
                                    if abs(my) > 1e-4:
                                        actor.set_position([0.0, -my, 0.0], True)
                            except Exception:
                                pass
                            half_x *= shrink
                            half_z *= shrink
                            logger.info("[SceneComposer] 外壳 %s 脚印 %.2f 超平台 %.2f → 夹回 ×%.2f",
                                        name, shell_r, plat_r, shrink)
                        self._shell_aabb[zone.zone_id] = {
                            "half_x": half_x, "half_z": half_z,
                            "asset": name,
                        }
                        logger.info("[SceneComposer] 外壳 %s 真实足迹: half_x=%.2f half_z=%.2f",
                                    name, half_x, half_z)
                except Exception as e:
                    logger.warning("[SceneComposer] 外壳 %s 贴地/足迹测量失败（忽略）: %s", name, e)
                mech = getattr(actor, "_mechanics", None)
                if mech is not None:
                    try:
                        mech.set_physics_enabled(False)
                    except Exception:
                        pass
                scene.add_actor(actor)
                _t.sleep(0.3)
                report["placed"].append(name)
                logger.info("[SceneComposer] 外壳(shell)已放置: %s scale=%s",
                            name, [round(s, 2) for s in scale])
            except Exception as e:
                report["failed"].append(f"{name}: {e}")
                logger.warning("[SceneComposer] 外壳放置失败 %s: %s", name, e)
        return report


    def _run_original_workflow(self, prompt: str, resolved: List[Dict[str, Any]],
                               all_items: List[Dict[str, Any]],
                               do_import: bool,
                               reviews: List[Dict[str, Any]] = None) -> Dict[str, Any]:
        """调用原 scene_composition_workflow 节点完成布局+导入。

        reviews: Phase 2 审查结果, 注入布局 prompt 让 LLM 考虑旋转/比例建议。
        """

        # 场景框架：按 ZoneTree 生成（terrain/box，或退化单盒），再往里面摆物体
        self._generate_scene_framework(prompt)
        # 15a：shell 建筑确定性放置成围合体（模型路径此时已生成，故在框架之后）。
        shell_models = getattr(self, "_shell_models", None)
        if shell_models:
            self._shell_report = self._place_shells(shell_models)
        # 锚定链-4：shell 放完、_shell_aabb 已填 → 此刻铺内皮地面，地毯尺寸从真实足迹派生
        # （而非框架阶段从抽象 volume 估）。这是 terrain→shell→interior_floor 的正确时序。
        if self.zone_tree is not None and self.zone_tree.root is not None:
            for z in self.zone_tree.list_all_zones():
                if (getattr(z, "enclosure", "") or "") == "shell":
                    self._generate_interior_floor(z)
                    self._generate_foundation_surface(z)
        # 锚定链-5：boundary 是 opt-in aspect；没有声明 boundary 就不生成围栏/边界物。
        # gate 放在调用边界，_generate_fence 只保留几何生成能力。
        if self.zone_tree is not None and self.zone_tree.root is not None:
            boundary_zone = next(
                (z for z in self.zone_tree.list_all_zones() if _has_aspect(z, "boundary")),
                None,
            )
            if boundary_zone is not None:
                boundary_params = _aspect_params(boundary_zone, "boundary")
                boundary_anchor = resolve_zone_anchor(
                    self, boundary_zone, "boundary", params=boundary_params,
                )
                self._generate_fence(
                    boundary_params,
                    anchor=boundary_anchor,
                )

        extracted = len(all_items)
        model_count = len(resolved)
        placement_items = self._build_placement_items(resolved)

        asset_meta = {}
        try:
            from ..flows.shared.asset_metadata import (
                build_asset_metadata_batch,
            )
            paths = [it["model_path"] for it in resolved if it.get("model_path")]
            asset_meta = build_asset_metadata_batch(paths)
        except Exception as e:
            logger.warning("[SceneComposer] asset_metadata 构建失败（忽略）: %s", e)

        # 注入审查结果到布局 prompt
        layout_prompt = prompt[:1500]
        if reviews:
            from .model_reviewer import build_review_context
            review_ctx = build_review_context(reviews)
            layout_prompt = layout_prompt + "\n" + review_ctx
            logger.info("[SceneComposer] 已注入 %d 条审查结果到布局 prompt", len(reviews))

        state: Dict[str, Any] = {
            "prompt": layout_prompt,
            "metadata": {"scene_name": self.scene_name, "room_size": self.room_size},
            "intermediate": {
                "placement_items": placement_items,
                "scene_name": self.scene_name,
                "asset_metadata": asset_meta,
                "total_models": extracted, "valid_models": model_count,
            },
        }

        scene_path = ""
        actors: List[Dict[str, Any]] = []
        try:
            from ..flows.scene_composition_workflow.compose_scene import compose_scene_node
            logger.info("[SceneComposer] 调用原 compose_scene_node...")
            out = compose_scene_node(state)
            if out.get("error"):
                logger.warning("[SceneComposer] compose_scene 失败: %s", out["error"])
                return {"items": resolved, "imported": [],
                        "failed": [it["name"] for it in resolved],
                        "extracted_count": extracted, "model_count": model_count,
                        "error": f"布局失败: {out['error']}"}
            inter = out.get("intermediate", {})
            scene_path = inter.get("scene_json_path", "")
            actors = inter.get("scene_actors", [])
            state["intermediate"].update(inter)
            logger.info("[SceneComposer] compose_scene 完成: %d actors", len(actors))
        except Exception as e:
            logger.exception("[SceneComposer] compose_scene 异常: %s", e)
            return {"items": resolved, "imported": [],
                    "failed": [it["name"] for it in resolved],
                    "extracted_count": extracted, "model_count": model_count,
                    "error": f"布局异常: {e}"}

        imported: List[str] = []
        failed: List[str] = []
        if do_import:
            try:
                from ..flows.scene_composition_workflow.import_to_engine import (
                    import_to_engine_node,
                )
                logger.info("[SceneComposer] 调用原 import_to_engine_node...")
                imp_out = import_to_engine_node(state)
                imp_inter = imp_out.get("intermediate", {})
                imported = [a.get("name", "?") for a in imp_inter.get("imported_actors", [])]
                failed = [a.get("name", "?") for a in imp_inter.get("failed_actors", [])]
                logger.info("[SceneComposer] import 完成: 成功 %d, 失败 %d",
                            len(imported), len(failed))

                # 受控后处理：导入完成后一次性修正所有物体位置
                # 原则：位置修正全在物理关闭时做，最后只开一次极短物理消穿模
                if imported and actors:
                    import time as _t
                    try:
                        from CoronaCore.core.managers import scene_manager as _sm
                        scene = _sm.get("")
                        if scene is None:
                            routes = _sm.list_all()
                            scene = _sm.get(routes[0]) if routes else None
                        if scene is None:
                            raise RuntimeError("无可用场景")

                        geo_map = {a.get("name") or a.get("source_name", ""): a.get("geometry", {})
                                   for a in actors if a.get("geometry")}
                        w, d, h = self.room_size[0], self.room_size[1], self.room_size[2]
                        hw, hd, margin = w / 2.0, d / 2.0, 0.15
                        # 锚定链-5：壁挂物的贴墙半径优先用 shell 真实足迹（_shell_aabb），
                        # 而非抽象 volume 的 hd——修兽头贴抽象方边界、扎进/飘出真实圆壁的穿模。
                        # 取最小 shell 内壁半径（圆壳用 min(half_x,half_z)），再内缩 0.15 贴内侧。
                        shell_wall_r = 0.0
                        _aabbs = getattr(self, "_shell_aabb", {}) or {}
                        if _aabbs:
                            shell_wall_r = min(min(a.get("half_x", 0.0), a.get("half_z", 0.0))
                                               for a in _aabbs.values())
                            shell_wall_r = max(0.0, shell_wall_r - 0.15)

                        # 第一步：回设 LLM 位置 + 钳制 + 整平（物理全程关）
                        mecha, fixed, clamped, leveled = [], 0, 0, 0
                        bounds_report = {
                            "checked": [],
                            "clamped": [],
                            "scaled": [],
                            "unresolved": [],
                            "room_size": [w, d, h],
                        }
                        wall_hung_n = 0   # 15d：壁挂物计数（沿后墙横向错开）
                        for actor_name in imported:
                            actor = scene.find_actor(actor_name) if scene else None
                            if actor is None:
                                continue

                            # 15d：壁挂物（标本/兽头/镜子/挂钟等）→ 贴墙、定高、法向朝内，
                            # 不落地、不入物理（否则被钳制砸到地面或物理撞下墙 → 悬空/朝向乱）。
                            # wall_hung 之前是死分类：_infer_placement_type 算出但全代码零消费。
                            ptype = self._get_placement_type(actor_name, asset_meta, geo_map)
                            if ptype == "wall_hung":
                                wmech = getattr(actor, "_mechanics", None)
                                if wmech is not None:
                                    try:
                                        wmech.set_physics_enabled(False)
                                    except Exception:
                                        pass
                                # 锚定链-5：贴真实 shell 内壁半径（shell_wall_r），无则兜底抽象 hd。
                                # 定高 0.55h，多个壁挂物沿横向错开、夹在内壁半径内（不扎穿/不飘出）。
                                wall_r = shell_wall_r if shell_wall_r > 1e-6 else (hd - 0.1)
                                wx = (wall_hung_n - 0.5) * 0.9 if wall_hung_n > 0 else 0.0
                                wx = max(-wall_r + margin, min(wall_r - margin, wx))
                                # 修穿模：中心放 z=wall_r 会让后半截穿墙。往房内拉半个自身深度，
                                # 让背面贴墙、整体在墙内（_get_object_depth 取真实 z 厚度，取不到留 0.1m 余量）。
                                obj_depth = self._get_object_depth(actor_name, asset_meta, geo_map)
                                back_off = obj_depth / 2.0 if obj_depth > 1e-6 else 0.1
                                wz = wall_r - back_off
                                actor.set_position([wx, h * 0.55, wz])
                                actor.set_rotation([0.0, 0.0, 0.0])  # 后墙法向 +Z 朝内
                                wall_hung_n += 1
                                logger.info("[SceneComposer] 壁挂物贴墙: %s → (%.1f, %.2f, %.2f) 壁半径=%.2f 深度=%.2f",
                                            actor_name, wx, h * 0.55, wz, wall_r, obj_depth)
                                continue

                            mech = getattr(actor, "_mechanics", None)
                            if mech is not None:
                                try:
                                    mech.set_physics_enabled(False)
                                    mech.set_damping(0.98)
                                    mech.set_restitution(0.0)
                                    mecha.append((actor, mech))
                                except Exception:
                                    pass

                            geo = geo_map.get(actor_name, {})
                            x, y, z = actor.get_position()
                            rx, ry, rz = actor.get_rotation()

                            # 回设 LLM 布局位置
                            if geo.get("pos"):
                                px, py, pz = geo["pos"]
                                if abs(x - px) > 0.01 or abs(y - py) > 0.01 or abs(z - pz) > 0.01:
                                    x, y, z = px, py, pz
                                    fixed += 1

                            changed = False
                            size = self._get_asset_size(actor_name, asset_meta, geo_map)
                            half_extents = [
                                size[0] / 2.0 if size[0] > 0 else 0.25,
                                size[1] / 2.0 if size[1] > 0 else 0.25,
                                size[2] / 2.0 if size[2] > 0 else 0.25,
                            ]
                            bounds_report["checked"].append(actor_name)
                            clamped_pos, bounds_changed, bounds_issue, fit_scale = self._clamp_position_to_room_bounds(
                                [x, y, z],
                                half_extents,
                                self.room_size,
                                margin=margin,
                            )
                            if bounds_issue:
                                bounds_report["unresolved"].append(f"{actor_name}: {bounds_issue}")
                            else:
                                if bounds_changed:
                                    x, y, z = clamped_pos
                                    changed = True
                                    clamped += 1
                                    bounds_report["clamped"].append(actor_name)
                                if fit_scale < 0.999:
                                    bounds_report["scaled"].append(f"{actor_name}: {fit_scale:.2f}")

                            # 地面整平：底部贴 Y=0，去倾斜
                            aabb_h = self._get_object_height(actor_name, asset_meta, geo_map)
                            if aabb_h > 0 and abs(y - aabb_h / 2.0) > 0.02:
                                y = aabb_h / 2.0
                                changed = True
                            if abs(rx) > 0.01 or abs(rz) > 0.01:
                                rx, rz = 0.0, 0.0
                                changed = True
                            if changed:
                                leveled += (1 if aabb_h > 0 else 0)

                            actor.set_position([x, y, z])
                            actor.set_rotation([rx, ry, rz])
                            if geo.get("scale"):
                                scale = list(geo["scale"])
                                if fit_scale < 0.999 and len(scale) >= 3:
                                    scale = [float(scale[0]) * fit_scale, float(scale[1]) * fit_scale, float(scale[2]) * fit_scale]
                                actor.set_scale(scale)
                            elif fit_scale < 0.999:
                                try:
                                    current_scale = list(actor.get_scale())
                                except Exception:
                                    current_scale = [1.0, 1.0, 1.0]
                                if len(current_scale) >= 3:
                                    actor.set_scale([
                                        float(current_scale[0]) * fit_scale,
                                        float(current_scale[1]) * fit_scale,
                                        float(current_scale[2]) * fit_scale,
                                    ])

                        logger.info("[SceneComposer] 修正: 回设%d 钳制%d 整平%d",
                                    fixed, clamped, leveled)
                        self._last_indoor_bounds_report = bounds_report

                        # 第二步：仅一次极短暂物理消穿模（0.25s，阻尼 0.98 基本不位移）
                        if mecha:
                            for _actor, mech in mecha:
                                try:
                                    mech.set_physics_enabled(True)
                                except Exception:
                                    pass
                            _t.sleep(0.25)
                            for _actor, mech in mecha:
                                try:
                                    mech.set_physics_enabled(False)
                                except Exception:
                                    pass

                        logger.info("[SceneComposer] 后处理完成: %d 个物体", len(mecha))
                    except Exception as e:
                        logger.warning("[SceneComposer] 后处理失败（忽略）: %s", e)
            except Exception as e:
                logger.exception("[SceneComposer] import_to_engine 异常: %s", e)
                failed = [it["name"] for it in resolved]

        return {
            "items": resolved, "imported": imported, "failed": failed,
            "extracted_count": extracted, "model_count": model_count,
            "scene_path": scene_path, "error": None,
        }


__all__ = ["SceneComposer", "is_compose_request"]



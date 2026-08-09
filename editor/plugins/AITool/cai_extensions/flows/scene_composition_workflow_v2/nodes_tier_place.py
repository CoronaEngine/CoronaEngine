"""分层放置节点 — tier1 (大件 LLM 绝对坐标), tier2 (从属语义关系), tier3 (装饰绝对坐标)。

数据流:
  tier1 → LLM 输出坐标 → place_scene_from_items → scene.json → import_to_engine
  tier2 → LLM 输出语义关系 → _calculate_semantic_position 算坐标 → import
  tier3 → LLM 输出坐标 (同 tier1) → import

差量修正: 只重算问题 actor, 其余 locked 物体作为约束传给 LLM。
"""

from __future__ import annotations

import json
import logging
import time
from typing import Any, Dict, List, Optional

from Quasar.ai_workflow.streaming import stream_output_node

from ..scene_composition_workflow.formatters import NO_OUTPUT
from ..scene_composition_workflow.helpers import get_tool, parse_placement_result

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# 物体分类 (关键词匹配)
# ---------------------------------------------------------------------------

TIER1_KEYWORDS = {
    "bed", "sofa", "couch", "table", "desk", "wardrobe", "cabinet", "closet",
    "bookshelf", "shelf", "dresser", "tv_stand", "tv stand", "fridge",
    "床", "沙发", "桌", "书桌", "衣柜", "柜", "书架", "电视柜", "冰箱",
    "屏风", "茶几", "梳妆台", "餐边柜",
}

TIER2_KEYWORDS = {
    "chair", "nightstand", "lamp", "stool", "ottoman", "side_table", "side table",
    "床头柜", "椅", "灯", "凳", "边桌", "靠垫", "垫", "枕",
}


_CLASSIFY_SYSTEM_PROMPT = """你是室内设计专家。将以下物体按三层分类:

Tier 1 (大件): 占主要空间的大型家具, 如床/沙发/桌子/衣柜/屏风/书架等 (3-5个)
Tier 2 (从属): 依附于大件的中小型物体, 如椅子/台灯/床头柜/靠垫/落地灯/凳子等 (2-4个)
Tier 3 (装饰): 小型装饰物, 如地毯/挂画/花瓶/窗帘/摆件等 (其余)

只输出 JSON 对象, 不要其他文字:
{"object_id": 1, ...}"""


def _classify_item(item: Dict[str, Any]) -> int:
    """关键词回退分类。"""
    name = str(item.get("name", "") or "").lower()
    object_id = str(item.get("object_id", "") or "").lower()
    combined = f"{name} {object_id}"
    for kw in TIER2_KEYWORDS:
        if kw in combined:
            return 2
    for kw in TIER1_KEYWORDS:
        if kw in combined:
            return 1
    return 3


def _classify_items_batch(items: List[Dict[str, Any]]) -> Dict[str, int]:
    """批量 LLM 语义分类, 失败时回退关键词。"""
    if not items:
        return {}

    # 构建 item 描述
    item_lines = []
    for it in items:
        oid = it.get("object_id", it.get("name", "?"))
        name = it.get("name", it.get("object_id", "?"))
        item_lines.append(f"  - object_id: {oid}, 名称: {name}")
    items_text = "\n".join(item_lines)

    try:
        from concurrent.futures import ThreadPoolExecutor, TimeoutError as FuturesTimeoutError
        from langchain_core.messages import HumanMessage, SystemMessage
        from Quasar.ai_models.base_pool.registry import get_chat_model

        model_name = _get_layout_model_name()
        llm = get_chat_model(temperature=0, request_timeout=15.0, model_name=model_name)

        def _do_classify():
            return llm.invoke([
                SystemMessage(content=_CLASSIFY_SYSTEM_PROMPT),
                HumanMessage(content=f"物体列表:\n{items_text}"),
            ])

        executor = ThreadPoolExecutor(max_workers=1)
        future = executor.submit(_do_classify)
        try:
            response = future.result(timeout=30.0)
        except FuturesTimeoutError:
            executor.shutdown(wait=False, cancel_futures=True)
            raise
        else:
            executor.shutdown(wait=False)
        text = (response.content if hasattr(response, "content") else str(response)).strip()

        # 解析 JSON
        if "```" in text:
            start = text.find("{")
            end = text.rfind("}")
            if start != -1 and end != -1:
                text = text[start: end + 1]
        result = json.loads(text)
        if isinstance(result, dict):
            return {str(k): int(v) for k, v in result.items() if isinstance(v, (int, float))}
    except Exception as e:
        logger.warning("_classify_items_batch: LLM 分类失败, 回退关键词: %s", e)

    # 回退关键词
    logger.info("_classify_items_batch: LLM 失败, 回退关键词匹配")
    return {str(it.get("object_id", it.get("name", ""))): _classify_item(it) for it in items}


_CLASSIFIED_CACHE: Dict[int, Dict[str, int]] = {}  # id(placement_items) → {object_id: tier}

def _filter_tier(items: List[Dict[str, Any]], tier: int) -> List[Dict[str, Any]]:
    """从已分类的 items 中筛选指定 tier。LLM 批量分类, 结果缓存。"""
    if not items:
        return []
    cache_key = id(items)
    if cache_key not in _CLASSIFIED_CACHE:
        _CLASSIFIED_CACHE[cache_key] = _classify_items_batch(items)
    classified = _CLASSIFIED_CACHE[cache_key]
    return [it for it in items if classified.get(str(it.get("object_id", it.get("name", "")))) == tier]


# ===========================================================================
# Prompt 模板
# ===========================================================================

TIER1_INITIAL_PROMPT = """你是室内设计师。为以下大件家具规划空间关系。

**不要输出坐标。输出语义关系, 系统会自动计算精确坐标。**

## 场景信息
房间: {room_w}×{room_d}×{room_h}m
原点=房间中心地面, X∈[{neg_x:.1f}, {x_half:.1f}], Z∈[{neg_z:.1f}, {z_half:.1f}]

## 设计意图
{design_intent}

## 大件列表 ({n} 个)
{items_text}

## 可用关系 (每个物体选一个)

| 关系 | target 参数 | 说明 | 参数范围 |
|------|------------|------|---------|
| against_wall | wall: back/front/left/right | 贴墙放置 | offset: 0.2~1.0m, offset_along: -2.5~2.5m |
| in_front | target: 参照物名 | 放在参照物正前方 | distance: 0.3~2.0m |
| near_anchor | target: 参照物名, side: left/right | 放在参照物侧边 | distance: 0.2~0.8m |
| between | target_a: 物体A, target_b: 物体B | 放在两个物体中点 | — |

## 布局原则

1. 沙发/柜子等大件用 against_wall, 贴后墙(back)或前墙(front)或侧墙(left/right)
2. offset_along 用于沿墙错开: 正=右/后, 负=左/前。不同家具用不同 offset_along 避免居中堆叠
3. 茶几/咖啡桌用 in_front 放在沙发前方, distance 0.6~1.0m
4. 单椅可放在茶几侧边用 near_anchor

## 输出 JSON (语义关系, 非坐标)
[
  {{
    "object_id": "沙发",
    "relation": "against_wall",
    "target": "back",
    "offset": 0.3,
    "offset_along": 0.5,
    "reason": "沙发靠后墙, 右偏0.5m"
  }},
  {{
    "object_id": "茶几",
    "relation": "in_front",
    "target": "沙发",
    "distance": 0.8,
    "reason": "茶几在沙发前0.8m"
  }}
]

只输出 JSON 数组, 不要其他文字。"""

TIER1_RETRY_PROMPT = """你是室内设计师。VLM 审查发现以下物体有问题, 请修改它们的**语义关系**。

房间: {room_w}×{room_d}×{room_h}m

【已锁定物体 (不可修改)】:
{locked_text}

【VLM 反馈 (据此调整)】:
{vlm_feedback}

【需修正的物体】:
{problem_items_text}

## 可用关系
- against_wall: wall=back/front/left/right, offset=0.2~1.0, offset_along=-2.5~2.5
- in_front: target=参照物名, distance=0.3~2.0
- near_anchor: target=参照物名, side=left/right, distance=0.2~0.8
- between: target_a=物体A, target_b=物体B

## 修正策略
- VLM 说 too_far: 减小 distance 或改用 near_anchor
- VLM 说 misaligned: 用 in_front 对准正确参照物
- VLM 说 overlap: 增大 offset_along 或换 side
- **target 只能使用锁定物体的完整名称**

只输出需修正物体的 JSON 数组 (语义关系, 非坐标):"""

TIER2_PLACE_PROMPT = """你是室内设计师。当前场景已有以下物体 (位置已确定):

{locked_text}

现在放置以下从属物体:
{items_text}

**输出空间关系描述, 代码会根据关系自动计算坐标。**

JSON 数组:
[
  {{
    "object_id": "床头柜",
    "reference_actor": "双人床",
    "relation": "right",
    "distance_m": 0.3,
    "reason": "床头柜在床右侧, 方便取物"
  }},
  ...
]

**relation 字段** (只能选这 6 个):
- "left" (左侧, -X) | "right" (右侧, +X)
- "front" (前方, -Z) | "behind" (后方, +Z)
- "above" (上方, 放在参照物上面) | "below" (下方)

**规则**:
- reference_actor 必须使用上面列表中的完整名称
- distance_m 是边缘到边缘的间距, 0.2-0.5m
- 台灯放在桌上用 "above", 地毯用 "below"
- 只输出 JSON 数组, 不要其他文字"""

TIER2_RETRY_PROMPT = """修正以下从属物体的空间关系。

【已确定物体 (含已通过的从属, 不可修改)】:
{locked_text}

【VLM 反馈】:
{vlm_feedback}

【需修正的从属物体】:
{problem_items_text}

根据 VLM 反馈调整空间关系。relation 合法值: left/right/front/behind/above/below

输出 JSON 数组:
[{{"object_id": "...", "reference_actor": "...", "relation": "left|right|front|behind|above|below", "distance_m": 0.3, "reason": "修正原因"}}]"""

TIER3_PLACE_PROMPT = """你是室内设计师。当前场景已有以下物体 (含 AABB):

{locked_text}

现在放置以下装饰物 ({n} 个):
{items_text}

房间: {room_w}×{room_d}×{room_h}m

装饰物布局:
1. 地毯: Y≈0.01 (地面), 铺在主要家具下方/前方
2. 挂画: Y≈1.5-2.0 (视线高度), 贴墙放置
3. 窗帘: 窗户位置或墙边
4. 不能遮挡主要家具, 不超出房间边界

输出 JSON 数组 (绝对坐标, 同大件格式):
[
  {{"object_id": "carpet_01", "pos": [x,y,z], "rot": [rx,ry,rz], "scale": [sx,sy,sz], "reason": "..."}},
  ...
]"""

TIER3_RETRY_PROMPT = """修正以下装饰物的位置。

【已确定物体 (不可修改)】:
{locked_text}

【VLM 反馈】:
{vlm_feedback}

【需修正的装饰物】:
{problem_items_text}

输出 JSON 数组 (绝对坐标):"""


def _get_retry_prompt(tier: int) -> str:
    return {1: TIER1_RETRY_PROMPT, 2: TIER2_RETRY_PROMPT, 3: TIER3_RETRY_PROMPT}.get(tier, TIER1_RETRY_PROMPT)


# ===========================================================================
# LLM 调用
# ===========================================================================

def _get_layout_model_name() -> str:
    """读取 layout_model 配置，未设置则回退到 chat.model。"""
    try:
        from Quasar.ai_config.ai_config import get_ai_config
        cfg = get_ai_config()
        return getattr(cfg.chat, "layout_model", None) or cfg.chat.model
    except Exception:
        return "gpt-5.5"


def _call_llm(
    system_prompt: str,
    user_prompt: str,
    timeout: float = 90.0,
    use_layout_model: bool = False,
) -> Optional[str]:
    from concurrent.futures import ThreadPoolExecutor, TimeoutError as FuturesTimeoutError
    from langchain_core.messages import HumanMessage, SystemMessage

    model_name = _get_layout_model_name() if use_layout_model else None

    def _do_call():
        from Quasar.ai_models.base_pool.registry import get_chat_model
        kwargs = {"temperature": 0.3, "request_timeout": 120.0}
        if model_name:
            kwargs["model_name"] = model_name
        llm = get_chat_model(**kwargs)
        return llm.invoke([SystemMessage(content=system_prompt), HumanMessage(content=user_prompt)])

    executor = ThreadPoolExecutor(max_workers=1)
    future = executor.submit(_do_call)
    try:
        response = future.result(timeout=timeout)
    except FuturesTimeoutError:
        executor.shutdown(wait=False, cancel_futures=True)
        logger.warning("tier_place: LLM 超时 (%.0fs)", timeout)
        return None
    except Exception as e:
        executor.shutdown(wait=False)
        logger.warning("tier_place: LLM 失败: %s", e)
        return None
    else:
        executor.shutdown(wait=False)

    return (response.content if hasattr(response, "content") else str(response)).strip()


def _parse_llm_json(text: str) -> Optional[List[Dict[str, Any]]]:
    import re
    text = text.strip()
    # 优先直接解析
    try:
        parsed = json.loads(text)
        return parsed if isinstance(parsed, list) else None
    except json.JSONDecodeError:
        pass
    # 提取代码块中的 JSON
    if "```" in text:
        code_match = re.search(r'```(?:json)?\s*(\[[\s\S]*?\])\s*```', text)
        if code_match:
            text = code_match.group(1)
        else:
            # 回退: 取第一个 [ 到最后一个 ]
            start = text.find("[")
            end = text.rfind("]")
            if start != -1 and end != -1 and start < end:
                text = text[start: end + 1]
    try:
        parsed = json.loads(text)
        return parsed if isinstance(parsed, list) else None
    except json.JSONDecodeError:
        return None


# ===========================================================================
# 辅助
# ===========================================================================

def _format_items(items: List[Dict[str, Any]]) -> str:
    return "\n".join(
        f"  - object_id: {it.get('object_id', '')}, 名称: {it.get('name', '未知')}"
        for it in items
    )


def _format_locked_actors(locked: List[Dict[str, Any]]) -> str:
    if not locked:
        return "无"
    lines = []
    for a in locked:
        name = a.get("name") or a.get("actor_name") or a.get("source_name") or "?"
        pos = a.get("position") or a.get("pos") or a.get("geometry", {}).get("pos") or [0, 0, 0]
        aabb = a.get("aabb")
        line = f"  - {name}: pos={pos}"
        if aabb:
            line += f", AABB=min{aabb.get('min',[])}, max{aabb.get('max',[])}"
        lines.append(line)
    return "\n".join(lines)


def _solve_and_apply(
    items: List[Dict[str, Any]],
    relations: List[Dict[str, Any]],
    room_size: List[float],
    asset_meta: Dict[str, Any],
    placed: Dict[str, Dict[str, Any]] = None,
) -> Optional[List[Dict[str, Any]]]:
    """用 Constraint Solver 将语义关系转为坐标, 应用到 items 上。

    placed: retry 时传入已锁定物体的 {name: {pos, scale}}, 作为 solver 的锚点参考。
    返回设置了 pos 的 items 列表, solver 失败时返回 None (回退 _apply_layout)。
    """
    try:
        from .constraint_solver import solve_relations
        solved = solve_relations(relations, room_size, asset_meta, placed=placed)
    except ImportError:
        logger.warning("_solve_and_apply: constraint_solver 导入失败, 回退 LLM 坐标")
        return None

    if not solved:
        return None

    result = []
    for item in items:
        oid = str(item.get("object_id", ""))
        if oid in solved:
            item_copy = dict(item)
            item_copy["pos"] = solved[oid]["pos"]
            item_copy["rot"] = solved[oid].get("rot", [0, 0, 0])
            item_copy["scale"] = solved[oid].get("scale", item.get("scale", [1, 1, 1]))
            result.append(item_copy)
        else:
            result.append(dict(item))

    logger.info("_solve_and_apply: %d/%d 物体求解成功", len(solved), len(items))
    return result


def _apply_layout(
    items: List[Dict[str, Any]],
    layouts: List[Dict[str, Any]],
) -> List[Dict[str, Any]]:
    """用 LLM 输出的布局覆盖 items 的 pos/rot/scale, 优先 object_id 精确匹配。"""
    # 检测 LLM 输出格式: 语义关系 (无 pos) vs 坐标 (有 pos)
    has_pos = any("pos" in l for l in layouts)
    if not has_pos:
        logger.warning("_apply_layout: LLM 输出无 pos 字段 (可能是语义关系格式), 前100字: %s", str(layouts)[:100])
    layout_map = {str(l["object_id"]): l for l in layouts}
    matched = 0
    for item in items:
        oid = str(item.get("object_id", ""))
        layout = layout_map.get(oid)
        if layout is None:
            continue
        for key, field in (("pos", "pos"), ("rot", "rot"), ("scale", "scale")):
            val = layout.get(field)
            if isinstance(val, list) and len(val) == 3:
                item[key] = [float(v) for v in val]
        matched += 1
    # 索引回退
    if matched < len(items):
        for i, item in enumerate(items):
            if str(item.get("object_id", "")) in layout_map:
                continue
            if i < len(layouts):
                for key, field in (("pos", "pos"), ("rot", "rot"), ("scale", "scale")):
                    val = layouts[i].get(field)
                    if isinstance(val, list) and len(val) == 3:
                        item[key] = [float(v) for v in val]
    return items


# 默认 Scale 表: 按物体类型预设合理尺寸
# 格式: [X_width, Y_height, Z_depth] — 引擎坐标系 X=左右 Y=上下 Z=前后
# 仅修正 LLM 输出 scale=[1,1,1] (未显式设置) 的物体
_DEFAULT_SCALES: Dict[str, List[float]] = {
    # 灯具 — 桌面/角落尺度
    "台灯": [0.35, 0.35, 0.35],
    "落地灯": [0.6, 0.6, 0.6],
    "吊灯": [0.5, 0.5, 0.5],
    "壁灯": [0.3, 0.3, 0.3],
    # 布艺 — 装饰尺度
    "靠垫": [0.3, 0.3, 0.3],
    "抱枕": [0.25, 0.25, 0.25],
    # 地面铺设 — 宽×薄×长
    "地毯": [2.0, 1.0, 1.5],  # Y=1.0 保持模型原始厚度, 物理沉降会修正Y位置
    # 墙面装饰 — 宽×厚×高 (厚度方向垂直墙面)
    "挂画": [1.2, 0.04, 0.8],
    "窗帘": [1.5, 2.5, 0.05],
    # 装饰品
    "花瓶": [0.2, 0.35, 0.2],
    "摆件": [0.15, 0.2, 0.15],
    "绿植": [0.5, 0.8, 0.5],
    "盆栽": [0.4, 0.6, 0.4],
}

# 挂墙物体目标高度 (Y 坐标, 单位米)
_WALL_MOUNT_HEIGHTS: Dict[str, float] = {
    "挂画": 1.6,
    "画": 1.6,
    "窗帘": 2.0,
    "壁灯": 1.8,
    "卷帘": 2.0,
}


class _NativePlacementScene:
    """Read-only scene facade backed by the aggregate scene snapshot."""

    def __init__(self, actors: List[Any]):
        self._actors = list(actors)

    def find_actor(self, name: str):
        wanted = str(name or "").lower()
        for actor in self._actors:
            actor_name = str(getattr(actor, "name", "") or "").lower()
            if actor_name == wanted:
                return actor
        return None

    def get_actors(self):
        return list(self._actors)


def _resolve_tier2_scene(scene_name: str):
    """Resolve tier2's read-only scene view through the aggregate adapter first."""
    try:
        from ...mcp.tools import native_scene_state

        actors = native_scene_state.native_actor_views_with_legacy_fallback(
            scene_name or ""
        )
        if actors:
            return _NativePlacementScene(actors)
    except Exception as exc:
        logger.info("tier2_place: aggregate scene resolver unavailable: %s", exc)
        return None

    return None


def _validate_positions(
    items: List[Dict[str, Any]], room_size: List[float],
) -> int:
    """校验 LLM 输出的坐标: 边界 clamp + 检测全 X=0 模式。

    返回修正的物体数量。
    """
    x_half = room_size[0] / 2
    z_half = (room_size[1] / 2) if len(room_size) > 1 else 1.5
    fixed = 0
    x_values = []

    for item in items:
        pos = item.get("pos")
        if not pos or len(pos) < 3:
            continue
        x_values.append(pos[0])

        # 边界 clamp
        clamped = False
        if pos[0] < -x_half + 0.2:
            pos[0] = -x_half + 0.2
            clamped = True
        elif pos[0] > x_half - 0.2:
            pos[0] = x_half - 0.2
            clamped = True
        if pos[2] < -z_half + 0.2:
            pos[2] = -z_half + 0.2
            clamped = True
        elif pos[2] > z_half - 0.2:
            pos[2] = z_half - 0.2
            clamped = True
        if pos[1] < 0:
            pos[1] = 0
            clamped = True
        # Y 上限: 不超过房间高度的 80%
        room_h = room_size[2] if len(room_size) > 2 else 3
        if pos[1] > room_h * 0.8:
            pos[1] = 0
            clamped = True
            logger.warning("_validate: %s Y=%.2f 异常高, 修正为 0", item.get("object_id", "?"), pos[1])

        # LLM 输出 scale 异常 clamp: 任意轴 >1.8 时回退到 [1,1,1]
        scl = item.get("scale")
        if scl and any(abs(s) > 1.8 for s in scl):
            item["scale"] = [1.0, 1.0, 1.0]
            clamped = True
            logger.warning("_validate: %s scale=%s 异常, 回退 [1,1,1]", item.get("object_id", "?"), scl)

        if clamped:
            fixed += 1
            logger.info("_validate: %s clamp → [%.2f, %.2f, %.2f]",
                        item.get("object_id", "?"), pos[0], pos[1], pos[2])

    # 全部 X=0 检测: 如果 >50% 物品 X≈0, 警告但不强制修改 (交给 VLM 发现)
    if x_values:
        zero_x = sum(1 for x in x_values if abs(x) < 0.1)
        if zero_x > len(x_values) / 2:
            logger.warning("_validate: %d/%d 物品 X≈0, 布局可能过于集中", zero_x, len(x_values))

    return fixed


def _apply_default_scale(items: List[Dict[str, Any]]) -> int:
    """始终用默认 scale 表覆盖已知类型的物体。

    LLM 不知道模型实际尺寸, 可能输出不合理的 scale。
    已知类型 (台灯/落地灯/地毯等) 强制使用预设 scale。
    大件家具 (沙发/床/桌/柜) 保持不变。
    """
    changed = 0
    for item in items:
        name = item.get("name", "") or item.get("object_id", "")
        for keyword, default in _DEFAULT_SCALES.items():
            if keyword in name:
                current = item.get("scale", [1, 1, 1])
                if current != list(default):
                    item["scale"] = list(default)
                    changed += 1
                break
    if changed:
        logger.info("_apply_default_scale: 修正 %d 个物体的 scale", changed)
    return changed


def _calculate_semantic_position(
    ref_name: str, relation: str, distance_m: float, scene,
) -> Optional[List[float]]:
    """根据语义关系计算目标坐标。不依赖引擎 AABB, 用 position + scale 估算。

    relation: left/right/front/behind/above/below
    返回 [x, y, z] 或 None (参考物体未找到)
    """
    ref_actor = scene.find_actor(ref_name)
    if ref_actor is None:
        for a in scene.get_actors():
            if a.name.lower() == ref_name.lower():
                ref_actor = a
                break
    if ref_actor is None:
        return None

    try:
        ref_pos = list(ref_actor.get_position())
        ref_scale = list(ref_actor.get_scale())
    except Exception:
        return None

    # 用 scale 估算半尺寸 (scale 反映模型比例, 默认 1m³ 物体)
    half_x = 0.45 * ref_scale[0]
    half_z = 0.45 * ref_scale[2]
    half_y = 0.5 * ref_scale[1]

    rel = relation.lower()
    if rel == "right":
        return [ref_pos[0] + half_x + distance_m, ref_pos[1], ref_pos[2]]
    elif rel == "left":
        return [ref_pos[0] - half_x - distance_m, ref_pos[1], ref_pos[2]]
    elif rel == "front":
        return [ref_pos[0], ref_pos[1], ref_pos[2] - half_z - distance_m]
    elif rel == "behind":
        return [ref_pos[0], ref_pos[1], ref_pos[2] + half_z + distance_m]
    elif rel == "above":
        return [ref_pos[0], ref_pos[1] + half_y + distance_m, ref_pos[2]]
    elif rel == "below":
        return [ref_pos[0], 0.01, ref_pos[2]]
    return None


def _check_overlap(
    new_pos: List[float], new_scale: List[float],
    existing: List[Dict[str, Any]], threshold: float = 0.15,
    asset_meta: Dict[str, Any] = None,
    new_name: str = "",
) -> bool:
    """检查新位置是否与已有物体重叠 (>threshold 米视为重叠)。

    优先使用 asset_meta 中的真实 bbox 尺寸, 回退到 0.4*scale 估算。
    """
    hx, hz = _get_bbox_half(new_name, new_scale, asset_meta)
    for ex in existing:
        ep = ex.get("pos", [0, 0, 0])
        es = ex.get("scale", [1, 1, 1])
        ex_name = ex.get("name", "") or ex.get("object_id", "")
        ehx, ehz = _get_bbox_half(ex_name, es, asset_meta)
        ox = (hx + ehx) - abs(new_pos[0] - ep[0])
        oz = (hz + ehz) - abs(new_pos[2] - ep[2])
        if ox > threshold and oz > threshold:
            return True
    return False


def _get_bbox_half(
    name: str, scale: List[float], asset_meta: Dict[str, Any] = None,
) -> tuple:
    """从 asset_meta 获取真实半尺寸 (X, Z), 回退到 0.4*scale 估算。"""
    if asset_meta and name:
        meta = asset_meta.get(name, {})
        if meta and meta.get("size"):
            s = meta["size"]
            return s[0] / 2, s[2] / 2
    sc = scale or [1, 1, 1]
    return 0.4 * sc[0], 0.4 * sc[2]


def _resolve_tier_overlaps(
    items: List[Dict[str, Any]],
    asset_meta: Dict[str, Any] = None,
    locked: List[Dict[str, Any]] = None,
) -> int:
    """检测并解决物品间碰撞重叠。对重叠物体尝试 X/Z 偏移。

    返回解决的碰撞数量。
    """
    if len(items) <= 1 and not locked:
        return 0

    existing = list(locked or [])  # 已锁定的先行物体
    resolved = 0

    offsets = (
        [(dx, 0) for dx in (0.3, -0.3, 0.6, -0.6, 0.9, -0.9, 1.2)] +
        [(0, dz) for dz in (0.3, -0.3, 0.6, -0.6, 0.9)] +
        [(dx, dz) for dx in (0.5, -0.5) for dz in (0.4, -0.4)]
    )

    for item in items:
        pos = item.get("pos")
        scl = item.get("scale", [1, 1, 1])
        name = item.get("name", "") or item.get("object_id", "")
        if not pos or len(pos) < 3:
            existing.append(item)
            continue

        if _check_overlap(pos, scl, existing, asset_meta=asset_meta, new_name=name):
            found = False
            for dx, dz in offsets:
                test_pos = [pos[0] + dx, pos[1], pos[2] + dz]
                if not _check_overlap(test_pos, scl, existing, asset_meta=asset_meta, new_name=name):
                    pos[0], pos[2] = test_pos[0], test_pos[2]
                    logger.info("[overlap] %s 偏移 dx=%.1f dz=%.1f 解决重叠", name, dx, dz)
                    found = True
                    resolved += 1
                    break
            if not found:
                logger.warning("[overlap] %s 无法避开重叠, 保持原位置 [%.2f, %.2f]",
                             name, pos[0], pos[2])

        existing.append(item)

    return resolved


_FLOOR_KEYWORDS = {"沙发", "床", "桌", "茶几", "柜", "灯", "地毯", "椅", "凳", "几", "架"}
_WALL_KEYWORDS = {"挂画", "窗帘", "壁灯", "画卷", "卷帘", "画"}


def _classify_objects(names: List[str]):
    """分类: 落地物体 vs 挂墙物体。"""
    floor, wall = [], []
    for name in names:
        if any(kw in name for kw in _WALL_KEYWORDS):
            wall.append(name)
        elif any(kw in name for kw in _FLOOR_KEYWORDS):
            floor.append(name)
    return floor, wall


def _fix_wall_objects(scene_name: str, wall_names: List[str]) -> int:
    """挂墙物体直接修正 Y 坐标 (不参与物理, 物理会让它们掉到地上)。"""
    if not wall_names:
        return 0

    # Wall placement only changes the actor transform, so use the aggregate
    # value-object contract before entering the old Python Scene fallback.
    try:
        from ...mcp.tools.native_scene_state import find_actor_with_legacy_fallback

        fixed = 0
        for name in wall_names:
            actor = find_actor_with_legacy_fallback(scene_name, name)
            if actor is None:
                continue
            pos = actor.get_position()
            old_y = pos[1]
            target_y = old_y
            for kw, height in _WALL_MOUNT_HEIGHTS.items():
                if kw in name:
                    target_y = height
                    break
            if target_y != old_y:
                pos[1] = target_y
                actor.set_position(pos)
                logger.info("[physics] %s wall Y=%.3f → %.3f", name, old_y, target_y)
                fixed += 1
        if fixed:
            logger.info("[physics] native 挂墙修正: %d 个物体", fixed)
        return fixed
    except Exception as exc:
        logger.info("[physics] 挂墙修正异常: %s", exc)
        return 0


def _apply_physics_settlement(scene_name: str, all_actor_names: List[str]) -> int:
    """物理修正: 落地物体重力沉降 + 挂墙物体修正 Y。

    落地: 开物理 1.2s → 重力落至 floor_grid(Y=0) → 关物理
    挂墙: 直接 set_position Y=目标高度 (不参与物理, 否则会掉地上)
    """
    floor_names, wall_names = _classify_objects(all_actor_names)

    # 挂墙物体先修正 (不需要等物理)
    wall_fixed = _fix_wall_objects(scene_name, wall_names)

    # 落地物体物理沉降
    if not floor_names:
        return wall_fixed

    try:
        from ...mcp.tools.native_scene_state import (
            find_actor_with_legacy_fallback,
            set_actor_physics_value,
        )

        actors = []
        for name in floor_names:
            actor = find_actor_with_legacy_fallback(scene_name, name)
            if actor is None:
                continue
            try:
                if set_actor_physics_value(
                    actor,
                    {"damping": 0.9, "restitution": 0.1, "physics_enabled": True},
                ):
                    actors.append((actor, name))
            except Exception as exc:
                logger.info("[physics] 启用失败 %s: %s", name, exc)

        if not actors:
            return wall_fixed

        # 等待沉降 (1 秒足够 60fps × 0.9 阻尼快速稳定)
        time.sleep(1.2)

        # 关闭物理 + 读取最终位置
        settled = 0
        for actor, name in actors:
            try:
                set_actor_physics_value(actor, {"physics_enabled": False})
                pos = actor.get_position()
                logger.info("[physics] %s → Y=%.3f", name, pos[1])
                settled += 1
            except Exception as exc:
                logger.info("[physics] 关闭失败 %s: %s", name, exc)

        logger.info("[physics] 沉降完成: %d/%d 个物体", settled, len(actors))
        return wall_fixed + settled
    except Exception as exc:
        logger.info("[physics] 沉降异常: %s", exc)
        return wall_fixed


def _cleanup_tier_actors(
    tier_items: List[Dict[str, Any]],
    scene_name: str,
) -> int:
    """重试前清理当前 tier 的全部旧 actor (含 _1/_2 后缀变体)。

    引擎遇到同名 actor 自动加 _1/_2 后缀而非替换,
    导致 VLM 在截图中看到多个副本 → 误判为"位置漂移"。
    """
    remove_tool = get_tool("remove_model")
    if remove_tool is None:
        return 0
    removed = 0
    for item in tier_items:
        name = item.get("name") or item.get("object_id", "")
        if not name:
            continue
        # 尝试删除基础名 + _1/_2 后缀变体 (最多清 3 个副本)
        for suffix in ("", "_1", "_2"):
            try_name = f"{name}{suffix}"
            try:
                remove_tool.invoke({"actor_name": try_name, "scene_name": scene_name})
                removed += 1
            except Exception:
                break  # 当前后缀不存在, 停止尝试更高后缀
    if removed:
        logger.info("_cleanup_tier: 清理 %d 个旧 actor (scene=%s)", removed, scene_name)
    return removed


_MECHANICS_VERIFIED = False


def _verify_mechanics_available(scene_name: str) -> None:
    """一次性验证: 引擎导入的 actor 是否挂载了 Mechanics (物理) 组件。"""
    global _MECHANICS_VERIFIED
    if _MECHANICS_VERIFIED:
        return
    _MECHANICS_VERIFIED = True

    # Diagnostics are read-only and can use the authoritative aggregate
    # snapshot.  Do not obtain an engine Actor merely to inspect Mechanics.
    try:
        from ...mcp.tools import native_scene_state

        actors = native_scene_state.native_actor_views_with_legacy_fallback(
            scene_name or ""
        )
        if not actors and scene_name:
            actors = native_scene_state.native_actor_views_with_legacy_fallback("")
        if actors:
            sample = actors[0]
            native_mechanics = getattr(sample, "mechanics", None)
            legacy_mechanics = None
            if isinstance(native_mechanics, dict):
                mechanics = native_mechanics
                has_mechanics = True
            else:
                legacy_mechanics = getattr(sample, "_mechanics", None)
                mechanics = {}
                has_mechanics = legacy_mechanics is not None
                if legacy_mechanics is not None:
                    try:
                        mechanics["physics_enabled"] = (
                            legacy_mechanics.get_physics_enabled()
                        )
                    except Exception:
                        pass
            has_physics = bool(mechanics.get("physics_enabled")) if has_mechanics else False
            aabb_ready = bool(getattr(sample, "bounds_ready", False))
            if not aabb_ready:
                try:
                    aabb = sample.get_world_aabb()
                    aabb_ready = aabb is not None and len(aabb) >= 6
                except Exception:
                    pass
            logger.info(
                "[mechanics_verify] actor=%s  mechanics=%s  physics_enabled=%s  aabb=%s",
                getattr(sample, "name", "?"),
                has_mechanics,
                has_physics,
                aabb_ready,
            )
            return
    except Exception as exc:
        logger.info("[mechanics_verify] native snapshot unavailable: %s", exc)
        logger.info("[mechanics_verify] 无已加载场景, 跳过")


def _import_actors(actors: List[Dict[str, Any]], scene_name: str, skip_names: set = None) -> tuple:
    """导入 actors 到引擎。返回 (imported_list, failed_list)。skip_names 中的 actor 不重复导入。"""
    import_tool = get_tool("import_model")
    if import_tool is None:
        return [], [{"name": a.get("name", "?"), "error": "import_model 不可用"} for a in actors]
    skip = skip_names or set()
    imported, failed = [], []
    for actor in actors:
        name = actor.get("name") or actor.get("source_name") or actor.get("actor_name") or "unknown"
        if name in skip:
            imported.append({"name": name, "status": "skipped"})
            continue
        path = actor.get("path") or actor.get("model_path") or actor.get("geometry", {}).get("path", "")
        geom = actor.get("geometry") or actor.get("transform") or {}
        try:
            import_tool.invoke({
                "model_path": path,
                "actor_name": name,
                "position": geom.get("pos", [0, 0, 0]),
                "rotation": geom.get("rot", [0, 0, 0]),
                "scale": geom.get("scale", [1, 1, 1]),
                "scene_name": scene_name,
            })
            imported.append({"name": name, "model_path": path, "status": "success"})
        except Exception as e:
            logger.warning("import %s 失败: %s", name, e)
            failed.append({"name": name, "error": str(e)})

    # 首次导入后验证引擎物理能力
    if imported and not _MECHANICS_VERIFIED:
        _verify_mechanics_available(scene_name)

    return imported, failed


def _dump_llm_debug(state, tier, retry_count, sys_prompt, user_prompt, llm_output):
    """将 LLM retry 输入输出写入 debug JSON。"""
    import json as _json
    from pathlib import Path as _Path
    from datetime import datetime as _dt

    intermediate = state.get("intermediate", {})
    scene_path = intermediate.get("scene_json_path", "")
    if not scene_path:
        return
    dump_dir = _Path(scene_path).parent
    dump_path = dump_dir / f"llm_debug_t{tier}_r{retry_count}.json"
    try:
        debug = {
            "timestamp": _dt.now().isoformat(),
            "tier": tier,
            "retry": retry_count,
            "system_prompt": sys_prompt,
            "user_prompt": user_prompt,
            "llm_output_raw": llm_output,
        }
        dump_path.write_text(_json.dumps(debug, ensure_ascii=False, indent=2), encoding="utf-8")
        logger.info("llm_debug: dumped tier%d_r%d → %s", tier, retry_count, dump_path)
    except Exception as e:
        logger.warning("llm_debug: dump failed: %s", e)


def _dump_raw_llm_output(state, tier, retry_count, raw_text, is_retry):
    """将 LLM 原始输出文本写入 debug JSON (用于诊断输出格式: 语义关系 vs 坐标)。"""
    import json as _json
    from pathlib import Path as _Path
    from datetime import datetime as _dt

    intermediate = state.get("intermediate", {})
    scene_path = intermediate.get("scene_json_path", "")
    if not scene_path:
        return
    dump_dir = _Path(scene_path).parent
    label = "retry" if is_retry else "initial"
    dump_path = dump_dir / f"llm_raw_t{tier}_{label}.json"
    try:
        debug = {
            "timestamp": _dt.now().isoformat(),
            "tier": tier,
            "phase": label,
            "retry_count": retry_count,
            "raw_text": raw_text[:8000],
        }
        dump_path.write_text(_json.dumps(debug, ensure_ascii=False, indent=2), encoding="utf-8")
        logger.info("llm_raw: dumped tier%d_%s → %s", tier, label, dump_path)
    except Exception as e:
        logger.warning("llm_raw: dump failed: %s", e)


def _dump_layout_result(state, tier, retry_count, items, is_retry):
    """将 LLM 输出的坐标写入 debug JSON。"""
    import json as _json
    from pathlib import Path as _Path
    from datetime import datetime as _dt

    intermediate = state.get("intermediate", {})
    scene_path = intermediate.get("scene_json_path", "")
    if not scene_path:
        return
    dump_dir = _Path(scene_path).parent
    label = "retry" if is_retry else "initial"
    dump_path = dump_dir / f"layout_debug_t{tier}_{label}.json"
    try:
        debug = {
            "timestamp": _dt.now().isoformat(),
            "tier": tier,
            "phase": label,
            "retry_count": retry_count,
            "items": [
                {
                    "object_id": it.get("object_id", ""),
                    "name": it.get("name", ""),
                    "pos": it.get("pos"),
                    "rot": it.get("rot"),
                    "scale": it.get("scale"),
                }
                for it in items
            ],
            "room_size": state.get("metadata", {}).get("room_size"),
        }
        dump_path.write_text(_json.dumps(debug, ensure_ascii=False, indent=2), encoding="utf-8")
        logger.info("layout_debug: dumped tier%d_%s → %s", tier, label, dump_path)
    except Exception as e:
        logger.warning("layout_debug: dump failed: %s", e)


def _place_with_absolute_coords(
    state: Dict[str, Any],
    tier: int,
    items: List[Dict[str, Any]],
    prompt_template: str,
) -> Dict[str, Any]:
    """回退: 用绝对坐标放置指定 tier 的物品 (不用锚点工具)。"""
    intermediate = state.get("intermediate", {})
    metadata = state.get("metadata", {})
    room_size = metadata.get("room_size", [5, 3, 3])
    scene_name = metadata.get("scene_name", "composed_scene")
    locked = intermediate.get("locked_actors", [])

    user_prompt = prompt_template.format(
        locked_text=_format_locked_actors(locked),
        n=len(items),
        items_text=_format_items(items),
        room_w=room_size[0], room_d=room_size[1],
        room_h=room_size[2] if len(room_size) > 2 else 3,
    )

    text = _call_llm(TIER3_PLACE_PROMPT, user_prompt, timeout=120.0)
    if text is None:
        return {"error": f"tier{tier} 回退绝对坐标 LLM 调用失败"}

    layouts = _parse_llm_json(text)
    if layouts is None:
        return {"error": f"tier{tier} 回退 LLM 输出解析失败"}

    items = _apply_layout(list(items), layouts)

    # 合并: 已锁定的 actors (有 pos) + 新 items
    locked_actors = intermediate.get("locked_actors", [])
    existing_locked = [
        {"object_id": a["name"], "name": a["name"], "pos": a["pos"],
         "rot": a.get("rot", [0,0,0]), "scale": a.get("scale", [1,1,1])}
        for a in locked_actors if a.get("name")
    ]
    merged = existing_locked + items

    parsed = _run_place_scene(merged, state)
    if parsed is None:
        return {"error": f"tier{tier} 回退 place_scene 失败"}

    actors = parsed.get("actors", [])
    prev_names = intermediate.get("imported_names", set())
    imported, import_failed = _import_actors(actors, scene_name, skip_names=prev_names)
    imported_names = prev_names | {a["name"] for a in imported if a.get("name")}

    logger.info("tier%d_place: 回退绝对坐标 %d 件 → %s", tier, len(actors), parsed["scene_path"])
    return {
        "intermediate": {
            f"tier{tier}_items": items,
            "locked_actors": actors,
            "scene_json_path": parsed["scene_path"],
            "scene_actors": actors,
            "current_tier": tier,
            f"tier{tier}_retry_count": intermediate.get(f"tier{tier}_retry_count", 0),
            f"tier{tier}_retry_actors": None,
            f"tier{tier}_feedback": "",
            "imported_actors": imported,
            "failed_actors": import_failed,
            "imported_names": imported_names,
        },
    }


def _place_fallback_absolute(
    state: Dict[str, Any],
    items: List[Dict[str, Any]],
    locked: List[Dict[str, Any]],
    room_size: List[float],
) -> Optional[List[Dict[str, Any]]]:
    """锚点失败时回退: LLM 为单个 item 生成绝对坐标。"""
    if not items:
        return None

    prompt = f"""你是室内设计师。以下从属物体的锚点放置失败（参考物体可能已被移除）。
请为它们生成绝对坐标。

房间: {room_size[0]}×{room_size[1]}×{room_size[2] if len(room_size) > 2 else 3}m
已有物体:
{_format_locked_actors(locked)}

需放置的物体:
{_format_items(items)}

输出 JSON 数组（绝对坐标）:
[{{"object_id": "...", "pos": [x,y,z], "rot": [rx,ry,rz], "scale": [sx,sy,sz], "reason": "..."}}]"""

    fallback_sys = "你是室内设计师。为物体生成绝对坐标。只输出 JSON 数组，不要其他文字。"
    text = _call_llm(fallback_sys, prompt, timeout=120.0)
    if text is None:
        return None

    layouts = _parse_llm_json(text)
    if layouts is None:
        return None

    result = _apply_layout(list(items), layouts)
    return result


def _run_place_scene(
    items: List[Dict[str, Any]],
    state: Dict[str, Any],
) -> Optional[Dict[str, Any]]:
    """调用 place_scene_from_items 并返回 parsed result。"""
    metadata = state.get("metadata", {})
    room_size = metadata.get("room_size", [5, 3, 3])
    scene_name = metadata.get("scene_name", "composed_scene")
    scene_path = metadata.get("scene_path", f"Scene/{scene_name}/{scene_name}.scene")

    tool = get_tool("place_scene_from_items")
    if tool is None:
        return None

    raw = tool.invoke({
        "scene_path": scene_path,
        "scene_name": scene_name,
        "room_size": room_size,
        "items": items,
    })
    parsed = parse_placement_result(raw)
    if parsed.get("error"):
        return None
    return parsed


# ===========================================================================
# 放置节点
# ===========================================================================

@stream_output_node("integrated", NO_OUTPUT)
def tier1_place_node(state: Dict[str, Any]) -> Dict[str, Any]:
    """Tier 1: 大件 LLM 绝对坐标 → place_scene_from_items → import。"""
    intermediate = state.get("intermediate", {})
    all_items = intermediate.get("placement_items", [])
    tier1_items = _filter_tier(all_items, 1)

    if not tier1_items:
        logger.info("tier1_place: 无大件, 跳过")
        return {"intermediate": {"tier1_items": [], "locked_actors": []}}

    metadata = state.get("metadata", {})
    room_size = metadata.get("room_size", [5, 3, 3])
    prompt = state.get("prompt", "")

    is_retry = intermediate.get("tier1_review_decision") == "fail"
    if is_retry:
        locked = intermediate.get("locked_actors", [])
        feedback = intermediate.get("tier1_feedback", "")
        problem_ids = set(intermediate.get("tier1_retry_actors", []))
        problem_items = [it for it in tier1_items if it.get("object_id") in problem_ids]
        user_prompt = TIER1_RETRY_PROMPT.format(
            room_w=room_size[0], room_d=room_size[1], room_h=room_size[2] if len(room_size) > 2 else 3,
            locked_text=_format_locked_actors(locked),
            vlm_feedback=feedback,
            problem_items_text=_format_items(problem_items),
        )
        items_to_place = problem_items
    else:
        user_prompt = TIER1_INITIAL_PROMPT.format(
            room_w=room_size[0], room_d=room_size[1], room_h=room_size[2] if len(room_size) > 2 else 3,
            x_half=room_size[0] / 2, z_half=room_size[1] / 2,
            neg_x=-room_size[0] / 2, neg_z=-room_size[1] / 2,
            n=len(tier1_items),
            items_text=_format_items(tier1_items),
            design_intent=prompt or "根据物品功能合理布局",
        )
        items_to_place = tier1_items

    sys_prompt = TIER1_RETRY_PROMPT if is_retry else TIER1_INITIAL_PROMPT
    text = _call_llm(sys_prompt, user_prompt, use_layout_model=True)
    if is_retry and text:
        _dump_llm_debug(state, 1, intermediate.get("tier1_retry_count", 0), sys_prompt, user_prompt, text)
    if text is None:
        return {"error": "tier1 LLM 调用失败"}

    relations = _parse_llm_json(text)
    if relations is None:
        return {"error": "tier1 LLM 输出解析失败"}

    # 保存原始 LLM 输出文本, 稍后 dump
    _llm_raw_text = text

    asset_meta = intermediate.get("asset_metadata", {})
    if is_retry:
        # retry: 也使用语义关系 → Constraint Solver
        # locked 物体的 pos 从 intermediate.locked_actors 传入 solver 作为 placed 参考
        locked_actors = intermediate.get("locked_actors", [])
        placed_refs = {a["name"]: {"pos": a["pos"], "scale": a.get("scale", [1,1,1])} for a in locked_actors if a.get("name")}
        logger.info("tier1_retry: LLM 原始输出 (前200字): %s", (_llm_raw_text or "")[:200])
        logger.info("tier1_retry: placed_refs keys=%s", list(placed_refs.keys()))
        solved = _solve_and_apply(items_to_place, relations, room_size, asset_meta, placed=placed_refs)
        if solved:
            items_to_place = solved
            logger.info("tier1_retry: solver 成功, %d 个物体", len(solved))
        else:
            logger.warning("tier1_retry: solver 失败! 回退 _apply_layout, LLM 输出前200字: %s", (_llm_raw_text or "")[:200])
            items_to_place = _apply_layout(items_to_place, relations)
            logger.info("tier1_retry: _apply_layout 后 items pos=%s", [it.get("pos") for it in items_to_place])
    else:
        # 初始布局: LLM 输出语义关系 → Constraint Solver 计算坐标
        solved = _solve_and_apply(tier1_items, relations, room_size, asset_meta)
        items_to_place = solved if solved else _apply_layout(tier1_items, relations)

    _apply_default_scale(items_to_place)
    _validate_positions(items_to_place, room_size)
    _resolve_tier_overlaps(items_to_place, asset_meta)

    # 合并: 已锁定的 + 新布局的
    if is_retry:
        problem_ids = set(intermediate.get("tier1_retry_actors", []))
        # 用 locked_actors (含 pos) 而非 tier1_items (无 pos)
        locked_actors = intermediate.get("locked_actors", [])
        locked = [
            {"object_id": a["name"], "name": a["name"], "pos": a["pos"],
             "rot": a.get("rot", [0,0,0]), "scale": a.get("scale", [1,1,1])}
            for a in locked_actors
            if a.get("name") and a["name"] not in problem_ids
        ]
        merged = locked + items_to_place
    else:
        merged = items_to_place

    parsed = _run_place_scene(merged, state)
    if parsed is None:
        return {"error": "tier1 place_scene_from_items 失败"}

    # debug dump (必须在 _run_place_scene 之后, scene_json_path 可用)
    state["intermediate"]["scene_json_path"] = parsed["scene_path"]
    _dump_layout_result(state, 1, intermediate.get("tier1_retry_count", 0), items_to_place, is_retry)
    _dump_raw_llm_output(state, 1, intermediate.get("tier1_retry_count", 0), _llm_raw_text, is_retry)

    scene_json_path = parsed["scene_path"]
    actors = parsed.get("actors", [])
    scene_name = metadata.get("scene_name", "composed_scene")
    # retry 时先清理当前 tier 的旧 actor，防止重复导入累积
    if is_retry:
        _cleanup_tier_actors(tier1_items, scene_name)
    imported, import_failed = _import_actors(actors, scene_name)
    imported_names = {a["name"] for a in imported if a.get("name")}
    # 大件全部落地, 物理沉降到 Y=0
    _apply_physics_settlement(scene_name, [a["name"] for a in imported if a.get("status") == "success"])

    logger.info("tier1_place: %s %d 大件 → %s", "差量修正" if is_retry else "初始布局", len(actors), scene_json_path)
    return {
        "intermediate": {
            "tier1_items": merged,
            "locked_actors": actors,
            "scene_json_path": scene_json_path,
            "scene_actors": actors,
            "scene_name": scene_name,
            "current_tier": 1,
            "tier1_retry_count": intermediate.get("tier1_retry_count", 0),
            "tier1_retry_actors": None,
            "tier1_feedback": "",
            "imported_actors": imported,
            "failed_actors": import_failed,
            "imported_names": imported_names,
        },
    }


@stream_output_node("integrated", NO_OUTPUT)
def tier2_place_node(state: Dict[str, Any]) -> Dict[str, Any]:
    """Tier 2: 从属锚点放置。LLM 输出空间关系 → place_object_near 计算坐标。"""
    intermediate = state.get("intermediate", {})
    all_items = intermediate.get("placement_items", [])
    tier2_items = _filter_tier(all_items, 2)

    if not tier2_items:
        logger.info("tier2_place: 无从属, 跳过")
        return {"intermediate": {"tier2_items": [], "current_tier": 2}}

    locked = intermediate.get("locked_actors", [])
    metadata = state.get("metadata", {})
    room_size = metadata.get("room_size", [5, 3, 3])
    scene_name = metadata.get("scene_name", "composed_scene")
    asset_meta = intermediate.get("asset_metadata", {})

    # 只读场景事实优先来自 native scene snapshot；旧宿主由集中兼容入口回退。
    scene = _resolve_tier2_scene(scene_name)
    if scene is None:
        logger.warning("tier2_place: 无法获取场景, 回退绝对坐标")
        return _place_with_absolute_coords(state, 2, tier2_items, TIER2_PLACE_PROMPT)

    is_retry = intermediate.get("tier2_review_decision") == "fail"
    if is_retry:
        feedback = intermediate.get("tier2_feedback", "")
        problem_ids = set(intermediate.get("tier2_retry_actors", []))
        problem_items = [it for it in tier2_items if it.get("object_id") in problem_ids]
        user_prompt = TIER2_RETRY_PROMPT.format(
            locked_text=_format_locked_actors(locked),
            vlm_feedback=feedback,
            problem_items_text=_format_items(problem_items),
        )
    else:
        user_prompt = TIER2_PLACE_PROMPT.format(
            locked_text=_format_locked_actors(locked),
            items_text=_format_items(tier2_items),
        )

    sys_prompt2 = TIER2_RETRY_PROMPT if is_retry else TIER2_PLACE_PROMPT
    text = _call_llm(sys_prompt2, user_prompt, timeout=120.0)
    if is_retry and text:
        _dump_llm_debug(state, 2, intermediate.get("tier2_retry_count", 0), sys_prompt2, user_prompt, text)
    if text is None:
        return {"error": "tier2 LLM 锚点生成失败"}

    anchors = _parse_llm_json(text)
    if anchors is None:
        return {"error": "tier2 LLM 输出解析失败"}

    # 用语义关系直接计算坐标 (不依赖 place_object_near / AABB)
    new_items = []
    placed_count = 0
    for anchor in anchors:
        oid = anchor.get("object_id", "")
        ref = anchor.get("reference_actor", "")
        rel = _normalize_relation(anchor.get("relation", ""))
        if rel is None:
            logger.warning("tier2: %s relation 无法识别: '%s'", oid, anchor.get("relation", "")[:40])
            continue
        dist = float(anchor.get("distance_m", anchor.get("gap_m", 0.3)))

        item = next((it for it in tier2_items if str(it.get("object_id", "")) == oid), None)
        if item is None:
            logger.warning("tier2: %s 无对应 item", oid)
            continue

        # 语义计算: 用参考物 position + scale 估算, 不需要 AABB
        pos = _calculate_semantic_position(ref, rel, dist, scene)
        if pos is None:
            logger.warning("tier2: %s 参考物 '%s' 未找到", oid, ref)
            continue

        item_with_pos = dict(item)
        scl = item.get("scale", [1, 1, 1])
        item_with_pos["scale"] = scl
        item_with_pos["rot"] = [0, 0, 0]

        # 碰撞检测: 与已放置物体重叠时尝试偏移
        # locked 中的 item 可能不带 scale, 从 scene actor 获取真实 scale
        all_existing = []
        for ex in locked + new_items:
            ex_copy = dict(ex)
            if "scale" not in ex_copy or not ex_copy["scale"]:
                name = ex_copy.get("name", ex_copy.get("object_id", ""))
                try:
                    actor = scene.find_actor(name) if scene else None
                    if actor:
                        ex_copy["scale"] = list(actor.get_scale())
                except Exception:
                    pass
            if "scale" not in ex_copy:
                ex_copy["scale"] = [1, 1, 1]
            all_existing.append(ex_copy)

        if _check_overlap(pos, scl, all_existing, asset_meta=asset_meta, new_name=oid):
            for offset in [0.3, 0.6, -0.3, -0.6, 0.9]:
                test_pos = [pos[0] + offset, pos[1], pos[2]]
                if not _check_overlap(test_pos, scl, all_existing, asset_meta=asset_meta, new_name=oid):
                    pos = test_pos
                    logger.info("tier2: %s 碰撞偏移 +%.1fm", oid, offset)
                    break

        item_with_pos["pos"] = pos
        new_items.append(item_with_pos)
        placed_count += 1
        logger.info("tier2: %s = %s.%s + %.2fm → [%.2f, %.2f, %.2f]",
                    oid, ref, rel, dist, pos[0], pos[1], pos[2])

    # 合并 tier1 (locked, 有 pos) + tier2 (new) → place_scene_from_items → 更新 scene.json
    _apply_default_scale(new_items)
    # 用 locked_actors (有 pos) 而非 _filter_tier(all_items) (无 pos)
    locked_with_pos = [
        {"object_id": a["name"], "name": a["name"], "pos": a["pos"],
         "rot": a.get("rot", [0,0,0]), "scale": a.get("scale", [1,1,1])}
        for a in locked if a.get("name")
    ]
    merged = locked_with_pos + new_items
    parsed = _run_place_scene(merged, state)
    prev_names = intermediate.get("imported_names", set())
    if parsed:
        scene_json_path = parsed["scene_path"]
        actors = parsed.get("actors", [])
        if is_retry:
            _cleanup_tier_actors(tier2_items, scene_name)
            # 清理后从 skip 集合中移除当前 tier 的名字，否则重导入会被跳过
            cleaned_names = {it.get("name", it.get("object_id", "")) for it in tier2_items}
            prev_names = prev_names - cleaned_names
        imported, import_failed = _import_actors(actors, scene_name, skip_names=prev_names)
        locked_updated = actors
        _apply_physics_settlement(scene_name, [a["name"] for a in imported if a.get("status") == "success"])
    else:
        scene_json_path = intermediate.get("scene_json_path", "")
        locked_updated = locked
        imported, import_failed = [], []

    imported_names = prev_names | {a["name"] for a in imported if a.get("name")}
    logger.info("tier2_place: %d/%d 语义放置成功", placed_count, len(tier2_items))
    return {
        "intermediate": {
            "tier2_items": new_items,
            "tier2_placed_count": placed_count,
            "locked_actors": locked_updated,
            "scene_json_path": scene_json_path,
            "scene_actors": locked_updated,
            "current_tier": 2,
            "tier2_retry_count": intermediate.get("tier2_retry_count", 0),
            "tier2_retry_actors": None,
            "tier2_feedback": "",
            "imported_actors": imported,
            "failed_actors": import_failed,
            "imported_names": imported_names,
        },
    }


@stream_output_node("integrated", NO_OUTPUT)
def tier3_place_node(state: Dict[str, Any]) -> Dict[str, Any]:
    """Tier 3: 装饰物绝对坐标 (和 tier1 同模式)。"""
    intermediate = state.get("intermediate", {})
    all_items = intermediate.get("placement_items", [])
    tier3_items = _filter_tier(all_items, 3)

    if not tier3_items:
        logger.info("tier3_place: 无装饰物, 跳过")
        return {"intermediate": {"tier3_items": [], "current_tier": 3}}

    locked = intermediate.get("locked_actors", [])
    metadata = state.get("metadata", {})
    room_size = metadata.get("room_size", [5, 3, 3])
    scene_name = metadata.get("scene_name", "composed_scene")

    is_retry = intermediate.get("tier3_review_decision") == "fail"
    if is_retry:
        feedback = intermediate.get("tier3_feedback", "")
        problem_ids = set(intermediate.get("tier3_retry_actors", []))
        problem_items = [it for it in tier3_items if it.get("object_id") in problem_ids]
        user_prompt = TIER3_RETRY_PROMPT.format(
            locked_text=_format_locked_actors(locked),
            vlm_feedback=feedback,
            problem_items_text=_format_items(problem_items),
        )
        items_to_place = problem_items
    else:
        user_prompt = TIER3_PLACE_PROMPT.format(
            locked_text=_format_locked_actors(locked),
            n=len(tier3_items),
            items_text=_format_items(tier3_items),
            room_w=room_size[0], room_d=room_size[1], room_h=room_size[2] if len(room_size) > 2 else 3,
        )
        items_to_place = tier3_items

    sys_prompt3 = TIER3_RETRY_PROMPT if is_retry else TIER3_PLACE_PROMPT
    text = _call_llm(sys_prompt3, user_prompt, timeout=120.0)
    if is_retry and text:
        _dump_llm_debug(state, 3, intermediate.get("tier3_retry_count", 0), sys_prompt3, user_prompt, text)
    if text is None:
        return {"error": "tier3 LLM 调用失败"}

    layouts = _parse_llm_json(text)
    if layouts is None:
        return {"error": "tier3 LLM 输出解析失败"}

    items_to_place = _apply_layout(items_to_place, layouts)
    _apply_default_scale(items_to_place)
    _validate_positions(items_to_place, room_size)
    asset_meta = intermediate.get("asset_metadata", {})
    _resolve_tier_overlaps(items_to_place, asset_meta, locked=locked)

    # 合并已有 + 新装饰
    if is_retry:
        problem_ids = set(intermediate.get("tier3_retry_actors", []))
        kept = [it for it in tier3_items if it.get("object_id") not in problem_ids]
        new_tier3 = kept + items_to_place
    else:
        new_tier3 = items_to_place

    # 用 locked_actors (有 pos) 而非 _filter_tier(all_items) (无 pos)
    locked_with_pos = [
        {"object_id": a["name"], "name": a["name"], "pos": a["pos"],
         "rot": a.get("rot", [0,0,0]), "scale": a.get("scale", [1,1,1])}
        for a in locked if a.get("name")
    ]
    final_items = locked_with_pos + new_tier3

    parsed = _run_place_scene(final_items, state)
    if parsed is None:
        return {"error": "tier3 place_scene_from_items 失败"}

    scene_json_path = parsed["scene_path"]
    actors = parsed.get("actors", [])
    prev_names = intermediate.get("imported_names", set())
    if is_retry:
        _cleanup_tier_actors(tier3_items, scene_name)
        cleaned_names = {it.get("name", it.get("object_id", "")) for it in tier3_items}
        prev_names = prev_names - cleaned_names
    imported, import_failed = _import_actors(actors, scene_name, skip_names=prev_names)
    imported_names = prev_names | {a["name"] for a in imported if a.get("name")}
    _apply_physics_settlement(scene_name, [a["name"] for a in imported if a.get("status") == "success"])

    logger.info("tier3_place: %s %d 装饰 → %s", "差量修正" if is_retry else "初始", len(new_tier3), scene_json_path)
    return {
        "intermediate": {
            "tier3_items": new_tier3,
            "locked_actors": actors,
            "scene_json_path": scene_json_path,
            "scene_actors": actors,
            "current_tier": 3,
            "tier3_retry_count": intermediate.get("tier3_retry_count", 0),
            "tier3_retry_actors": None,
            "tier3_feedback": "",
            "imported_actors": imported,
            "failed_actors": import_failed,
        },
    }


def _extract_calculated_position(raw: Any) -> Optional[Dict[str, Any]]:
    """从 place_object_near 的 envelope 中提取 calculated_position。"""
    try:
        if isinstance(raw, str):
            raw = json.loads(raw)
        llm_parts = (raw.get("llm_content") or [{}])[0].get("part")
        parts = llm_parts if llm_parts is not None else raw.get("result", {}).get("parts", [])
        for part in parts:
            text = part.get("content_text", "")
            if text:
                inner = json.loads(text)
                if inner.get("calculated_position"):
                    return {
                        "position": inner["calculated_position"],
                        "rotation": inner.get("rotation", [0, 0, 0]),
                        "scale": inner.get("scale", [1, 1, 1]),
                    }
    except Exception:
        pass
    return None


_VALID_RELATIONS = {"left", "right", "front", "behind", "above", "below"}

_RELATION_ALIASES = {
    "左侧": "left", "左": "left", "左边": "left",
    "右侧": "right", "右": "right", "右边": "right",
    "前方": "front", "前": "front", "前面": "front",
    "后方": "behind", "后": "behind", "后面": "behind",
    "上方": "above", "上": "above", "上面": "above",
    "下方": "below", "下": "below", "下面": "below",
    "-x": "left", "+x": "right", "-z": "front", "+z": "behind",
    "-y": "below", "+y": "above",
}


def _normalize_relation(raw: str) -> Optional[str]:
    """将 LLM 可能输出的自然语言映射到枚举值。无法映射时返回 None (触发 fallback)。"""
    if not raw:
        return None
    clean = raw.strip().lower()
    if clean in _VALID_RELATIONS:
        return clean
    for alias, value in _RELATION_ALIASES.items():
        if alias in raw:
            logger.info("_normalize_relation: '%s' → '%s'", raw[:30], value)
            return value
    logger.warning("_normalize_relation: 无法映射 '%s', 返回 None 触发 fallback", raw[:50])
    return None

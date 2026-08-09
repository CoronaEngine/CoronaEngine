"""
Multi-Scene Parallel Generation — 工作流节点
"""
from __future__ import annotations

import json
import logging
import os
import shutil
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Any, Dict, List

from Quasar.ai_workflow.progress import publish_node_entries_event
from Quasar.ai_workflow.state import (
    WorkflowState,
    deep_merge_dict,
)
from Quasar.ai_workflow.streaming import build_node_dialogue_entry, stream_output_node

from .constants import (
    CHECKPOINT_SCHEMA,
    CHECKPOINT_VERSION,
    MAX_PARALLEL_SCENES,
    PARALLEL_GENERATE_FUNCTION_ID,
    PARALLEL_GENERATE_V2_FUNCTION_ID,
    TIMEOUTS,
    make_child_session_id,
)
from .progress import ParallelProgressTracker
from .paths import resolve_multi_scene_output_dir

logger = logging.getLogger(__name__)

# 延迟导入，避免循环依赖
_generate_terrain_for_outdoor = None

def _get_terrain_classifier():
    global _generate_terrain_for_outdoor
    if _generate_terrain_for_outdoor is None:
        from ..terrain_generation_workflow.classifier import (
            generate_terrain_for_outdoor_scenes,
        )
        _generate_terrain_for_outdoor = generate_terrain_for_outdoor_scenes
    return _generate_terrain_for_outdoor

# stream_output_node 的 no-op formatter（节点内部自行发布事件）
NO_OUTPUT = lambda _state, _updates: []

# ---------------------------------------------------------------------------
# Decompose Node
# ---------------------------------------------------------------------------

DECOMPOSE_SYSTEM_PROMPT = """你是场景拆解专家。将用户的高层空间/建筑需求拆解为独立的子场景。

输出格式（严格 JSON 数组，不要输出其他内容）:
[
  {"scene_name": "大堂", "scene_prompt": "Design a luxury hotel lobby with marble floors, grand chandelier, reception desk, and lounge seating area. European classical style."},
  {"scene_name": "客房", "scene_prompt": "Create a 5-star hotel guest room with king-size bed, velvet drapes, gold-trimmed furniture, and a marble bathroom."}
]

核心规则:
1. scene_name 必须简短（≤10 个字符），用于文件命名
2. scene_prompt 必须详细（≥50 个字符），包含风格、元素、材质、布局
3. 最多拆解 5 个子场景（避免超时）
4. 只输出 JSON 数组，不要输出解释文字

地形场景拆解规则（重要）:
- 当用户描述的是一个自然地形场景（草原/沙漠/山脉/森林/海滩等），没有明确列出子区域时，
  你必须主动拆解为 2-3 个不同特征的子场景，每个子场景有不同的地形变化:
  * 示例输入: "草原"
  * 示例输出: [
      {"scene_name": "平坦草原", "scene_prompt": "一片开阔平坦的草原区域，低矮起伏，主要是低地草和中坡草覆盖，适合搭建蒙古包和放牧。地表有少量灌木点缀，远处是连绵缓坡。"},
      {"scene_name": "丘陵草坡", "scene_prompt": "草原上的丘陵地带，地形有明显起伏，高地区域植被以高草和灌木为主，有几处岩石露头。坡面朝南，适合建造瞭望台。"},
      {"scene_name": "河畔草地", "scene_prompt": "草原边缘的河畔区域，地势低洼平坦，靠近水源。地面以低地草为主，湿润处有花丛分布。适合搭建帐篷营地和钓鱼点。"}
    ]
  * 每个子场景的 scene_prompt 必须包含具体的地形描述词（平坦/起伏/缓坡/陡坡/低洼/高地），
    这样后面的地形生成模块才能为每个子场景生成不同的地形

- 当用户明确列出了具体区域（如"蒙古包和河边帐篷"），按用户描述拆解，不用自动扩展

室内场景拆解规则:
- 当用户描述室内空间（卧室/客厅/办公室/酒店等），按功能区域拆解
- 如果用户只说了单一室内类型（如"卧室"），返回 1 个元素即可"""


@stream_output_node("integrated", NO_OUTPUT)
def decompose_node(state: WorkflowState) -> Dict[str, Any]:
    """用 LLM 将用户的高层需求拆解为子场景列表。"""
    # 确保用户配置已生效（兜底：warmup 可能用默认配置跑过）
    from Quasar.ai_config.ai_config import reload_ai_config
    from Quasar.ai_tools.registry import get_tool_registry
    reload_ai_config()
    get_tool_registry().reset_discovery()
    from Quasar.ai_tools.load_tools import load_tools
    from Quasar.ai_config.ai_config import get_ai_config
    _cfg = get_ai_config()
    load_tools(_cfg)
    logger.info("[parallel] tools reloaded: hunyuan3d.enable=%s", getattr(_cfg.hunyuan3d, 'enable', 'N/A'))

    user_input = state.get("prompt", "") or state.get("raw_user_input", "")

    logger.info("[parallel] decompose: user_input='%s'", user_input[:100])

    try:
        from Quasar.ai_modules.text_generate.tools.client_openai import (
            build_openai_chat,
        )
        from Quasar.ai_config.ai_config import get_ai_config
        from Quasar.ai_modules.providers.configs.dataclasses import (
            ProviderConfig,
        )

        config = get_ai_config()
        chat_cfg = config.chat
        provider_name = chat_cfg.provider
        provider_raw = config.providers.get(provider_name) if hasattr(config, 'providers') else None
        if provider_raw is None:
            raise RuntimeError(f"Provider '{provider_name}' not found in config")

        provider = (
            provider_raw
            if isinstance(provider_raw, ProviderConfig)
            else ProviderConfig(
                name=provider_name,
                type=getattr(provider_raw, 'type', 'openai-compatible'),
                api_key=getattr(provider_raw, 'api_key', ''),
                base_url=getattr(provider_raw, 'base_url', ''),
            )
        )

        llm = build_openai_chat(
            provider=provider,
            model=chat_cfg.model,
            temperature=0.3,
            request_timeout=30.0,
        )

        response = llm.invoke([
            {"role": "system", "content": DECOMPOSE_SYSTEM_PROMPT},
            {"role": "user", "content": user_input},
        ])

        text = response.content if hasattr(response, 'content') else str(response)
        text = text.strip()
        # 剥离可能的 markdown code fences
        if text.startswith("```"):
            lines = text.split("\n")
            text = "\n".join(lines[1:])
            if text.endswith("```"):
                text = text[:-3]
            text = text.strip()

        sub_scenes = json.loads(text)
        if not isinstance(sub_scenes, list) or len(sub_scenes) == 0:
            raise ValueError(f"expected non-empty array, got {type(sub_scenes)}")

        logger.info("[parallel] decompose: %d sub-scenes", len(sub_scenes))

    except Exception as e:
        logger.warning("[parallel] decompose failed, fallback to single scene: %s", e)
        sub_scenes = [{"scene_name": "main", "scene_prompt": user_input}]

    return {
        "intermediate": {
            "sub_scenes": sub_scenes,
        },
    }


# ---------------------------------------------------------------------------
# Fork + Generate Node (Phase 1)
# ---------------------------------------------------------------------------

def _resolve_model_paths_from_state(sub_state: Dict[str, Any]) -> List[str]:
    """从子工作流完成后的 state 中提取模型本地路径。"""
    paths: List[str] = []
    model_results = (
        sub_state.get("global_assets", {})
        .get("model_retrieval", {})
        .get("model_results", [])
    )
    for row in model_results:
        mp = row.get("model_path", "")
        if mp:
            from ..model_retrieval_workflow.helpers import (
                resolve_model_file,
            )
            resolved = resolve_model_file(mp)
            if resolved:
                paths.append(resolved)
    return paths


def _run_phase1_for_scene(
    sub_scene: Dict[str, Any],
    parent_state: Dict[str, Any],
    index: int,
) -> Dict[str, Any]:
    """在子线程中执行 Phase 1（multi_scene + model_retrieval）。"""
    from ..full_pipeline_workflow.nodes import _make_sub_state
    from ..integrated_multi_scene_workflow import (
        WORKFLOWS as MS_WORKFLOWS,
        MULTI_SCENE_FUNCTION_ID,
    )
    from ..model_retrieval_workflow import (
        WORKFLOWS as MR_WORKFLOWS,
        MODEL_RETRIEVAL_FUNCTION_ID,
    )

    scene_name = sub_scene["scene_name"]
    scene_prompt = sub_scene["scene_prompt"]
    parent_session = parent_state.get("session_id", "default")
    child_session = make_child_session_id(parent_session, scene_name, index)

    logger.info("[parallel] fork[%d] scene='%s' session=%s", index, scene_name, child_session)

    # 构造子状态（复用 full_pipeline 的 _make_sub_state）
    sub_state = _make_sub_state(parent_state, MULTI_SCENE_FUNCTION_ID)
    sub_state["session_id"] = child_session
    sub_state["prompt"] = scene_prompt
    # Hunyuan3D 已返回多视角预览图，跳过六视角截图以节省 GPU 资源
    sub_state.setdefault("metadata", {})["skip_six_view_capture"] = True

    # 补齐缺失字段
    parent_output = parent_state.get("intermediate", {}).get("output_dir", "")
    sub_output = str(resolve_multi_scene_output_dir(parent_output, child_session, scene_name))
    os.makedirs(sub_output, exist_ok=True)
    sub_state["intermediate"]["output_dir"] = sub_output

    # 如果有 governor，注入子场景的 scene_ctx
    governor = parent_state.get("__governor__")
    if governor and hasattr(governor, 'create_scene'):
        try:
            sub_state["__scene_ctx__"] = governor.create_scene(child_session)
        except Exception as e:
            logger.warning("[parallel] governor.create_scene failed: %s", e)
        sub_state["__governor__"] = governor

    # Phase 1: multi_scene → model_retrieval
    ms_graph = MS_WORKFLOWS[MULTI_SCENE_FUNCTION_ID]
    mr_graph = MR_WORKFLOWS[MODEL_RETRIEVAL_FUNCTION_ID]

    result = ms_graph.invoke(sub_state)

    # 自动绕过 human_review：如果工作流停在审核节点，提取元素后自动批准
    if result.get("awaiting_review"):
        review_data = result.get("intermediate", {}).get("human_review", {})
        elements = review_data.get("elements", [])
        logger.info(
            "[parallel] fork[%d] auto-approving human_review: %d elements for '%s'",
            index, len(elements), scene_name,
        )
        result["approved_elements"] = elements
        result["metadata"] = dict(result.get("metadata", {}))
        result["metadata"]["resume_from_review"] = True
        result["awaiting_review"] = False
        # 重新调用以继续执行
        result = ms_graph.invoke(result)

    # 补齐 generated_images（图片生成工具不可用时，走文本到3D）
    ga = result.get("global_assets", {})
    ms = ga.get("multi_scene", {})
    generated_images = ms.get("generated_images") or {}
    approved = ms.get("approved_elements") or []
    if not generated_images and approved:
        image_prompts = {}
        for elem in approved:
            name = elem.get("item_name", "")
            prompt = elem.get("image_prompt", "")
            if name and prompt:
                # __text_to_3d__: 前缀 → dispatch 保留 item → generate_single_item 用 text_to_3d 模式
                image_prompts[name] = f"__text_to_3d__:{prompt}"
            elif name:
                image_prompts[name] = f"__text_to_3d__:{name}"
        if image_prompts:
            result.setdefault("global_assets", {})
            result["global_assets"].setdefault("multi_scene", {})
            result["global_assets"]["multi_scene"]["generated_images"] = image_prompts
            logger.info(
                "[parallel] fork[%d] injected text-to-3d prompts: %d items for '%s'",
                index, len(image_prompts), scene_name,
            )

    result = mr_graph.invoke(result)

    # 等待所有后台 3D 模型下载完成，消除异步竞态
    _wait_all_mesh_downloads(result, scene_name)

    return {
        "child_session": child_session,
        "scene_name": scene_name,
        "state": result,
        "success": True,
    }


def _wait_all_mesh_downloads(result: Dict[str, Any], scene_name: str) -> None:
    """遍历模型结果，等待所有后台异步下载完成。"""
    model_results = (
        result.get("global_assets", {})
        .get("model_retrieval", {})
        .get("model_results", [])
    )
    overall_deadline = time.time() + 300.0  # 单个 fork 最多等 5 分钟
    pending_count = 0
    for row in model_results:
        param = row.get("parameter", {})
        if param.get("has_mesh_pending"):
            if time.time() > overall_deadline:
                logger.warning("[parallel] fork '%s' mesh download wait timeout, giving up on remaining", scene_name)
                break
            pending_count += 1
            mp = row.get("model_path", "")
            oid = param.get("object_id") or param.get("folder_object_id") or ""
            from ..model_retrieval_workflow.helpers import wait_mesh_then_resolve_model_file
            resolved = wait_mesh_then_resolve_model_file(
                raw_model_path=mp,
                wait_object_id=oid,
                has_mesh_pending=True,
                retry_times=15,
                retry_interval_seconds=1.0,
            )
            if resolved:
                row["model_path"] = resolved
                param["has_mesh_pending"] = False
    if pending_count:
        logger.info("[parallel] fork '%s' waited for %d pending mesh downloads", scene_name, pending_count)


def _push_progress(
    session_id: str,
    tracker: ParallelProgressTracker,
    highlight_scenes: List[str],
) -> None:
    """向界面推送并行生成进度事件。"""
    p = tracker.overall_progress
    total = p["total"]
    done = p["completed"]
    pending = total - done
    succeeded = [n for n, s in p["scenes"].items() if s["status"] == "success"]
    failed = [(n, s.get("error", "?")) for n, s in p["scenes"].items() if s["status"] == "failed"]

    if done == 0:
        text = f"开始并行为 {total} 个子场景生成图片与3D模型..."
    elif pending > 0:
        text = f"进度 {done}/{total} | 完成: {', '.join(succeeded[-3:])} | 剩余 {pending} 个场景处理中..."
    else:
        text = f"全部 {total} 个子场景的图片与3D模型生成完毕"

    if failed:
        text += f"\n⚠ 失败: {', '.join(f'{n}({e})' for n, e in failed)}"

    entry = build_node_dialogue_entry(
        "integrated",
        [{"content_type": "text", "content_text": text}],
        node_name="fork_generate",
        function_id=PARALLEL_GENERATE_FUNCTION_ID,
    )
    try:
        publish_node_entries_event(session_id, "fork_generate", [entry])
    except Exception:
        pass  # 进度推送失败不阻塞主流程


# ---------------------------------------------------------------------------
# Classify & Generate Terrain Node (indoor/outdoor)
# ---------------------------------------------------------------------------

@stream_output_node("integrated", NO_OUTPUT)
def classify_and_generate_terrain_node(state: WorkflowState) -> Dict[str, Any]:
    """对 decompose 产生的子场景分类 indoor/outdoor, outdoor 场景生成地形。

    分类规则:
      1. 关键词快速匹配 (中文/英文 indoor/outdoor 词表)
      2. 无法匹配 → LLM 批量分类
      3. outdoor → 匹配地形预设 (草原/沙漠/...) → 调用 terrain_generator
      4. indoor → 跳过, 不做任何地形处理

    产出:
      intermediate.classified_scenes: 带 scene_type 标记的子场景列表
      global_assets.terrain: {scene_name: terrain_result} 供 scene_composition 使用
    """
    sub_scenes: List[Dict[str, Any]] = state.get("intermediate", {}).get("sub_scenes", [])

    if not sub_scenes:
        logger.warning("[classify_terrain] no sub_scenes to classify")
        return {"intermediate": {"classified_scenes": []}}

    output_base = state.get("intermediate", {}).get("output_dir", "")

    try:
        classify_fn = _get_terrain_classifier()
        result = classify_fn(sub_scenes, dict(state), output_base)
    except Exception as e:
        logger.exception("[classify_terrain] failed: %s", e)
        for sc in sub_scenes:
            sc["scene_type"] = "indoor"
            sc["terrain_keyword"] = None
        result = {
            "intermediate": {"classified_scenes": sub_scenes, "terrain_results": {}},
            "global_assets": {},
        }

    # 推送进度
    session_id = str(state.get("session_id", "default") or "default")
    classified = result.get("intermediate", {}).get("classified_scenes", sub_scenes)
    terrain_results = result.get("intermediate", {}).get("terrain_results", {})
    outdoor_count = sum(1 for sc in classified if sc.get("scene_type") == "outdoor")
    terrain_ok = sum(1 for r in terrain_results.values() if r.get("ok"))

    status_text = f"场景分类完成: {len(classified)} 个子场景"
    if outdoor_count > 0:
        status_text += f" ({outdoor_count} 个室外, {terrain_ok} 个地形已生成)"

    entry = build_node_dialogue_entry(
        "integrated",
        [{"content_type": "text", "content_text": status_text}],
        node_name="classify_terrain",
        function_id=PARALLEL_GENERATE_FUNCTION_ID,
    )
    try:
        publish_node_entries_event(session_id, "classify_terrain", [entry])
    except Exception:
        pass

    logger.info("[classify_terrain] done: %d outdoor, %d terrain generated",
                outdoor_count, terrain_ok)

    return result


@stream_output_node("integrated", NO_OUTPUT)
def fork_generate_node(state: WorkflowState) -> Dict[str, Any]:
    """Phase 1: 并行执行所有子场景的 multi_scene + model_retrieval。"""
    sub_scenes = state.get("intermediate", {}).get("sub_scenes", [])

    # 预检
    if len(sub_scenes) > MAX_PARALLEL_SCENES:
        logger.warning(
            "[parallel] sub_scenes count %d exceeds limit %d, truncating",
            len(sub_scenes), MAX_PARALLEL_SCENES,
        )
        sub_scenes = sub_scenes[:MAX_PARALLEL_SCENES]

    names = [s.get("scene_name", "") for s in sub_scenes]
    if len(set(names)) < len(names):
        logger.warning("[parallel] duplicate scene names: %s", names)

    if not sub_scenes:
        return {"error": "decompose 未产生子场景"}

    tracker = ParallelProgressTracker(len(sub_scenes))
    parent_state = dict(state)  # plain dict for thread safety
    session_id = str(state.get("session_id", "default") or "default")
    results: List[Dict[str, Any]] = []

    max_workers = min(len(sub_scenes), MAX_PARALLEL_SCENES)
    logger.info("[parallel] fork_generate: %d scenes, %d workers", len(sub_scenes), max_workers)

    # 向界面推送启动事件
    _push_progress(session_id, tracker, list(sc["scene_name"] for sc in sub_scenes))

    with ThreadPoolExecutor(max_workers=max_workers) as pool:
        futures = {}
        for i, sc in enumerate(sub_scenes):
            tracker.mark_started(sc.get("scene_name", f"scene_{i}"))
            future = pool.submit(_run_phase1_for_scene, sc, parent_state, i)
            futures[future] = (i, sc)

        for future in as_completed(futures):
            i, sc = futures[future]
            scene_name = sc.get("scene_name", f"scene_{i}")
            try:
                result = future.result(timeout=TIMEOUTS["single_scene_total"])
                results.append(result)
                tracker.mark_completed(scene_name)
                logger.info("[parallel] fork[%d] '%s' completed", i, scene_name)
            except Exception as e:
                logger.exception("[parallel] fork[%d] '%s' failed: %s", i, scene_name, e)
                results.append({
                    "child_session": make_child_session_id(
                        parent_state.get("session_id", "default"), scene_name, i,
                    ),
                    "scene_name": scene_name,
                    "state": {},
                    "success": False,
                    "error": str(e),
                })
                tracker.mark_failed(scene_name, str(e))

            # 每个子场景完成后向界面推送进度
            _push_progress(session_id, tracker, [scene_name])

    logger.info("[parallel] fork_generate done: %s", tracker.summary())

    return {
        "intermediate": {
            "phase1_results": results,
            "phase1_summary": tracker.summary(),
        },
    }


# ---------------------------------------------------------------------------
# Checkpoint (Phase 1 → Phase 2)
# ---------------------------------------------------------------------------

def serialize_checkpoint(phase1_results: List[Dict[str, Any]], output_dir: str) -> str:
    """将 Phase 1 结果落盘为 checkpoint JSON。

    只保存 CHECKPOINT_SCHEMA 定义的字段，mesh 文件复制到本地缓存。
    """
    checkpoint: Dict[str, Any] = {
        "schema_version": CHECKPOINT_VERSION,
        "created_at": time.time(),
        "scenes": [],
    }

    for result in phase1_results:
        if not result.get("success"):
            continue

        state = result.get("state", {})
        scene_name = result.get("scene_name", "unknown")
        scene_output = state.get("intermediate", {}).get("output_dir", output_dir)

        # 解析本地 mesh 路径
        local_mesh_paths = _resolve_model_paths_from_state(state)

        # 缓存 mesh 到本地（硬链接优先，回退到复制）
        mesh_cache_dir = os.path.join(output_dir, scene_name, "mesh_cache")
        os.makedirs(mesh_cache_dir, exist_ok=True)
        cached_paths: List[str] = []
        for idx, src in enumerate(local_mesh_paths):
            ext = os.path.splitext(src)[1] or ".glb"
            dst = os.path.join(mesh_cache_dir, f"{scene_name}_model_{idx:03d}{ext}")
            if not os.path.exists(dst):
                try:
                    os.link(src, dst)  # 硬链接（零拷贝）
                except (OSError, NotImplementedError):
                    shutil.copy2(src, dst)  # 回退复制
            cached_paths.append(dst)

        checkpoint["scenes"].append({
            "child_session": result.get("child_session", ""),
            "scene_name": scene_name,
            "output_dir": scene_output,
            "local_mesh_paths": cached_paths,
            "scene_center": state.get("intermediate", {}).get("scene_center"),
            "camera_distance": state.get("intermediate", {}).get("camera_distance"),
        })

    checkpoint_path = os.path.join(output_dir, "phase1_checkpoint.json")
    with open(checkpoint_path, "w", encoding="utf-8") as f:
        json.dump(checkpoint, f, indent=2, ensure_ascii=False)

    logger.info(
        "[parallel] checkpoint saved: %s (%d scenes, %d bytes)",
        checkpoint_path, len(checkpoint["scenes"]),
        os.path.getsize(checkpoint_path),
    )
    return checkpoint_path


def restore_from_checkpoint(
    checkpoint_path: str, parent_state: Dict[str, Any]
) -> List[Dict[str, Any]]:
    """从 checkpoint JSON 恢复 Phase 1 结果，验证本地文件，重建状态。"""
    with open(checkpoint_path, "r", encoding="utf-8") as f:
        checkpoint = json.load(f)

    version = checkpoint.get("schema_version")
    if version != CHECKPOINT_VERSION:
        raise RuntimeError(
            f"Checkpoint schema version mismatch: {version} != {CHECKPOINT_VERSION}. "
            f"Please re-run from scratch."
        )

    from ..full_pipeline_workflow.nodes import (
        _make_sub_state,
    )

    phase1_results: List[Dict[str, Any]] = []
    for scene in checkpoint.get("scenes", []):
        # 验证本地 mesh 文件
        mesh_paths = scene.get("local_mesh_paths", [])
        valid_paths = [p for p in mesh_paths if os.path.exists(p)]
        if len(valid_paths) < len(mesh_paths):
            logger.warning(
                "[parallel] checkpoint restore: %d/%d mesh files missing for '%s'",
                len(mesh_paths) - len(valid_paths), len(mesh_paths),
                scene.get("scene_name"),
            )
        if not valid_paths:
            logger.error(
                "[parallel] checkpoint restore: no valid mesh files for '%s', skipping",
                scene.get("scene_name"),
            )
            continue

        # 重建最小 state
        sub_state = _make_sub_state(parent_state, 21003)  # SCENE_COMPOSITION_FUNCTION_ID
        sub_state["session_id"] = scene["child_session"]
        sub_state["intermediate"].update({
            "output_dir": scene.get("output_dir", ""),
            "local_mesh_paths": valid_paths,
            "scene_center": scene.get("scene_center"),
            "camera_distance": scene.get("camera_distance"),
        })

        # 重建 scene_ctx（引擎重启后旧的已失效）
        governor = parent_state.get("__governor__")
        if governor and hasattr(governor, 'create_scene'):
            try:
                sub_state["__scene_ctx__"] = governor.create_scene(scene["child_session"])
            except Exception as e:
                logger.warning("[parallel] restore: governor.create_scene failed: %s", e)
            sub_state["__governor__"] = governor

        phase1_results.append({
            "child_session": scene["child_session"],
            "scene_name": scene["scene_name"],
            "state": sub_state,
            "success": True,
        })

    logger.info(
        "[parallel] checkpoint restored: %d scenes from %s",
        len(phase1_results), checkpoint_path,
    )
    return phase1_results


# ---------------------------------------------------------------------------
# Serial Compose Node (Phase 2)
# ---------------------------------------------------------------------------

def _resolve_composition_workflow_for_parallel(state):
    """根据 function_id 选择场景组装版本。"""
    fid = state.get("function_id", 0)
    if fid == PARALLEL_GENERATE_V2_FUNCTION_ID:
        from ..scene_composition_workflow_v2 import (
            WORKFLOWS as _SC_W,
            SCENE_COMPOSITION_V2_FUNCTION_ID as _SC_F,
        )
        return _SC_W, _SC_F, "v2"
    from ..scene_composition_workflow import (
        WORKFLOWS as _SC_W,
        SCENE_COMPOSITION_FUNCTION_ID as _SC_F,
    )
    return _SC_W, _SC_F, "v1"


def serial_compose_node(state: WorkflowState) -> Dict[str, Any]:
    """Phase 2: 串行执行每个子场景的 scene_composition。"""
    SC_WORKFLOWS, SCENE_COMPOSITION_FUNCTION_ID, _sc_ver = _resolve_composition_workflow_for_parallel(state)

    # output_dir: 优先用 intermediate 里的，否则用临时目录
    import tempfile
    output_dir = state.get("intermediate", {}).get("output_dir", "") or tempfile.mkdtemp(prefix="parallel_")
    checkpoint_path = os.path.join(output_dir, "phase1_checkpoint.json")

    # 尝试从 checkpoint 恢复
    if os.path.exists(checkpoint_path):
        logger.info("[parallel] resuming from checkpoint: %s", checkpoint_path)
        try:
            phase1_results = restore_from_checkpoint(checkpoint_path, dict(state))
        except Exception as e:
            logger.warning("[parallel] checkpoint restore failed, falling back: %s", e)
            phase1_results = state.get("intermediate", {}).get("phase1_results", [])
    else:
        phase1_results = state.get("intermediate", {}).get("phase1_results", [])
        # 在 Phase 2 开始前写入 checkpoint（防止中途崩溃白费 Phase 1）
        try:
            serialize_checkpoint(phase1_results, output_dir)
        except Exception as e:
            logger.warning("[parallel] checkpoint write failed (non-fatal): %s", e)

    composed: List[Dict[str, Any]] = []
    failed: List[Dict[str, Any]] = []

    for result in phase1_results:
        if not result.get("success"):
            continue

        scene_name = result.get("scene_name", "unknown")
        sub_state = result.get("state", {})

        if not sub_state:
            logger.warning("[parallel] compose: empty state for '%s', skipping", scene_name)
            continue

        logger.info("[parallel] compose: starting '%s'", scene_name)

        # 注入场景名到 metadata，确保每个子场景写出独立的 scene.json
        sub_state.setdefault("metadata", {})["scene_name"] = scene_name
        sub_state.setdefault("metadata", {})["scene_path"] = (
            f"Scene/{scene_name}/{scene_name}.scene"
        )

        try:
            sc_graph = SC_WORKFLOWS[SCENE_COMPOSITION_FUNCTION_ID]
            final = sc_graph.invoke(sub_state)

            scene_path = (
                final.get("global_assets", {})
                .get("scene_composition", {})
                .get("scene_path", "")
            )
            composed.append({
                "scene_name": scene_name,
                "scene_path": scene_path,
                "child_session": result.get("child_session", ""),
            })
            logger.info("[parallel] compose: '%s' done → %s", scene_name, scene_path)

            # 导入地形/地板 mesh
            terrain_results = state.get("global_assets", {}).get("terrain", {})
            terrain_data = terrain_results.get(scene_name, {})
            terrain_obj = terrain_data.get("files", {}).get("obj", "")
            # 检测地板类型: 读 config 文件内容
            is_indoor = False
            preset_floor_size = 10.0
            config_path = terrain_data.get("files", {}).get("config", "")
            if config_path and os.path.exists(config_path):
                try:
                    with open(config_path, "r", encoding="utf-8") as _f:
                        _cfg = json.loads(_f.read())
                        is_indoor = _cfg.get("type") == "indoor_floor"
                        if is_indoor:
                            preset_floor_size = _cfg.get("size", 10.0)
                except Exception:
                    pass

            if terrain_obj and os.path.exists(terrain_obj):
                try:
                    from ..scene_composition_workflow.helpers import get_tool

                    pos_y = 0.0
                    final_obj = terrain_obj

                    # ---- 室内地板：固定大尺寸铺满整个区域 ----
                    if is_indoor:
                        scene_actors = final.get("intermediate", {}).get("scene_actors", [])
                        # 取物体位置的包围盒
                        min_x = min_z = float("inf")
                        max_x = max_z = float("-inf")
                        min_y = float("inf")
                        for actor in scene_actors:
                            geo = actor.get("geometry", {})
                            pos = geo.get("pos", [0, 0, 0])
                            min_x = min(min_x, pos[0])
                            max_x = max(max_x, pos[0])
                            min_z = min(min_z, pos[2])
                            max_z = max(max_z, pos[2])
                            min_y = min(min_y, pos[1])

                        pos_y = -0.5
                        if scene_actors:
                            pos_y = min_y - 0.5
                            # 用物体跨度 + 大 margin，再取 sqrt 保证地板足够大
                            span = max(max_x - min_x, max_z - min_z, 4.0)
                            half = max(span * 1.5, 12.0)  # 至少 24m 见方，确保铺满所有物体
                        else:
                            half = 12.0

                        terrain_dir = os.path.dirname(terrain_obj)
                        prompt_text = ""
                        for sc in state.get("intermediate", {}).get("classified_scenes", []):
                            if sc.get("scene_name") == scene_name:
                                prompt_text = sc.get("scene_prompt", "")
                                break
                        from ..terrain_generation_workflow.terrain_generator import (
                            generate_room_box,
                            _resolve_indoor_floor_style,
                            INDOOR_FLOOR_STYLES,
                        )
                        style = _resolve_indoor_floor_style(prompt_text)
                        style_def = INDOOR_FLOOR_STYLES.get(style, INDOOR_FLOOR_STYLES["wood_warm"])
                        try:
                            result_floor = generate_room_box(
                                terrain_dir,
                                min_x=-half, max_x=half,
                                min_y=0.0, max_y=style_def["wall_height"],
                                min_z=-half, max_z=half,
                                style=style,
                            )
                            final_obj = result_floor["files"]["obj"]
                            logger.info("[parallel] floor: %s %.0fx%.0fm style=%s y=%.2f",
                                        scene_name, half*2, half*2,
                                        style_def.get("label", style), pos_y)
                        except Exception as fe:
                            logger.warning("[parallel] floor generation failed: %s", fe)

                    tool = get_tool("import_model")
                    if tool:
                        tool.invoke({
                            "model_path": final_obj,
                            "actor_name": f"Terrain_{scene_name}",
                            "position": [0, pos_y, 0],
                            "rotation": [0, 0, 0],
                            "scale": [1, 1, 1],
                            "scene_name": scene_name,
                        })
                        logger.info("[parallel] %s imported: '%s' → %s (y=%.2f)",
                                    "indoor floor" if is_indoor else "terrain",
                                    scene_name, final_obj, pos_y)
                    else:
                        logger.warning("[parallel] import_model tool not available, terrain not imported")
                except Exception as te:
                    logger.warning("[parallel] terrain import failed for '%s': %s", scene_name, te)
            elif terrain_obj:
                logger.warning("[parallel] terrain obj not found: %s", terrain_obj)

        except Exception as e:
            logger.exception("[parallel] compose: '%s' failed: %s", scene_name, e)
            failed.append({"scene_name": scene_name, "error": str(e)})

    # 序列化 checkpoint（Phase 1 结果 + 已完成的 compose 标记）
    if output_dir:
        try:
            serialize_checkpoint(phase1_results, output_dir)
        except Exception as e:
            logger.warning("[parallel] checkpoint serialize failed: %s", e)

    return {
        "intermediate": {
            "composed_scenes": composed,
            "failed_compose": failed,
        },
    }


# ---------------------------------------------------------------------------
# Aggregate Node
# ---------------------------------------------------------------------------

@stream_output_node("integrated", NO_OUTPUT)
def aggregate_node(state: WorkflowState) -> Dict[str, Any]:
    """汇总所有子场景结果，生成最终输出。"""
    intermediate = state.get("intermediate", {})
    composed = intermediate.get("composed_scenes", [])
    failed_compose = intermediate.get("failed_compose", [])
    phase1_results = intermediate.get("phase1_results", [])
    phase1_summary = intermediate.get("phase1_summary", "")

    # 合并 global_assets
    merged_assets: Dict[str, Any] = {}
    for result in phase1_results:
        if result.get("success"):
            merged_assets = deep_merge_dict(
                merged_assets,
                result.get("state", {}).get("global_assets", {}),
            )

    # 构建汇总文本
    total = len(composed) + len(failed_compose)
    lines = [f"多场景并行生成完成 ({total} 个子场景)"]
    if composed:
        paths = [f"{c['scene_name']} → {c.get('scene_path', 'N/A')}" for c in composed]
        lines.append("成功: " + "; ".join(paths))
    if failed_compose:
        fails = [f"{f['scene_name']}({f.get('error', 'unknown')})" for f in failed_compose]
        lines.append("失败: " + "; ".join(fails))
    if phase1_summary:
        lines.append(f"Phase1: {phase1_summary}")

    summary = "\n".join(lines)
    logger.info("[parallel] aggregate: %s", summary)

    return {
        "global_assets": merged_assets,
        "dialogue_entries": [{
            "role": "assistant",
            "interface_type": "integrated",
            "sent_time_stamp": int(time.time()),
            "part": [{"content_type": "text", "content_text": summary}],
            "parameter": {
                "parallel_result": {
                    "total": total,
                    "succeeded": len(composed),
                    "failed": len(failed_compose),
                    "scenes": composed + failed_compose,
                },
            },
        }],
    }

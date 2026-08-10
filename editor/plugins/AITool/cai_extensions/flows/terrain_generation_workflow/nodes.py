"""
Terrain Generation Workflow — LangGraph 节点
"""
from __future__ import annotations

import json
import logging
import time
from typing import Any, Dict

from Quasar.ai_workflow.progress import publish_node_entries_event
from Quasar.ai_workflow.state import WorkflowState
from Quasar.ai_workflow.streaming import build_node_dialogue_entry, stream_output_node

from .constants import (
    ANALYZER_SYSTEM_PROMPT,
    DEFAULT_RESOLUTION,
    PRESET_DESERT,
    PRESET_GRASSLAND,
    TERRAIN_GENERATE_FUNCTION_ID,
)
from .terrain_generator import generate_terrain
from .paths import resolve_terrain_output_dir

logger = logging.getLogger(__name__)

NO_OUTPUT = lambda _state, _updates: []

_BUILTIN_PRESETS: Dict[str, Dict[str, Any]] = {
    "grassland": PRESET_GRASSLAND,
    "草原": PRESET_GRASSLAND,
    "desert": PRESET_DESERT,
    "沙漠": PRESET_DESERT,
}


@stream_output_node("integrated", NO_OUTPUT)
def analyze_params_node(state: WorkflowState) -> Dict[str, Any]:
    user_input = state.get("prompt", "") or state.get("raw_user_input", "")
    logger.info("[terrain] analyze_params: user_input='%s'", user_input[:100])

    for keyword, preset in _BUILTIN_PRESETS.items():
        if keyword in user_input.lower():
            logger.info("[terrain] matched builtin preset: %s", keyword)
            return {"intermediate": {"terrain_config": preset, "terrain_source": "preset"}}

    terrain_config = None
    try:
        from Quasar.ai_config.ai_config import reload_ai_config, get_ai_config
        from Quasar.ai_tools.registry import get_tool_registry
        reload_ai_config()
        get_tool_registry().reset_discovery()
        from Quasar.ai_tools.load_tools import load_tools
        cfg = get_ai_config()
        load_tools(cfg)
        from Quasar.ai_modules.text_generate.tools.client_openai import build_openai_chat
        from Quasar.ai_config.ai_config import get_ai_config as _get_cfg
        from Quasar.ai_modules.providers.configs.dataclasses import ProviderConfig

        config = _get_cfg()
        chat_cfg = config.chat
        provider_name = chat_cfg.provider
        provider_raw = config.providers.get(provider_name) if hasattr(config, 'providers') else None
        if provider_raw is None:
            raise RuntimeError(f"Provider '{provider_name}' not found")
        provider = (
            provider_raw if isinstance(provider_raw, ProviderConfig)
            else ProviderConfig(
                name=provider_name,
                type=getattr(provider_raw, 'type', 'openai-compatible'),
                api_key=getattr(provider_raw, 'api_key', ''),
                base_url=getattr(provider_raw, 'base_url', ''),
            )
        )
        llm = build_openai_chat(provider=provider, model=chat_cfg.model, temperature=0.3, request_timeout=60.0)
        response = llm.invoke([
            {"role": "system", "content": ANALYZER_SYSTEM_PROMPT},
            {"role": "user", "content": user_input},
        ])
        text = response.content if hasattr(response, 'content') else str(response)
        text = text.strip()
        if text.startswith("```"):
            lines = text.split("\n")
            text = "\n".join(lines[1:])
            if text.endswith("```"):
                text = text[:-3]
            text = text.strip()
        terrain_config = json.loads(text)
        logger.info("[terrain] LLM generated config for: %s", terrain_config.get("scene_name", "?"))
    except Exception as e:
        logger.warning("[terrain] LLM failed, fallback to grassland preset: %s", e)
        terrain_config = PRESET_GRASSLAND

    return {"intermediate": {"terrain_config": terrain_config, "terrain_source": "llm"}}


@stream_output_node("integrated", NO_OUTPUT)
def generate_terrain_node(state: WorkflowState) -> Dict[str, Any]:
    terrain_config = state.get("intermediate", {}).get("terrain_config", PRESET_GRASSLAND)
    resolution = DEFAULT_RESOLUTION
    session_id = state.get("session_id", "default")
    output_dir = str(resolve_terrain_output_dir(session_id=session_id))

    logger.info("[terrain] generating: scene='%s', res=%d, output=%s",
                terrain_config.get("scene_name", "?"), resolution, output_dir)

    try:
        publish_node_entries_event(
            state.get("session_id", ""),
            [{"type": "status", "text": f"正在生成地形: {terrain_config.get('scene_name', '')}..."}],
            "generate_terrain",
        )
    except Exception:
        pass

    t_start = time.time()
    result = generate_terrain(terrain_config, output_dir, resolution)
    elapsed = time.time() - t_start
    logger.info("[terrain] generation complete in %.1fs: stats=%s", elapsed, result["stats"])

    return {"intermediate": {"terrain_result": result, "terrain_output_dir": output_dir}}


@stream_output_node("integrated", NO_OUTPUT)
def output_result_node(state: WorkflowState) -> Dict[str, Any]:
    result = state.get("intermediate", {}).get("terrain_result", {})
    config = state.get("intermediate", {}).get("terrain_config", {})
    files = result.get("files", {})
    stats = result.get("stats", {})
    scene_name = config.get("scene_name", "地形")

    text_parts = (
        f"## {scene_name} — 地形生成完成\n"
        f"- **分辨率**: {stats.get('resolution', '?')}×{stats.get('resolution', '?')}\n"
        f"- **海拔范围**: {stats.get('height_range', [0,0])[0]}-{stats.get('height_range', [0,0])[1]}m\n"
        f"- **面数**: {stats.get('faces', 0):,}\n"
        f"- **植被点**: {stats.get('veg_count', 0)}\n"
        f"- **水体**: {stats.get('water_count', 0)} (接口已预留)\n"
        f"- **耗时**: {stats.get('generation_time_s', 0)}s\n\n"
        f"### 输出文件:\n"
        f"- 高度图: `{files.get('heightmap', '')}`\n"
        f"- 材质图: `{files.get('splatmap', '')}`\n"
        f"- 植被分布: `{files.get('veg_scatter', '')}`\n"
        f"- OBJ 网格: `{files.get('obj', '')}`\n"
        f"- 配置快照: `{files.get('config', '')}`\n"
    )
    dialogue = build_node_dialogue_entry(
        "integrated",
        [{"content_type": "text", "content_text": text_parts}],
        node_name="output_result",
        function_id=TERRAIN_GENERATE_FUNCTION_ID,
    )

    return {
        "dialogue_entries": [dialogue],
        "intermediate": {"terrain_complete": True},
        "output_parts": [{"type": "terrain", "scene_name": scene_name, "files": files, "stats": stats}],
    }

"""
Scene classifier — indoor / outdoor detection + terrain generation trigger.
"""
from __future__ import annotations

import json
import logging
import time
from typing import Any, Dict, List, Optional

from .constants import PRESET_DESERT, PRESET_GRASSLAND

logger = logging.getLogger(__name__)

_INDOOR_KEYWORDS = [
    "卧室", "客厅", "厨房", "卫生间", "浴室", "书房", "办公室",
    "会议室", "走廊", "大厅", "室内", "房间", "公寓", "酒店房间",
    "bedroom", "living room", "kitchen", "bathroom", "office",
    "meeting room", "corridor", "hall", "indoor", "room", "apartment",
    "地下室", "阁楼", "餐厅", "酒吧", "咖啡馆",
]

_OUTDOOR_KEYWORDS = [
    "草原", "沙漠", "山脉", "森林", "海滩", "山谷", "雪地",
    "河岸", "户外", "野外", "公园", "花园", "庭院", "广场",
    "grassland", "desert", "mountain", "forest", "beach", "valley",
    "snow", "riverbank", "outdoor", "park", "garden", "plaza",
    "高原", "丘陵", "沼泽", "湖边", "海岸", "田野", "牧场",
    "营地", "悬崖", "火山", "冰川",
]

_TERRAIN_PRESETS: Dict[str, Dict[str, Any]] = {
    "草原": PRESET_GRASSLAND, "grassland": PRESET_GRASSLAND,
    "沙漠": PRESET_DESERT, "desert": PRESET_DESERT,
}


def _classify_scene_type(scene_prompt: str) -> str:
    prompt_lower = scene_prompt.lower()
    indoor_score = sum(1 for kw in _INDOOR_KEYWORDS if kw in prompt_lower)
    outdoor_score = sum(1 for kw in _OUTDOOR_KEYWORDS if kw in prompt_lower)
    if outdoor_score > indoor_score:
        return "outdoor"
    elif indoor_score > outdoor_score:
        return "indoor"
    return "indoor"


def _extract_terrain_keyword(scene_prompt: str) -> Optional[str]:
    for keyword in _TERRAIN_PRESETS:
        if keyword in scene_prompt.lower() or keyword in scene_prompt:
            return keyword
    return None


def _classify_via_llm(sub_scenes: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    unmatched = [{"index": i, "scene_name": sc.get("scene_name", ""),
                  "scene_prompt": sc.get("scene_prompt", "")}
                 for i, sc in enumerate(sub_scenes) if sc.get("scene_type") is None]
    if not unmatched:
        return sub_scenes
    try:
        from Quasar.ai_config.ai_config import reload_ai_config, get_ai_config
        from Quasar.ai_tools.registry import get_tool_registry
        from Quasar.ai_tools.load_tools import load_tools
        reload_ai_config()
        get_tool_registry().reset_discovery()
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
        provider = (provider_raw if isinstance(provider_raw, ProviderConfig)
                    else ProviderConfig(name=provider_name, type=getattr(provider_raw, 'type', 'openai-compatible'),
                                        api_key=getattr(provider_raw, 'api_key', ''),
                                        base_url=getattr(provider_raw, 'base_url', '')))
        llm = build_openai_chat(provider=provider, model=chat_cfg.model, temperature=0.1, request_timeout=20.0)

        scenes_text = "\n".join(f"{i}. {u['scene_name']}: {u['scene_prompt'][:200]}" for i, u in enumerate(unmatched))
        response = llm.invoke([
            {"role": "system", "content": (
                "你是一个场景分类器。判断每个场景是室内(indoor)还是室外(outdoor)。"
                "室内: 有墙壁、天花板、地板的封闭空间(卧室、客厅、办公室等)。"
                "室外: 露天、自然地形、户外环境(草原、沙漠、森林、街道等)。"
                "输出严格JSON数组: [{\"index\": 0, \"scene_type\": \"indoor\"}, ...]"
            )},
            {"role": "user", "content": scenes_text},
        ])
        text = response.content if hasattr(response, 'content') else str(response)
        text = text.strip()
        if text.startswith("```"):
            lines = text.split("\n")
            text = "\n".join(lines[1:])
            if text.endswith("```"):
                text = text[:-3]
            text = text.strip()
        llm_results = json.loads(text)
        for item in llm_results:
            idx = item.get("index", -1)
            st = item.get("scene_type", "indoor")
            if 0 <= idx < len(sub_scenes):
                sub_scenes[idx]["scene_type"] = st
                logger.info("[classifier] LLM classified '%s' -> %s", sub_scenes[idx].get("scene_name", "?"), st)
    except Exception as e:
        logger.warning("[classifier] LLM classification failed, defaulting to indoor: %s", e)
        for sc in sub_scenes:
            if sc.get("scene_type") is None:
                sc["scene_type"] = "indoor"
    return sub_scenes


def _resolve_terrain_config(terrain_keyword: Optional[str]) -> Optional[Dict[str, Any]]:
    if not terrain_keyword:
        return None
    for kw, preset in _TERRAIN_PRESETS.items():
        if kw in terrain_keyword.lower() or kw in terrain_keyword:
            return dict(preset)
    return None


def _seed_for_scene(scene_name: str, base_seed: int = 0) -> int:
    h = 0
    for i, ch in enumerate(scene_name):
        h = (h * 31 + ord(ch)) & 0x7FFFFFFF
    return (h + base_seed) % 100000


def _inject_scene_seed(config: Dict[str, Any], scene_name: str) -> Dict[str, Any]:
    base = config.get("terrain", {}).get("seed", 0)
    scene_seed = _seed_for_scene(scene_name, base)
    config.setdefault("terrain", {})["seed"] = scene_seed
    logger.info("[classifier] seed override: scene='%s' preset_seed=%d -> scene_seed=%d",
                scene_name, base, scene_seed)
    return config


def generate_terrain_for_outdoor_scenes(sub_scenes: List[Dict[str, Any]], state: Dict[str, Any],
                                        output_base: str = "") -> Dict[str, Any]:
    session_id = str(state.get("session_id", "default") or "default")

    for sc in sub_scenes:
        sc["scene_type"] = _classify_scene_type(sc.get("scene_prompt", ""))
        sc["terrain_keyword"] = _extract_terrain_keyword(sc.get("scene_prompt", ""))

    sub_scenes = _classify_via_llm(sub_scenes)

    for sc in sub_scenes:
        logger.info("[classifier] scene='%s' type=%s terrain_hint=%s",
                    sc.get("scene_name", "?"), sc.get("scene_type", "indoor"), sc.get("terrain_keyword") or "-")

    from .terrain_generator import generate_terrain
    from .paths import resolve_terrain_output_dir

    terrain_results: Dict[str, Any] = {}

    # ---- Outdoor: terrain generation ----
    outdoor_scenes = [sc for sc in sub_scenes if sc.get("scene_type") == "outdoor"]
    if not outdoor_scenes:
        logger.info("[classifier] no outdoor scenes, terrain skipped")

    for sc in outdoor_scenes:
        scene_name = sc.get("scene_name", "outdoor")
        terrain_keyword = sc.get("terrain_keyword")
        config = _resolve_terrain_config(terrain_keyword)
        if config is None:
            logger.info("[classifier] no terrain preset for '%s', skipping terrain for '%s'",
                        terrain_keyword, scene_name)
            continue
        try:
            if output_base:
                output_dir = str(resolve_terrain_output_dir(output_base, session_id, scene_name))
            else:
                output_dir = str(resolve_terrain_output_dir(session_id=session_id, scene_name=scene_name))

            _inject_scene_seed(config, scene_name)
            logger.info("[classifier] generating terrain for '%s': preset=%s seed=%d output=%s",
                        scene_name, terrain_keyword, config["terrain"].get("seed", 0), output_dir)

            t0 = time.time()
            result = generate_terrain(config, output_dir, config.get("resolution", 256))
            elapsed = time.time() - t0
            terrain_results[scene_name] = {**result, "scene_name": scene_name, "output_dir": output_dir}
            logger.info("[classifier] terrain for '%s' done in %.1fs: %d faces, %d veg",
                        scene_name, elapsed, result["stats"]["faces"], result["stats"]["veg_count"])
        except Exception as e:
            logger.exception("[classifier] terrain generation failed for '%s': %s", scene_name, e)
            terrain_results[scene_name] = {"ok": False, "error": str(e), "scene_name": scene_name}

    # ---- Indoor: hakoniwa floor generation ----
    indoor_scenes = [sc for sc in sub_scenes if sc.get("scene_type") == "indoor"]
    for sc in indoor_scenes:
        scene_name = sc.get("scene_name", "indoor")
        try:
            from .terrain_generator import (
                generate_indoor_floor,
                _resolve_indoor_floor_style,
                INDOOR_FLOOR_STYLES,
            )

            if output_base:
                output_dir = str(resolve_terrain_output_dir(output_base, session_id, scene_name))
            else:
                output_dir = str(resolve_terrain_output_dir(session_id=session_id, scene_name=scene_name))

            # Match floor style + size to scene prompt
            prompt_text = sc.get("scene_prompt", "")
            style = _resolve_indoor_floor_style(prompt_text)
            style_def = INDOOR_FLOOR_STYLES.get(style, INDOOR_FLOOR_STYLES["wood_warm"])
            size = style_def["default_size"]
            wh = style_def["wall_height"]

            prompt_lower = prompt_text.lower()
            if any(w in prompt_lower for w in ["大", "large", "big", "厅", "堂", "会议室", "hall"]):
                size = min(size * 1.5, 28.0)
            elif any(w in prompt_lower for w in ["小", "small", "tiny", "卫生间"]):
                size = max(size * 0.6, 8.0)

            logger.info("[classifier] generating indoor floor for '%s': style=%s size=%.0fm output=%s",
                        scene_name, style_def.get("label", style), size, output_dir)

            t0 = time.time()
            result = generate_indoor_floor(output_dir, size=size, wall_height=wh, style=style)
            elapsed = time.time() - t0
            terrain_results[scene_name] = {**result, "scene_name": scene_name, "output_dir": output_dir}
            logger.info("[classifier] indoor floor for '%s' done in %.1fs", scene_name, elapsed)

        except Exception as e:
            logger.exception("[classifier] indoor floor generation failed for '%s': %s", scene_name, e)
            terrain_results[scene_name] = {"ok": False, "error": str(e), "scene_name": scene_name}

    return {
        "intermediate": {"classified_scenes": sub_scenes, "terrain_results": terrain_results},
        "global_assets": {"terrain": terrain_results},
    }

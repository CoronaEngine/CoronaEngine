from __future__ import annotations

import json
import logging
from typing import Any, Dict, List

from langchain_core.messages import HumanMessage, SystemMessage

from Quasar.ai_workflow.state import MultiSceneWorkflowState
from Quasar.ai_workflow.streaming import stream_output_node

from .constants import (
    ANALYZER_MULTIMODAL_SUFFIX,
    ANALYZER_SYSTEM_PROMPT,
    FALLBACK_ELEMENTS,
)
from .formatters import NO_OUTPUT
from .helpers import analyze_images_with_vlm, clean_json_text, extract_text, get_llm
from ..workflow_test_support import get_test_case

logger = logging.getLogger(__name__)


@stream_output_node("integrated", NO_OUTPUT)
def analyzer_node(state: MultiSceneWorkflowState) -> Dict[str, Any]:
    """分析用户需求，提取结构化设计元素列表。"""
    metadata = state.get("metadata", {})
    if metadata.get("workflow_test"):
        test_case_key = metadata.get("workflow_test_case", "default")
        test_data = get_test_case(test_case_key)
        extracted = test_data.get("extracted_elements")
        if extracted:
            logger.info(
                "[Workflow][analyzer][TEST] 工作流测试模式，使用预定义样例: "
                "test_case=%s, elements=%s",
                test_case_key,
                len(extracted),
            )
            return {
                "is_multimodal": bool(state.get("images")),
                "extracted_elements": extracted,
            }

    if metadata.get("resume_from_review"):
        resumed_elements = state.get("approved_elements", []) or state.get(
            "extracted_elements", []
        )
        if resumed_elements:
            logger.info(
                "[Workflow][analyzer] 检测到 review resume，跳过分析，元素数=%s",
                len(resumed_elements),
            )
            return {
                "is_multimodal": bool(state.get("images")),
                "extracted_elements": resumed_elements,
            }

    test_prompt = ""
    if metadata.get("workflow_test"):
        test_case_key = metadata.get("workflow_test_case", "default")
        test_data = get_test_case(test_case_key)
        test_prompt = str(test_data.get("input_prompt", "") or "").strip()

    user_input = (state.get("prompt") or "").strip() or test_prompt
    if not user_input:
        return {"error": "缺少设计需求文本"}

    images = state.get("images") or []
    is_multimodal = bool(images)
    session_id = state.get("session_id", "")

    try:
        llm = get_llm(temperature=0.6)
        system_text = ANALYZER_SYSTEM_PROMPT
        user_content = f"用户需求：{user_input}"

        if is_multimodal:
            vlm_analysis = analyze_images_with_vlm(images, session_id)
            if vlm_analysis:
                system_text += ANALYZER_MULTIMODAL_SUFFIX
                user_content += f"\n\n{vlm_analysis}"
            else:
                logger.info("[Workflow][analyzer] VLM 分析无结果，仅用文本分析")

        response = llm.invoke(
            [
                SystemMessage(content=system_text),
                HumanMessage(content=user_content),
            ]
        )
        raw_text = extract_text(response)

        cleaned = clean_json_text(raw_text)
        parsed = json.loads(cleaned)

        if isinstance(parsed, dict):
            for value in parsed.values():
                if isinstance(value, list):
                    parsed = value
                    break

        if not isinstance(parsed, list) or len(parsed) == 0:
            raise ValueError("解析结果不是非空数组")

        elements: List[Dict[str, str]] = []
        for item in parsed:
            elements.append(
                {
                    "item_name": str(item.get("item_name", "未命名单品")),
                    "image_prompt": str(item.get("image_prompt", "")),
                    "layout_desc": str(item.get("layout_desc", "")),
                }
            )

        logger.info("[Workflow][analyzer] 提取到 %s 个设计元素", len(elements))
        result = {
            "is_multimodal": is_multimodal,
            "extracted_elements": elements,
        }
        logger.debug("[Workflow][analyzer] 返回结果: %s", result)
        return result

    except json.JSONDecodeError as e:
        logger.warning("[Workflow][analyzer] JSON 解析失败，使用回退元素: %s", e)
        result = {
            "is_multimodal": is_multimodal,
            "extracted_elements": list(FALLBACK_ELEMENTS),
        }
        logger.debug("[Workflow][analyzer] 返回回退结果: 3个默认元素")
        return result
    except Exception as e:
        logger.error("[Workflow][analyzer] 执行异常: %s", e, exc_info=True)
        return {"error": f"方案分析失败: {e}"}

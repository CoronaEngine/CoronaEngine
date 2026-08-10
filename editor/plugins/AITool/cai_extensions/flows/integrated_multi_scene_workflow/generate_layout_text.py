from __future__ import annotations

import logging
from typing import Any, Dict, List

from Quasar.ai_workflow.state import MultiSceneWorkflowState
from Quasar.ai_workflow.streaming import stream_output_node

from .formatters import NO_OUTPUT
from ..workflow_test_support import get_test_case

logger = logging.getLogger(__name__)


@stream_output_node("integrated", NO_OUTPUT)
def generate_layout_text_node(state: MultiSceneWorkflowState) -> Dict[str, Any]:
    """将审核通过的元素格式化为物品清单与布局描述文本。"""
    metadata = state.get("metadata", {})

    if metadata.get("workflow_test"):
        test_case_key = metadata.get("workflow_test_case", "default")
        test_data = get_test_case(test_case_key)
        layout_text = test_data.get("layout_text")
        if layout_text:
            logger.info(
                "[Workflow][generate_layout_text][TEST] 工作流测试模式，使用预定义 layout_text: "
                "test_case=%s",
                test_case_key,
            )
            return {"layout_text": layout_text}

    approved = state.get("approved_elements", [])
    if not approved:
        return {"layout_text": "暂无设计元素。"}

    lines: List[str] = ["设计方案"]
    for idx, element in enumerate(approved, 1):
        name = element.get("item_name", "未命名")
        desc = element.get("layout_desc", "")
        lines.append(f"{idx}. {name}")
        if desc:
            lines.append(f"   {desc}")

    layout_text = "\n".join(lines)
    logger.info("[Workflow][generate_layout_text] 格式化完成")
    return {"layout_text": layout_text}

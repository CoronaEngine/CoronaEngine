from __future__ import annotations

import logging
import uuid
from typing import Any, Dict

from Quasar.ai_workflow.state import MultiSceneWorkflowState
from Quasar.ai_workflow.streaming import stream_output_node

from .formatters import format_human_review_parts
from ..workflow_test_support import get_test_case

logger = logging.getLogger(__name__)


@stream_output_node(
    "integrated",
    format_human_review_parts,
    node_name="human_review",
)
def human_review_node(state: MultiSceneWorkflowState) -> Dict[str, Any]:
    """Human-in-the-loop 审核节点。"""
    metadata = state.get("metadata", {})

    if metadata.get("workflow_test"):
        test_case_key = metadata.get("workflow_test_case", "default")
        test_data = get_test_case(test_case_key)
        approved = test_data.get("approved_elements")
        if approved:
            logger.info(
                "[Workflow][human_review][TEST] 工作流测试模式，使用预定义 approved_elements: "
                "test_case=%s, count=%s",
                test_case_key,
                len(approved),
            )
            return {
                "approved_elements": approved,
                "intermediate": {
                    **state.get("intermediate", {}),
                    "human_review": {
                        "status": "test_approved",
                        "batch_id": "test_batch",
                        "elements": approved,
                        "note": "工作流测试模式，使用预定义元素。",
                    },
                },
            }

    if metadata.get("resume_from_review"):
        resumed_approved = state.get("approved_elements", [])
        if resumed_approved:
            logger.info(
                "[Workflow][human_review] 检测到 review resume，直接通过已确认元素，数量=%s",
                len(resumed_approved),
            )
            return {
                "approved_elements": resumed_approved,
                "intermediate": {
                    **state.get("intermediate", {}),
                    "human_review": {
                        "status": "resumed",
                        "batch_id": state.get("metadata", {}).get(
                            "resume_batch_id", ""
                        ),
                        "elements": resumed_approved,
                        "note": "已接收前端审核结果，继续执行后续节点。",
                    },
                },
            }

    extracted = state.get("extracted_elements", [])
    if not extracted:
        return {"error": "无可审核的设计元素"}

    batch_id = str(uuid.uuid4())
    review_payload = {
        "stage": "pending",
        "review_type": "design_elements",
        "batch_id": batch_id,
        "schema_version": 1,
        "items": extracted,
    }

    try:
        from langgraph.types import interrupt  # type: ignore[import-untyped]

        interrupt_payload = {
            "action": "review_elements",
            "elements": extracted,
            "batch_id": batch_id,
            "message": "请审核以下设计元素，可修改后返回，或原样返回表示通过。",
        }
        approved = interrupt(interrupt_payload)

        if isinstance(approved, list) and len(approved) > 0:
            logger.info("[Workflow][human_review] 人工审核通过，%s 个元素", len(approved))
            return {
                "approved_elements": approved,
                "intermediate": {
                    **state.get("intermediate", {}),
                    "human_review": {
                        "status": "approved",
                        "batch_id": batch_id,
                        "elements": approved,
                        "note": "审核完成，继续执行后续节点。",
                    },
                },
            }
    except Exception as exc:
        logger.info(
            "[Workflow][human_review] interrupt 不可用或未接入 resume (%s)，改为等待前端审核提交",
            exc,
        )

    logger.info(
        "[Workflow][human_review] 已发送待审核内容，元素数量: %s, batch_id=%s",
        len(extracted),
        batch_id,
    )
    return {
        "review_payload": review_payload,
        "awaiting_review": True,
        "intermediate": {
            **state.get("intermediate", {}),
            "human_review": {
                "status": "pending",
                "batch_id": batch_id,
                "elements": extracted,
                "note": "审核请求已下发，等待前端提交确认结果。",
            },
        },
    }

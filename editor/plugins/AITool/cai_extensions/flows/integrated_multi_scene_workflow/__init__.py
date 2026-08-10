"""
第一步工作流：多物体场景设计（LangGraph DAG）

将原单节点耦合流程拆分为 5 个独立节点的 DAG：
  analyzer_node → human_review_node
      → generate_images_node      ─┐
      → generate_layout_text_node ─┤→ aggregate_result_node → END

支持：多模态输入、Human-in-the-loop 审核、并行图文生成。
保持对外接口约定（function_id、WORKFLOWS / WORKFLOW_COMMANDS 导出）。
"""

from __future__ import annotations

from typing import Dict, TYPE_CHECKING

from langgraph.graph import END, START, StateGraph

from Quasar.ai_workflow.executor import register_workflow_checkpoints
from Quasar.ai_workflow.state import MultiSceneWorkflowState

from .aggregate import aggregate_result_node
from .analyzer import analyzer_node
from .constants import MULTI_SCENE_FUNCTION_ID
from .generate_images import generate_images_node
from .generate_layout_text import generate_layout_text_node
from .human_review import human_review_node

TEST_CASES = {}

if TYPE_CHECKING:
    from langgraph.graph.state import CompiledStateGraph


def build_multi_scene_workflow() -> "CompiledStateGraph":
    """构建多场景室内设计 LangGraph DAG。"""
    graph = StateGraph(MultiSceneWorkflowState)

    graph.add_node("analyzer", analyzer_node)
    graph.add_node("human_review", human_review_node)
    graph.add_node("generate_images", generate_images_node)
    graph.add_node("generate_layout_text", generate_layout_text_node)
    graph.add_node("aggregate_result", aggregate_result_node)

    graph.add_edge(START, "analyzer")
    graph.add_edge("analyzer", "human_review")
    graph.add_edge("human_review", "generate_images")
    graph.add_edge("human_review", "generate_layout_text")
    graph.add_edge("generate_images", "aggregate_result")
    graph.add_edge("generate_layout_text", "aggregate_result")
    graph.add_edge("aggregate_result", END)

    return graph.compile()


WORKFLOWS: Dict[int, "CompiledStateGraph"] = {
    MULTI_SCENE_FUNCTION_ID: build_multi_scene_workflow(),
}

WORKFLOW_COMMANDS: Dict[str, int] = {
    "/multi_scene": MULTI_SCENE_FUNCTION_ID,
}

register_workflow_checkpoints(
    MULTI_SCENE_FUNCTION_ID,
    {"human_review", "generate_images", "aggregate_result"},
)

__all__ = [
    "TEST_CASES",
    "WORKFLOWS",
    "WORKFLOW_COMMANDS",
    "MULTI_SCENE_FUNCTION_ID",
    "build_multi_scene_workflow",
]

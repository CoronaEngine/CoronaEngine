"""
第二步工作流：模型检索与 3D 生成（LangGraph DAG）

接收第一步（多场景室内设计工作流）的输出状态，对每个物体：
  1. 优先检索已有 3D 模型，未命中时生成新模型
  2. 对生成结果执行注册、六视图采集与视觉复核
  3. 若复核失败则回到检索/生成节点重试

DAG 拓扑：
    START → dispatch_node → retrieve_node → generate_node
          → six_view_capture_tool_node → visual_review_node
          → register_node
          → format_result_node → END

保持对外接口约定（function_id、WORKFLOWS / WORKFLOW_COMMANDS 导出）。
"""

from __future__ import annotations

from typing import Dict, TYPE_CHECKING

from langgraph.graph import END, START, StateGraph

from Quasar.ai_workflow.executor import register_workflow_checkpoints
from Quasar.ai_workflow.state import ModelRetrievalWorkflowState

from .constants import MODEL_RETRIEVAL_FUNCTION_ID
from .dispatch import dispatch_node
from .format_result import format_result_node
from .generate import generate_node
from .register import register_node
from .retrieve import retrieve_node
from .six_view_capture_tool import six_view_capture_tool_node
from .visual_review import visual_review_node

TEST_CASES = {}

if TYPE_CHECKING:
    from langgraph.graph.state import CompiledStateGraph


def check_if_needs_retry(state: ModelRetrievalWorkflowState) -> str:
    """动态路由决策器。"""
    if state.get("needs_retry"):
        return "generate"
    return "register"


def build_model_retrieval_workflow() -> "CompiledStateGraph":
    """构建模型检索与生成 LangGraph DAG。"""
    graph = StateGraph(ModelRetrievalWorkflowState)

    graph.add_node("dispatch", dispatch_node)
    graph.add_node("retrieve", retrieve_node)
    graph.add_node("generate", generate_node)
    graph.add_node("register", register_node)
    graph.add_node("capture_views", six_view_capture_tool_node)
    graph.add_node("visual_review", visual_review_node)
    graph.add_node("format_result", format_result_node)

    graph.add_edge(START, "dispatch")
    graph.add_edge("dispatch", "retrieve")
    graph.add_edge("retrieve", "generate")
    graph.add_edge("generate", "capture_views")
    graph.add_edge("capture_views", "visual_review")

    graph.add_conditional_edges(
        "visual_review",
        check_if_needs_retry,
        {
            "generate": "generate",
            "register": "register",
        },
    )

    graph.add_edge("register", "format_result")

    graph.add_edge("format_result", END)
    graph.set_entry_point("dispatch")
    graph.set_finish_point("format_result")

    return graph.compile()


WORKFLOWS: Dict[int, "CompiledStateGraph"] = {
    MODEL_RETRIEVAL_FUNCTION_ID: build_model_retrieval_workflow(),
}

WORKFLOW_COMMANDS: Dict[str, int] = {
    "/model_retrieval": MODEL_RETRIEVAL_FUNCTION_ID,
}

register_workflow_checkpoints(
    MODEL_RETRIEVAL_FUNCTION_ID,
    {"retrieve", "generate", "format_result"},
)

__all__ = [
    "WORKFLOWS",
    "WORKFLOW_COMMANDS",
    "MODEL_RETRIEVAL_FUNCTION_ID",
    "build_model_retrieval_workflow",
    "TEST_CASES",
]

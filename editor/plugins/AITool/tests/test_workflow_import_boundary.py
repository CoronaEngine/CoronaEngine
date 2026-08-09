import importlib
import sys
from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[3]
AI_TOOL_ROOT = EDITOR_ROOT / "plugins" / "AITool"
QUASAR_ROOT = AI_TOOL_ROOT / "Quasar"
for path in (EDITOR_ROOT, AI_TOOL_ROOT, QUASAR_ROOT):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))


def test_active_workflows_do_not_require_deleted_test_fixture_modules():
    modules = (
        "plugins.AITool.cai_extensions.flows.integrated_multi_scene_workflow",
        "plugins.AITool.cai_extensions.flows.model_retrieval_workflow",
        "plugins.AITool.cai_extensions.flows.scene_composition_workflow",
    )

    for module_name in modules:
        module = importlib.import_module(module_name)
        assert module is not None

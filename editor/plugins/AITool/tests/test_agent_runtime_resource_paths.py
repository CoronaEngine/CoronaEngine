import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[4]
EDITOR_ROOT = PROJECT_ROOT / "editor"
AI_TOOL_ROOT = EDITOR_ROOT / "plugins" / "AITool"
for path in (PROJECT_ROOT, EDITOR_ROOT, AI_TOOL_ROOT):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))


from plugins.AITool.services.agent_runtime import adapters


def test_relative_model_candidates_prefer_the_active_project_root(monkeypatch, tmp_path):
    project_root = tmp_path / "ActiveProject"
    model_path = project_root / "Resource" / "generated" / "model.obj"
    model_path.parent.mkdir(parents=True)
    model_path.write_text("o model\n", encoding="utf-8")
    monkeypatch.setattr(
        "runtime.project_context.get_project_root",
        lambda: project_root,
    )

    candidates = adapters._visible_model_path_candidates(
        "Resource/generated/model.obj"
    )

    assert candidates[0] == model_path


def test_agent_runtime_tools_use_project_context_for_relative_models():
    tools_source = (
        AI_TOOL_ROOT / "services" / "agent_runtime" / "tools.py"
    ).read_text(encoding="utf-8")

    assert "project_context.get_project_root()" in tools_source
    assert "Path.cwd() / path_text" not in tools_source


def test_absolute_model_candidates_are_not_rebased(monkeypatch, tmp_path):
    absolute_model = tmp_path / "absolute.obj"
    absolute_model.write_text("o model\n", encoding="utf-8")
    monkeypatch.setattr(
        "runtime.project_context.get_project_root",
        lambda: tmp_path / "OtherProject",
    )

    candidates = adapters._visible_model_path_candidates(str(absolute_model))

    assert candidates == [absolute_model]

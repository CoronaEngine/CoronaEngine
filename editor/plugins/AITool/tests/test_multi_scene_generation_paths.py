import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[4]
EDITOR_ROOT = PROJECT_ROOT / "editor"
AI_TOOL_ROOT = EDITOR_ROOT / "plugins" / "AITool"
for path in (PROJECT_ROOT, EDITOR_ROOT, AI_TOOL_ROOT):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))


from cai_extensions.flows.multi_scene_parallel_workflow.paths import (
    resolve_multi_scene_output_dir,
)


def test_default_multi_scene_output_belongs_to_active_project(monkeypatch, tmp_path):
    project_root = tmp_path / "ActiveProject"
    project_root.mkdir()
    monkeypatch.setattr(
        "runtime.project_context.get_project_root",
        lambda: project_root,
    )

    output = resolve_multi_scene_output_dir("", "session-1", "大厅")

    assert output == project_root / "Resource" / "generated" / "multi_scene" / "session-1" / "大厅"


def test_explicit_multi_scene_parent_remains_authoritative(tmp_path):
    parent_output = tmp_path / "custom-output"

    output = resolve_multi_scene_output_dir(str(parent_output), "session-1", "大厅")

    assert output == parent_output / "大厅"


def test_multi_scene_workflow_delegates_default_path_to_the_owner():
    workflow_root = (
        PROJECT_ROOT
        / "editor"
        / "plugins"
        / "AITool"
        / "cai_extensions"
        / "flows"
        / "multi_scene_parallel_workflow"
    )
    source = (workflow_root / "nodes.py").read_text(encoding="utf-8")

    assert "resolve_multi_scene_output_dir" in source
    assert "Path.cwd() / \"output\"" not in source

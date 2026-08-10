import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[4]
EDITOR_ROOT = PROJECT_ROOT / "editor"
AI_TOOL_ROOT = EDITOR_ROOT / "plugins" / "AITool"
for path in (PROJECT_ROOT, EDITOR_ROOT, AI_TOOL_ROOT):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))


from cai_extensions.flows.terrain_generation_workflow.paths import (
    resolve_terrain_output_dir,
)


def test_default_terrain_output_belongs_to_active_project(monkeypatch, tmp_path):
    project_root = tmp_path / "ActiveProject"
    project_root.mkdir()
    monkeypatch.setattr(
        "runtime.project_context.get_project_root",
        lambda: project_root,
    )

    output = resolve_terrain_output_dir("", "session-1", "草原")

    assert output == project_root / "Resource" / "generated" / "terrain" / "session-1" / "草原" / "terrain"


def test_explicit_terrain_output_base_remains_authoritative(tmp_path):
    output_base = tmp_path / "custom-output"

    output = resolve_terrain_output_dir(str(output_base), "session-1", "草原")

    assert output == output_base / "草原" / "terrain"


def test_terrain_workflows_delegate_default_paths_to_the_owner():
    workflow_root = (
        PROJECT_ROOT
        / "editor"
        / "plugins"
        / "AITool"
        / "cai_extensions"
        / "flows"
        / "terrain_generation_workflow"
    )
    nodes_source = (workflow_root / "nodes.py").read_text(encoding="utf-8")
    classifier_source = (workflow_root / "classifier.py").read_text(encoding="utf-8")

    assert "resolve_terrain_output_dir" in nodes_source
    assert "resolve_terrain_output_dir" in classifier_source
    assert "Path(__file__).resolve().parents[6] / \"output\"" not in nodes_source
    assert "Path(__file__).resolve().parents[6] / \"output\"" not in classifier_source

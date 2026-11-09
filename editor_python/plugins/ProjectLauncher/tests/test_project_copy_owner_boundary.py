from pathlib import Path

EDITOR = Path(__file__).resolve().parents[3]


def test_old_project_copy_and_template_writers_are_removed():
    assert not (EDITOR / "runtime/project_templates.py").exists()

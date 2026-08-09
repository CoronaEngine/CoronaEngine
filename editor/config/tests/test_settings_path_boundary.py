from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[2]


def test_project_settings_has_a_config_canonical_owner():
    canonical = EDITOR_ROOT / "config" / "project_state.py"
    file_manager = (
        EDITOR_ROOT
        / "plugins"
        / "FileManager"
        / "main.py"
    ).read_text(encoding="utf-8")

    assert canonical.is_file()
    assert not (EDITOR_ROOT / "config" / "settings.py").is_file()
    assert not any(
        path.is_file() and "__pycache__" not in path.parts
        for path in (EDITOR_ROOT / "utils").rglob("*")
    )
    assert "from config.settings" not in file_manager
    assert "CoronaEditorApi.files" in file_manager


def test_production_python_code_does_not_import_settings_from_utils():
    roots = (EDITOR_ROOT / "plugins", EDITOR_ROOT / "runtime", EDITOR_ROOT / "script_runtime")
    violations = []
    for root in roots:
        for path in root.rglob("*.py"):
            if "tests" in path.parts or "Quasar" in path.parts:
                continue
            source = path.read_text(encoding="utf-8")
            if "from utils.settings import" in source:
                violations.append(str(path))

    assert violations == []

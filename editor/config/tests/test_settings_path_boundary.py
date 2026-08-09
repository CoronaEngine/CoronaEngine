from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[2]


def test_project_settings_has_a_config_canonical_owner():
    canonical = EDITOR_ROOT / "config" / "settings.py"
    compatibility = EDITOR_ROOT / "utils" / "settings.py"
    file_manager = (
        EDITOR_ROOT
        / "plugins"
        / "FileManager"
        / "compat"
        / "legacy_file_manager.py"
    ).read_text(encoding="utf-8")

    assert canonical.is_file()
    assert "Compatibility" in compatibility.read_text(encoding="utf-8")
    assert "from config.settings" not in file_manager
    assert "CoronaEditorApi.files" in file_manager


def test_production_python_code_does_not_import_settings_from_utils():
    roots = (EDITOR_ROOT / "plugins", EDITOR_ROOT / "backend", EDITOR_ROOT / "CoronaCore")
    violations = []
    for root in roots:
        for path in root.rglob("*.py"):
            if "tests" in path.parts or "Quasar" in path.parts:
                continue
            source = path.read_text(encoding="utf-8")
            if "from utils.settings import" in source:
                violations.append(str(path))

    assert violations == []

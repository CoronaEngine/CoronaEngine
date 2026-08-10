from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1].parent


def test_python_service_registry_has_a_runtime_canonical_owner():
    source = (EDITOR_ROOT / "runtime" / "registry.py").read_text(encoding="utf-8")
    assert (EDITOR_ROOT / "runtime" / "registry.py").is_file()
    assert "plugins.AITool.main" in source
    assert "backend.registry" not in source


def test_runtime_registry_uses_script_runtime_for_generated_blockly():
    source = (EDITOR_ROOT / "runtime" / "registry.py").read_text(encoding="utf-8")
    assert '_BLOCKLY_PACKAGE = "script_runtime"' in source
    assert '"ScratchTool": (f"{_BLOCKLY_PACKAGE}.blockly.main", "ScratchTool")' in source


def test_active_services_have_a_separate_startup_inventory():
    source = (EDITOR_ROOT / "runtime" / "registry.py").read_text(encoding="utf-8")
    assert "ACTIVE_PYTHON_SCRIPT_SERVICES" in source
    assert "register_active_python_script_services" in source
    assert "register_legacy_python_script_services" not in source


def test_project_archive_and_aitool_are_lazy():
    source = (EDITOR_ROOT / "runtime" / "registry.py").read_text(encoding="utf-8")
    assert 'LAZY_PYTHON_SCRIPT_SERVICES = {"AITool", "ProjectArchive"}' in source

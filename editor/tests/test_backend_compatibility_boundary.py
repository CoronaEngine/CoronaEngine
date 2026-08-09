import ast
from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]
BACKEND_ROOT = EDITOR_ROOT / "backend"

EXPECTED_OWNERS = {
    "registry.py": "runtime.registry",
    "blockly/main.py": "script_runtime.blockly.main",
    "blockly/ai_node_graph_contract.py": "script_runtime.blockly.ai_node_graph_contract",
    "blockly/check_ai_contract_catalog.py": "script_runtime.blockly.check_ai_contract_catalog",
    "file_system/main.py": "plugins.FileManager.compat.legacy_file_manager",
    "project_settings/main.py": "plugins.ProjectSettings.compat.legacy_project_settings",
}


def test_backend_has_a_local_compatibility_inventory():
    inventory = BACKEND_ROOT / "COMPATIBILITY.md"

    assert inventory.is_file()
    source = inventory.read_text(encoding="utf-8")
    assert "canonical owner" in source
    assert "删除条件" in source
    for compatibility_path, canonical_owner in EXPECTED_OWNERS.items():
        assert compatibility_path in source
        assert canonical_owner in source


def test_backend_production_files_are_only_one_way_wrappers():
    for relative_path, canonical_owner in EXPECTED_OWNERS.items():
        source_path = BACKEND_ROOT / relative_path
        source = source_path.read_text(encoding="utf-8")
        tree = ast.parse(source, filename=str(source_path))

        assert "Compatibility" in source
        assert canonical_owner in source
        assert not any(
            isinstance(node, (ast.ClassDef, ast.AsyncFunctionDef))
            or isinstance(node, ast.FunctionDef)
            for node in ast.walk(tree)
        ), source_path


def test_canonical_production_code_does_not_import_backend():
    roots = (
        EDITOR_ROOT / "api",
        EDITOR_ROOT / "config",
        EDITOR_ROOT / "plugins",
        EDITOR_ROOT / "runtime",
        EDITOR_ROOT / "script_runtime",
    )
    violations = []
    for root in roots:
        for source_path in root.rglob("*.py"):
            if "tests" in source_path.parts or "__pycache__" in source_path.parts:
                continue
            tree = ast.parse(source_path.read_text(encoding="utf-8-sig"), filename=str(source_path))
            for node in ast.walk(tree):
                if isinstance(node, ast.Import):
                    modules = [alias.name for alias in node.names]
                elif isinstance(node, ast.ImportFrom):
                    modules = [node.module or ""]
                else:
                    continue
                violations.extend(
                    f"{source_path}: {module}"
                    for module in modules
                    if module == "backend" or module.startswith("backend.")
                )

    assert violations == []

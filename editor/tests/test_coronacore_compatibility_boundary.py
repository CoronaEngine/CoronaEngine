import ast
from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]
CORONA_CORE_ROOT = EDITOR_ROOT / "CoronaCore"

PATH_FAMILIES = {
    "archive": "runtime.archive",
    "core/corona_editor.py": "runtime.editor_host",
    "core/corona_engine.py": "runtime.native_engine",
    "core/editor_api.py": "api.editor_api",
    "core/engine_runtime.py": "runtime.legacy_engine_adapter",
    "core/components": "runtime.legacy.components",
    "core/entities": "runtime.legacy.entities",
    "core/managers": "runtime.legacy.managers",
    "core/legacy": "runtime.legacy",
    "core/scripts_system": "script_runtime.engine",
    "core/network_sync_policy.py": "runtime.network_sync_policy",
    "core/project_utils.py": ("runtime.project_templates", "runtime.scene_support"),
    "core/response_utils.py": "runtime.response_utils",
    "core/script_runtime_editor_api.py": "script_runtime.manifest_adapter",
    "core/legacy_editor_api.py": "script_runtime.compat.legacy_scene_datas_adapter",
    "core/legacy_scene_datas_adapter.py": "script_runtime.compat.legacy_scene_datas_adapter",
    "core/legacy_scene_store.py": "runtime.legacy_scene_store",
    "utils": "runtime.",
}


def test_coronacore_has_a_local_compatibility_inventory():
    inventory = CORONA_CORE_ROOT / "COMPATIBILITY.md"

    assert inventory.is_file()
    source = inventory.read_text(encoding="utf-8")
    assert "canonical owner" in source
    assert "删除条件" in source
    for path_family, canonical_owner in PATH_FAMILIES.items():
        assert path_family in source
        owners = canonical_owner if isinstance(canonical_owner, tuple) else (canonical_owner,)
        for owner in owners:
            assert owner in source


def test_coronacore_is_the_outer_compatibility_boundary():
    assert not (CORONA_CORE_ROOT / "compat").exists()
    source = (CORONA_CORE_ROOT / "COMPATIBILITY.md").read_text(encoding="utf-8")
    assert "不再在其中新增 `compat/` 子层" in source
    assert "一层 `CoronaCore/compat` 只会重复转发" in source


def test_coronacore_files_are_compatibility_wrappers_or_aliases():
    for source_path in CORONA_CORE_ROOT.rglob("*.py"):
        if source_path.name == "__init__.py" or "__pycache__" in source_path.parts:
            continue
        source = source_path.read_text(encoding="utf-8")
        tree = ast.parse(source, filename=str(source_path))

        assert "Compatibility" in source, source_path
        assert not any(
            isinstance(node, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef))
            for node in ast.walk(tree)
        ), source_path
        assert any(
            marker in source
            for marker in ("runtime.", "script_runtime.", "api.editor_api")
        ), source_path


def test_canonical_production_code_does_not_import_coronacore():
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
                    if module == "CoronaCore" or module.startswith("CoronaCore.")
                )

    assert violations == []

import ast
from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]

EXPECTED_OWNERS = {
    "CoronaPlugin/compat/legacy_plugin_base.py": "runtime.plugin_base",
    "CoronaPlugin/compat/legacy_load_utils.py": "runtime.plugin_loader",
    "utils/compat/legacy_settings.py": "config.settings",
    "utils/compat/legacy_logging.py": "runtime.logging",
    "scripts/compat/legacy_pack.py": "tools.pack",
}

LEGACY_WRAPPERS = {
    "CoronaPlugin/core/corona_plugin_base.py": "plugins.CoronaPlugin.compat.legacy_plugin_base",
    "CoronaPlugin/utils/load_utils.py": "plugins.CoronaPlugin.compat.legacy_load_utils",
    "utils/settings.py": "utils.compat.legacy_settings",
    "utils/logging.py": "utils.compat.legacy_logging",
    "scripts/pack.py": "editor.scripts.compat.legacy_pack",
}


def test_small_compatibility_packages_have_local_inventories():
    for package_name in ("CoronaPlugin", "utils", "scripts"):
        inventory = EDITOR_ROOT / package_name / "COMPATIBILITY.md"
        assert inventory.is_file(), inventory
        source = inventory.read_text(encoding="utf-8")
        assert "canonical owner" in source
        assert "删除条件" in source

    combined_source = "\n".join(
        (EDITOR_ROOT / path.split("/", 1)[0] / "COMPATIBILITY.md").read_text(encoding="utf-8")
        for path in EXPECTED_OWNERS
    )
    for compatibility_path, canonical_owner in EXPECTED_OWNERS.items():
        assert compatibility_path in combined_source
        assert canonical_owner in combined_source


def test_small_compatibility_packages_only_forward_to_canonical_owners():
    for relative_path, canonical_owner in EXPECTED_OWNERS.items():
        source_path = EDITOR_ROOT / relative_path
        source = source_path.read_text(encoding="utf-8")
        tree = ast.parse(source, filename=str(source_path))

        assert "Compatibility" in source
        assert canonical_owner in source
        assert not any(
            isinstance(node, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef))
            for node in ast.walk(tree)
        ), source_path

    for relative_path, compat_owner in LEGACY_WRAPPERS.items():
        source = (EDITOR_ROOT / relative_path).read_text(encoding="utf-8")
        assert compat_owner in source
        tree = ast.parse(source, filename=relative_path)
        assert not any(
            isinstance(node, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef))
            for node in ast.walk(tree)
        )


def test_canonical_production_code_does_not_import_small_compatibility_packages():
    roots = (
        EDITOR_ROOT / "api",
        EDITOR_ROOT / "config",
        EDITOR_ROOT / "plugins",
        EDITOR_ROOT / "runtime",
        EDITOR_ROOT / "script_runtime",
    )
    compatibility_prefixes = (
        "CoronaPlugin",
        "utils",
        "scripts",
    )
    violations = []
    for root in roots:
        for source_path in root.rglob("*.py"):
            if (
                "tests" in source_path.parts
                or "__pycache__" in source_path.parts
                or "Quasar" in source_path.parts
            ):
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
                    if module in compatibility_prefixes
                    or any(module.startswith(prefix + ".") for prefix in compatibility_prefixes)
                )

    assert violations == []

import ast
from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]

def test_external_compatibility_packages_are_removed():
    for package_name in ("CoronaPlugin", "utils", "scripts"):
        assert not any(
            path.is_file() and "__pycache__" not in path.parts
            for path in (EDITOR_ROOT / package_name).rglob("*")
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

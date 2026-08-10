from __future__ import annotations

import ast
import importlib.util
from pathlib import Path
from types import ModuleType
from typing import Iterable


def assert_module_has_no_forbidden_imports(
    testcase,
    module: ModuleType,
    forbidden_prefixes: Iterable[str],
) -> None:
    """Check one module's source imports without depending on global import order."""

    source_path = Path(str(module.__file__ or ""))
    if not source_path.is_file():
        raise AssertionError(f"module source is unavailable: {module.__name__}")
    tree = ast.parse(source_path.read_text(encoding="utf-8"), filename=str(source_path))
    imported: set[str] = set()
    package = str(module.__package__ or "")

    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            imported.update(alias.name for alias in node.names)
            continue
        if not isinstance(node, ast.ImportFrom):
            continue
        base = str(node.module or "")
        if node.level:
            relative_name = "." * node.level + base
            base = importlib.util.resolve_name(relative_name, package)
        if base:
            imported.add(base)
        imported.update(f"{base}.{alias.name}" if base else alias.name for alias in node.names)

    forbidden = tuple(str(item).rstrip(".") for item in forbidden_prefixes)
    violations = sorted(
        name
        for name in imported
        if any(name == prefix or name.startswith(prefix + ".") for prefix in forbidden)
    )
    testcase.assertEqual(violations, [], f"{module.__name__} imports forbidden modules")

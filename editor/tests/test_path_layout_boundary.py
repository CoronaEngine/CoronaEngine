import ast
import unittest
from pathlib import Path


class PathLayoutBoundaryTests(unittest.TestCase):
    def test_removed_external_compatibility_roots(self):
        root = Path(__file__).resolve().parents[1]
        for name in ("backend", "CoronaCore", "CoronaPlugin", "utils", "scripts"):
            files = [p for p in (root / name).rglob("*") if p.is_file() and "__pycache__" not in p.parts]
            self.assertEqual([], files, name)

    def test_canonical_production_code_does_not_import_removed_packages(self):
        root = Path(__file__).resolve().parents[1]
        prefixes = ("backend", "CoronaCore", "CoronaPlugin", "utils", "scripts")
        violations = []
        for base in (root / "api", root / "config", root / "plugins", root / "runtime", root / "script_runtime"):
            for path in base.rglob("*.py"):
                if "tests" in path.parts or "Quasar" in path.parts:
                    continue
                tree = ast.parse(path.read_text(encoding="utf-8-sig"), filename=str(path))
                for node in ast.walk(tree):
                    modules = ([a.name for a in node.names] if isinstance(node, ast.Import)
                               else [node.module or ""] if isinstance(node, ast.ImportFrom) else [])
                    for module in modules:
                        if module == "" or any(module == p or module.startswith(p + ".") for p in prefixes):
                            violations.append(f"{path}: {module}")
        self.assertEqual([], violations)

    def test_canonical_api_and_runtime_owners_exist(self):
        root = Path(__file__).resolve().parents[1]
        for relative in (
            "api/editor_api.py",
            "runtime/registry.py",
            "runtime/editor_host.py",
            "script_runtime/runner.py",
        ):
            self.assertTrue((root / relative).is_file(), relative)

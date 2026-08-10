from __future__ import annotations

import ast
from pathlib import Path
import unittest

from editor.plugins.AITool.services import schema_versions


class SchemaVersionTests(unittest.TestCase):
    def test_schema_version_registry_has_no_import_dependencies(self) -> None:
        path = Path(schema_versions.__file__)
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        imports = [
            node
            for node in ast.walk(tree)
            if isinstance(node, (ast.Import, ast.ImportFrom))
        ]
        self.assertEqual(imports, [])

    def test_schema_versions_are_declared_only_in_registry(self) -> None:
        registry_path = Path(schema_versions.__file__).resolve()
        services_root = registry_path.parent
        version_values = {
            value
            for name, value in vars(schema_versions).items()
            if name.endswith("_VERSION") and isinstance(value, str)
        }
        version_names = {
            name
            for name, value in vars(schema_versions).items()
            if name.endswith("_VERSION") and isinstance(value, str)
        }
        self.assertEqual(
            version_names,
            {
                "ACTION_PROPOSAL_SCHEMA_VERSION",
                "COLLABORATION_SCHEMA_VERSION",
                "ENGINE_ADAPTER_CONTRACT_VERSION",
                "ENGINE_SNAPSHOT_INPUT_CONTRACT_VERSION",
                "FRONTEND_INTERACTION_SCHEMA_VERSION",
                "GAMEPLAY_MANIFEST_SCHEMA_VERSION",
                "PLAN_PATCH_PAYLOAD_SCHEMA_VERSION",
                "R3_GATE_SCHEMA_VERSION",
                "SCENE_WORLD_SNAPSHOT_SCHEMA_VERSION",
                "SKELETON_CONTRACT_VERSION",
            },
        )

        duplicates: list[str] = []
        for path in services_root.rglob("*.py"):
            if path.resolve() == registry_path or path.name.startswith("test_") or path.name.startswith("_test_"):
                continue
            tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
            for node in ast.walk(tree):
                if isinstance(node, ast.Constant) and isinstance(node.value, str) and node.value in version_values:
                    duplicates.append(f"{path.relative_to(services_root)}:{node.lineno}:{node.value}")
        self.assertEqual(duplicates, [])

    def test_collaboration_contract_uses_central_version(self) -> None:
        from editor.plugins.AITool.services.agent_collaboration import contracts

        self.assertEqual(
            contracts.COLLABORATION_SCHEMA_VERSION,
            schema_versions.COLLABORATION_SCHEMA_VERSION,
        )


if __name__ == "__main__":
    unittest.main()

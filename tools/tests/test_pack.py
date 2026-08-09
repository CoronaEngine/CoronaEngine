import unittest
from pathlib import Path

from tools.pack import should_exclude


class PackPathBoundaryTests(unittest.TestCase):
    def test_pack_tool_has_a_tools_canonical_owner(self):
        repo_root = Path(__file__).resolve().parents[2]
        canonical = repo_root / "tools" / "pack.py"
        compatibility = repo_root / "editor" / "scripts" / "pack.py"
        duplicate_test = repo_root / "editor" / "scripts" / "test_pack.py"

        self.assertTrue(canonical.is_file())
        self.assertIn("Compatibility", compatibility.read_text(encoding="utf-8"))
        self.assertFalse(duplicate_test.exists())

    def test_current_generated_paths_are_excluded(self):
        self.assertTrue(should_exclude("Frontend/node_modules/vue/index.js"))
        self.assertTrue(should_exclude("Frontend/dist/assets/index.js"))
        self.assertTrue(should_exclude("backend/__pycache__/runtime.pyc"))

    def test_current_source_paths_remain_packable(self):
        self.assertFalse(should_exclude("backend/registry.py"))
        self.assertFalse(should_exclude("Frontend/src/main.js"))
        self.assertFalse(should_exclude("plugins/SceneTools/main.py"))

    def test_legacy_backend_paths_remain_compatible(self):
        self.assertTrue(should_exclude("Backend_backup/old.py"))
        self.assertTrue(should_exclude("Backend/script/blockly_code.py"))

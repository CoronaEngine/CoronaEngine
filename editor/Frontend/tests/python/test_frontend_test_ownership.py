import unittest
from pathlib import Path


class FrontendTestOwnershipTests(unittest.TestCase):
    def setUp(self):
        self.frontend_root = Path(__file__).resolve().parents[2]
        self.source_root = self.frontend_root / "src"
        self.javascript_tests_root = self.frontend_root / "tests" / "js"

    def test_javascript_tests_have_a_dedicated_owner_directory(self):
        self.assertTrue(self.javascript_tests_root.is_dir())
        javascript_tests = sorted(
            self.javascript_tests_root.rglob("*.test.mjs")
        )
        self.assertTrue(javascript_tests)
        self.assertEqual(
            [],
            [
                path
                for path in self.source_root.rglob("*")
                if path.is_file()
                and (".test." in path.name or ".spec." in path.name)
            ],
        )

    def test_manifest_contract_has_a_dedicated_api_owner(self):
        api_root = self.source_root / "api"
        bridge = self.source_root / "utils" / "bridge.js"

        self.assertTrue((api_root / "editorApi.js").is_file())
        self.assertIn(
            "export * from '../api/editorApi.js'",
            bridge.read_text(encoding="utf-8"),
        )

    def test_production_frontend_does_not_import_compatibility_bridge(self):
        bridge_path = self.source_root / "utils" / "bridge.js"
        offenders = []
        for path in self.source_root.rglob("*"):
            if (
                not path.is_file()
                or path == bridge_path
                or path.suffix not in {".js", ".mjs", ".ts", ".vue"}
            ):
                continue
            if "utils/bridge" in path.read_text(encoding="utf-8"):
                offenders.append(str(path.relative_to(self.source_root)))

        self.assertEqual(offenders, [])


if __name__ == "__main__":
    unittest.main()

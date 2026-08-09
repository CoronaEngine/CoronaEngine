import ast
import unittest
from dataclasses import fields
from pathlib import Path
from unittest.mock import patch


class PathLayoutBoundaryTests(unittest.TestCase):
    def test_canonical_production_code_does_not_import_compatibility_packages(self):
        repo_root = Path(__file__).resolve().parents[1]
        roots = [
            repo_root / "config",
            repo_root / "plugins",
            repo_root / "runtime",
            repo_root / "script_runtime",
            repo_root / "CoronaCore" / "core" / "editor_api.py",
            repo_root / "runtime" / "legacy_engine_adapter.py",
            repo_root / "script_runtime" / "compat" / "legacy_scene_datas_adapter.py",
            repo_root / "script_runtime" / "legacy_scene_datas_adapter.py",
        ]
        compatibility_prefixes = (
            "backend.file_system",
            "backend.project_settings",
            "backend.blockly",
            "backend.script",
            "CoronaCore.core.corona_editor",
            "CoronaCore.core.corona_engine",
            "CoronaCore.core.components",
            "CoronaCore.core.entities",
            "CoronaCore.core.managers",
            "CoronaCore.core.network_sync_policy",
            "CoronaCore.core.project_utils",
            "CoronaCore.core.response_utils",
            "CoronaCore.core.scripts_system",
            "CoronaCore.core.legacy_scene_store",
            "CoronaCore.utils",
            "utils.settings",
            "utils.logging",
        )
        violations = []
        files = []
        for root in roots:
            files.extend([root] if root.is_file() else root.rglob("*.py"))
        for path in files:
            if "tests" in path.parts or "__pycache__" in path.parts:
                continue
            tree = ast.parse(path.read_text(encoding="utf-8-sig"), filename=str(path))
            for node in ast.walk(tree):
                if isinstance(node, ast.Import):
                    imported = [alias.name for alias in node.names]
                elif isinstance(node, ast.ImportFrom):
                    imported = [node.module or ""]
                else:
                    continue
                for module in imported:
                    if any(
                        module == prefix or module.startswith(prefix + ".")
                        for prefix in compatibility_prefixes
                    ):
                        violations.append(f"{path}: {module}")

        self.assertEqual(violations, [])

    def test_runtime_paths_follow_lowercase_repository_layout(self):
        from config.paths_config import get_default_paths

        with patch("config.paths_config.get_project_media_dir", return_value=Path("media")), \
             patch("config.paths_config.get_project_models_dir", return_value=Path("models")), \
             patch("config.paths_config.get_project_recognition_db", return_value=Path("models/database.db")):
            paths = get_default_paths()

        self.assertEqual(paths.backend_root.name, "backend")
        self.assertEqual(paths.script_dir, paths.backend_root / "script")

    def test_legacy_backend_paths_are_derived_compatibility_properties(self):
        from config.paths_config import get_default_paths

        with patch("config.paths_config.get_project_media_dir", return_value=Path("media")), \
             patch("config.paths_config.get_project_models_dir", return_value=Path("models")), \
             patch("config.paths_config.get_project_recognition_db", return_value=Path("models/database.db")):
            paths = get_default_paths()

        field_names = {field.name for field in fields(paths)}
        self.assertNotIn("backend_root", field_names)
        self.assertNotIn("script_dir", field_names)
        self.assertEqual(paths.backend_root, paths.repo_root / "backend")
        self.assertEqual(paths.script_dir, paths.repo_root / "backend" / "script")

    def test_settings_reuses_the_canonical_paths_owner(self):
        from config import paths_config
        from config import settings

        self.assertIs(settings.PathsConfig, paths_config.PathsConfig)
        self.assertIs(settings.get_default_paths, paths_config.get_default_paths)
        self.assertIsInstance(settings.core_path, paths_config.PathsConfig)

    def test_blockly_and_main_view_use_lowercase_backend_module(self):
        repo_root = Path(__file__).resolve().parents[1]
        blockly_source = (repo_root / "script_runtime" / "blockly" / "main.py").read_text(encoding="utf-8")
        main_view_source = (repo_root / "plugins" / "MainView" / "main.py").read_text(encoding="utf-8")
        runtime_source = (repo_root / "config" / "runtime_config.py").read_text(encoding="utf-8")
        generator_constants = (
            repo_root / "Frontend" / "src" / "blockly" / "generators" / "constants.js"
        ).read_text(encoding="utf-8")

        for source in (blockly_source, main_view_source, runtime_source):
            self.assertNotIn("Backend/", source)
            self.assertNotIn("Backend\\", source)
        self.assertIn("generated_script_dir", blockly_source)
        self.assertNotIn("from backend.script import blockly_code", blockly_source)
        self.assertIn("from script_runtime.runner import run_generated_script", main_view_source)
        self.assertNotIn('repo_root / "backend" / "runScript.py"', main_view_source)
        self.assertNotIn("from backend import runScript", main_view_source)
        self.assertIn("./InnerAgentWorkflow", runtime_source)
        self.assertNotIn("from Backend", generator_constants)

    def test_file_and_project_services_have_plugin_canonical_paths(self):
        repo_root = Path(__file__).resolve().parents[1]
        registry_source = (repo_root / "runtime" / "registry.py").read_text(encoding="utf-8")
        file_service = repo_root / "plugins" / "FileManager" / "main.py"
        project_service = repo_root / "plugins" / "ProjectSettings" / "main.py"

        self.assertIn('"FileManager": ("plugins.FileManager.main", "FileManager")', registry_source)
        self.assertIn('"ProjectSettings": ("plugins.ProjectSettings.main", "ProjectSettings")', registry_source)
        self.assertTrue(file_service.is_file())
        self.assertTrue(project_service.is_file())

        file_compat = (repo_root / "backend" / "file_system" / "main.py").read_text(encoding="utf-8")
        project_compat = (repo_root / "backend" / "project_settings" / "main.py").read_text(encoding="utf-8")
        self.assertIn("from plugins.FileManager.main import FileManager", file_compat)
        self.assertIn("from plugins.ProjectSettings.main import ProjectSettings", project_compat)

    def test_aggregate_plugin_owners_are_explicit_python_packages(self):
        repo_root = Path(__file__).resolve().parents[1]

        for plugin_name in ("FileManager", "ProjectSettings"):
            plugin_root = repo_root / "plugins" / plugin_name
            self.assertTrue(
                (plugin_root / "__init__.py").is_file(),
                f"{plugin_name} must have an explicit package owner",
            )

    def test_backend_service_packages_identify_themselves_as_compatibility(self):
        repo_root = Path(__file__).resolve().parents[1]

        for package_name in ("file_system", "project_settings"):
            source = (
                repo_root / "backend" / package_name / "__init__.py"
            ).read_text(encoding="utf-8")
            self.assertIn("compatibility", source.lower())

    def test_legacy_plugin_package_has_an_explicit_root_boundary(self):
        repo_root = Path(__file__).resolve().parents[1]
        package_init = repo_root / "CoronaPlugin" / "__init__.py"

        self.assertTrue(package_init.is_file())
        self.assertIn("compatibility", package_init.read_text(encoding="utf-8").lower())

    def test_frontend_python_tests_have_a_separate_language_owner(self):
        repo_root = Path(__file__).resolve().parents[1]
        frontend_tests = repo_root / "Frontend" / "tests"
        python_tests = frontend_tests / "python"

        self.assertTrue(python_tests.is_dir())
        self.assertEqual([], list(frontend_tests.glob("test_*.py")))
        self.assertEqual(
            {
                "test_editor_api_aggregate_wrappers.py",
                "test_frontend_blockly_boundary.py",
                "test_frontend_compatibility_boundary.py",
                "test_frontend_service_boundary.py",
                "test_frontend_source_tree_boundary.py",
                "test_frontend_test_ownership.py",
                "test_frontend_utils_boundary.py",
                "test_legacy_frontend_entrypoint.py",
            },
            {path.name for path in python_tests.glob("test_*.py")},
        )

    def test_project_file_helpers_have_runtime_project_ownership(self):
        repo_root = Path(__file__).resolve().parents[1]
        canonical = repo_root / "runtime" / "project_support.py"
        compatibility = repo_root / "CoronaCore" / "utils" / "proejct_utils.py"

        self.assertTrue(canonical.is_file())
        compatibility_source = compatibility.read_text(encoding="utf-8")
        self.assertIn("from runtime.project_templates import", compatibility_source)
        self.assertIn("from runtime.scene_support import", compatibility_source)

    def test_scratch_runtime_has_script_system_ownership(self):
        repo_root = Path(__file__).resolve().parents[1]
        canonical = repo_root / "script_runtime" / "engine" / "corona_engine.py"
        compatibility = repo_root / "CoronaCore" / "utils" / "corona_engine_scratch.py"

        self.assertTrue(canonical.is_file())
        self.assertIn("from script_runtime.engine.corona_engine import *", compatibility.read_text(encoding="utf-8"))

    def test_engine_runtime_header_has_a_single_canonical_include_owner(self):
        repo_root = Path(__file__).resolve().parents[2]
        canonical = repo_root / "include" / "corona" / "engine" / "engine_runtime_api.h"
        compatibility = (
            repo_root / "include" / "corona" / "systems" / "script" / "corona_engine_api.h"
        )

        self.assertTrue(canonical.is_file())
        compatibility_source = compatibility.read_text(encoding="utf-8")
        self.assertIn("Compatibility include", compatibility_source)
        self.assertIn("#include <corona/engine/engine_runtime_api.h>", compatibility_source)

        for root in (repo_root / "src", repo_root / "include"):
            for path in root.rglob("*"):
                if not path.is_file() or path == compatibility:
                    continue
                if path.suffix.lower() not in {".h", ".hpp", ".cpp", ".cc", ".cxx"}:
                    continue
                source = path.read_text(encoding="utf-8", errors="ignore")
                self.assertNotIn(
                    "#include <corona/systems/script/corona_engine_api.h>",
                    source,
                    str(path),
                )

    def test_engine_runtime_implementation_has_engine_source_owner(self):
        repo_root = Path(__file__).resolve().parents[2]
        canonical = repo_root / "src" / "engine" / "engine_runtime_api.cpp"
        legacy = repo_root / "src" / "systems" / "script" / "python" / "corona_engine_api.cpp"
        script_cmake = (repo_root / "src" / "systems" / "script" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )

        self.assertTrue(canonical.is_file())
        self.assertFalse(legacy.is_file())
        self.assertIn("${PROJECT_SOURCE_DIR}/src/engine/engine_runtime_api.cpp", script_cmake)

    def test_canonical_editor_api_tests_use_the_canonical_import_path(self):
        repo_root = Path(__file__).resolve().parents[1]
        canonical_tests = repo_root / "api" / "tests"

        for source_path in canonical_tests.glob("*.py"):
            source = source_path.read_text(encoding="utf-8")
            self.assertNotIn("CoronaCore.core.editor_api", source, str(source_path))

    def test_script_runtime_tests_use_canonical_editor_api_imports(self):
        repo_root = Path(__file__).resolve().parents[1]
        source = (
            repo_root / "script_runtime" / "tests" / "test_scratch_camera_adapter.py"
        ).read_text(encoding="utf-8")

        self.assertNotIn("from CoronaCore.core", source)
        self.assertIn("from api import editor_api", source)

    def test_aitool_mcp_tests_use_canonical_editor_api_imports(self):
        repo_root = Path(__file__).resolve().parents[1]
        test_paths = (
            repo_root / "plugins" / "AITool" / "cai_extensions" / "mcp" / "tools" / "tests" / "test_camera_tools_boundary.py",
            repo_root / "plugins" / "AITool" / "cai_extensions" / "mcp" / "tools" / "tests" / "test_model_import_tools.py",
        )

        for source_path in test_paths:
            source = source_path.read_text(encoding="utf-8")
            self.assertNotIn("\nfrom CoronaCore.core import editor_api\n", source, str(source_path))
            self.assertNotIn("\n            \"CoronaCore.core.editor_api.", source, str(source_path))


if __name__ == "__main__":
    unittest.main()

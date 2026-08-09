import os
import re
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from editor.plugins.AITool.utils.load_local_ai_setting import (
    apply_api_key_env_overrides,
    load_dotenv_file,
)


class LoadLocalAISettingTests(unittest.TestCase):
    def test_local_secret_loader_has_a_config_owner_and_utility_compatibility_wrapper(self):
        aitool_root = Path(__file__).resolve().parents[1]
        config_source = aitool_root / "configuration" / "local_secrets.py"
        compat_source = (
            aitool_root / "compat" / "legacy_local_ai_setting.py"
        ).read_text(encoding="utf-8")
        compat_package_source = (
            aitool_root / "compat" / "legacy_aitool_utils.py"
        ).read_text(encoding="utf-8")
        utility_source = (aitool_root / "utils" / "load_local_ai_setting.py").read_text(
            encoding="utf-8"
        )
        utility_package_source = (aitool_root / "utils" / "__init__.py").read_text(
            encoding="utf-8"
        )
        main_source = (aitool_root / "main.py").read_text(encoding="utf-8")
        review_source = (
            aitool_root / "services" / "node_graph_review_service.py"
        ).read_text(encoding="utf-8")

        self.assertTrue(config_source.is_file())
        self.assertIn("from ..configuration.local_secrets import", compat_source)
        self.assertIn("from ..configuration.local_secrets import", compat_package_source)
        self.assertIn(
            "from plugins.AITool.compat.legacy_local_ai_setting import", utility_source
        )
        self.assertIn(
            "from plugins.AITool.compat.legacy_aitool_utils import",
            utility_package_source,
        )
        self.assertIn("from .configuration.local_secrets import", main_source)
        self.assertIn("from ..configuration.local_secrets import", review_source)

    def test_legacy_ai_setting_module_and_inline_secret_values_are_gone(self):
        aitool_root = Path(__file__).resolve().parents[1]
        self.assertFalse((aitool_root / "utils" / "ai_setting.py").exists())
        secret_pattern = re.compile(r"api_key\s*[:=]\s*[\"']sk-[A-Za-z0-9]{20,}")
        for path in aitool_root.rglob("*.py"):
            if "Quasar" in path.parts or "tests" in path.parts:
                continue
            self.assertIsNone(secret_pattern.search(path.read_text(encoding="utf-8")), path)

    def test_load_dotenv_file_sets_only_missing_environment_values(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            env_file = Path(temp_dir) / ".env"
            env_file.write_text(
                "OPENAI_API_KEY=from-dotenv\n"
                "# ignored comment\n"
                "QUOTED_KEY=\"quoted-value\"\n",
                encoding="utf-8",
            )
            with patch.dict(os.environ, {"OPENAI_API_KEY": "already-set"}, clear=False):
                loaded = load_dotenv_file(env_file)
                self.assertEqual("already-set", os.environ["OPENAI_API_KEY"])
                self.assertEqual("quoted-value", os.environ["QUOTED_KEY"])
            self.assertEqual({"QUOTED_KEY": "quoted-value"}, loaded)

    def test_provider_and_module_keys_are_overridden_by_environment(self):
        settings = {
            "providers": [
                {"name": "deepseek", "api_key": "placeholder"},
                {"name": "dashscope", "api_key": "placeholder"},
            ],
            "hunyuan3d": {"api_key": "placeholder"},
            "object_recognition": {"dashscope_api_key": "placeholder"},
            "music": {"api_key": "placeholder"},
        }
        with patch.dict(
            os.environ,
            {
                "CORONA_DEEPSEEK_API_KEY": "deepseek-test-key",
                "DASHSCOPE_API_KEY": "dashscope-test-key",
                "CORONA_HUNYUAN3D_API_KEY": "hunyuan-test-key",
                "SUNO_API_KEY": "suno-test-key",
            },
            clear=False,
        ):
            apply_api_key_env_overrides(settings)

        self.assertEqual("deepseek-test-key", settings["providers"][0]["api_key"])
        self.assertEqual("dashscope-test-key", settings["providers"][1]["api_key"])
        self.assertEqual("hunyuan-test-key", settings["hunyuan3d"]["api_key"])
        self.assertEqual(
            "dashscope-test-key",
            settings["object_recognition"]["dashscope_api_key"],
        )
        self.assertEqual("suno-test-key", settings["music"]["api_key"])

    def test_key_values_are_not_logged_or_returned(self):
        settings = {"providers": [{"name": "openai", "api_key": "placeholder"}]}
        with patch.dict(os.environ, {"OPENAI_API_KEY": "secret-test-key"}, clear=False):
            result = apply_api_key_env_overrides(settings)

        self.assertNotIn("secret-test-key", repr(result))
        self.assertEqual({"providers": 1, "overridden": 1}, result)


if __name__ == "__main__":
    unittest.main()

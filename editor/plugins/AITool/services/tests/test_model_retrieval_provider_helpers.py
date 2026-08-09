from __future__ import annotations

import sys
import types
import unittest
import importlib.util
from unittest.mock import patch
from pathlib import Path


_AITOOL_ROOT = Path(__file__).resolve().parents[2]
if str(_AITOOL_ROOT) not in sys.path:
    sys.path.insert(0, str(_AITOOL_ROOT))

_HELPERS_PATH = (
    _AITOOL_ROOT
    / "cai_extensions"
    / "flows"
    / "model_retrieval_workflow"
    / "helpers.py"
)
_SPEC = importlib.util.spec_from_file_location("model_retrieval_workflow_helpers_under_test", _HELPERS_PATH)
assert _SPEC is not None and _SPEC.loader is not None
helpers = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(helpers)

_MODEL_PROVIDER_PATH = (
    _AITOOL_ROOT
    / "cai_extensions"
    / "agent"
    / "model_provider.py"
)
_MODEL_PROVIDER_SPEC = importlib.util.spec_from_file_location(
    "agent_model_provider_under_test",
    _MODEL_PROVIDER_PATH,
)
assert _MODEL_PROVIDER_SPEC is not None and _MODEL_PROVIDER_SPEC.loader is not None
model_provider = importlib.util.module_from_spec(_MODEL_PROVIDER_SPEC)
_MODEL_PROVIDER_SPEC.loader.exec_module(model_provider)


class ModelRetrievalProviderHelperTests(unittest.TestCase):
    def setUp(self) -> None:
        self._orig_get_ai_config = helpers.get_ai_config
        self._orig_get_tool = helpers.get_tool

    def tearDown(self) -> None:
        helpers.get_ai_config = self._orig_get_ai_config
        helpers.get_tool = self._orig_get_tool

    def test_dashscope_embedding_search_and_store_are_disabled_for_formal_validation(self) -> None:
        self.assertFalse(helpers.object_embedding_tools_enabled())
        self.assertIsNone(helpers.get_search_tool())
        self.assertIsNone(helpers.get_store_tool())

    def test_get_3d_generate_tool_accepts_dict_hunyuan_settings(self) -> None:
        sentinel_tool = object()
        helpers.get_ai_config = lambda: types.SimpleNamespace(
            hunyuan3d={"enable": True, "api_keys": ["test-key"]},
        )
        helpers.get_tool = lambda name: sentinel_tool if name == "hunyuan_generate_3d" else None

        self.assertIs(helpers.get_3d_generate_tool(), sentinel_tool)

    def test_get_3d_generate_tool_rejects_disabled_dict_hunyuan_settings(self) -> None:
        helpers.get_ai_config = lambda: types.SimpleNamespace(
            hunyuan3d={"enable": False, "api_keys": ["test-key"]},
        )
        helpers.get_tool = lambda name: object()

        self.assertIsNone(helpers.get_3d_generate_tool())

    def test_agent_model_provider_accepts_dict_hunyuan_settings(self) -> None:
        sentinel_tool = object()
        config_module = types.ModuleType("Quasar.ai_config.ai_config")
        config_module.get_ai_config = lambda: types.SimpleNamespace(
            hunyuan3d={"enable": True, "api_keys": ["test-key"]},
        )
        provider = model_provider.ModelProvider()
        provider._get_tool = lambda name: sentinel_tool if name == "hunyuan_generate_3d" else None

        with patch.dict("sys.modules", {"Quasar.ai_config.ai_config": config_module}):
            self.assertIs(provider._get_3d_generate_tool(), sentinel_tool)

    def test_agent_model_provider_falls_back_to_plugin_quasar_namespace(self) -> None:
        sentinel_tool = object()
        config_module = types.SimpleNamespace(
            get_ai_config=lambda: types.SimpleNamespace(
                hunyuan3d={"enable": True, "api_keys": ["test-key"]},
            )
        )
        provider = model_provider.ModelProvider()
        provider._get_tool = lambda name: sentinel_tool if name == "hunyuan_generate_3d" else None

        def fake_import(name: str):
            if name == "Quasar.ai_config.ai_config":
                raise ImportError("bare Quasar unavailable")
            if name == "plugins.AITool.Quasar.ai_config.ai_config":
                return config_module
            raise AssertionError(f"unexpected import: {name}")

        with patch.object(model_provider.importlib, "import_module", side_effect=fake_import):
            self.assertIs(provider._get_3d_generate_tool(), sentinel_tool)


if __name__ == "__main__":
    unittest.main()

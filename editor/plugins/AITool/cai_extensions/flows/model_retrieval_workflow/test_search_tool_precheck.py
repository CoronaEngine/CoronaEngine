"""Offline self-check for object search credential preflight."""
from __future__ import annotations

import importlib.util
import os
import sys
import types
from pathlib import Path


_PKG = "cai_extensions.flows.model_retrieval_workflow"


def _install_quasar_stubs(config):
    ai_config_mod = types.ModuleType("Quasar.ai_config.ai_config")
    ai_config_mod.get_ai_config = lambda: config

    registry_mod = types.ModuleType("Quasar.ai_tools.registry")
    registry_mod.get_tool_registry = lambda: types.SimpleNamespace(list_tools=lambda: [])

    response_adapter_mod = types.ModuleType("Quasar.ai_tools.response_adapter")
    response_adapter_mod.FILEID_SCHEME = "fileid://"

    paths_config_mod = types.ModuleType("Quasar.ai_config.paths_config")
    paths_config_mod._get_active_project_path = lambda: Path.cwd()

    sys.modules.setdefault("Quasar", types.ModuleType("Quasar"))
    sys.modules.setdefault("Quasar.ai_config", types.ModuleType("Quasar.ai_config"))
    sys.modules.setdefault("Quasar.ai_tools", types.ModuleType("Quasar.ai_tools"))
    sys.modules["Quasar.ai_config.ai_config"] = ai_config_mod
    sys.modules["Quasar.ai_tools.registry"] = registry_mod
    sys.modules["Quasar.ai_tools.response_adapter"] = response_adapter_mod
    sys.modules["Quasar.ai_config.paths_config"] = paths_config_mod


def _load_helpers(config):
    _install_quasar_stubs(config)
    path = Path(__file__).with_name("helpers.py")
    spec = importlib.util.spec_from_file_location(f"{_PKG}.helpers_precheck_test", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[f"{_PKG}.helpers_precheck_test"] = module
    spec.loader.exec_module(module)
    return module


def test_search_tool_precheck_skips_missing_dashscope_key():
    old_enable = os.environ.pop("CORONA_ENABLE_OBJECT_EMBEDDING", None)
    config = types.SimpleNamespace(
        object_recognition={"enable": True, "provider": "dashscope", "dashscope_api_key": ""},
        providers={"dashscope": types.SimpleNamespace(api_key="")},
    )
    try:
        helpers = _load_helpers(config)

        called = {"value": False}

        def fail_if_called(_name):
            called["value"] = True
            raise AssertionError("get_tool should not be called without embedding credentials")

        helpers.get_tool = fail_if_called
        assert helpers.get_search_tool() is None
        assert called["value"] is False
    finally:
        if old_enable is not None:
            os.environ["CORONA_ENABLE_OBJECT_EMBEDDING"] = old_enable
    print("[OK] search tool precheck skips missing Dashscope key")


def test_search_tool_precheck_skips_provider_key_by_default():
    old_enable = os.environ.pop("CORONA_ENABLE_OBJECT_EMBEDDING", None)
    sentinel = object()
    config = types.SimpleNamespace(
        object_recognition={"enable": True, "provider": "dashscope", "dashscope_api_key": ""},
        providers={"dashscope": types.SimpleNamespace(api_key="valid-key")},
    )
    try:
        helpers = _load_helpers(config)

        helpers.get_tool = lambda name: sentinel if name == "search_similar_object" else None
        assert helpers.get_search_tool() is None
    finally:
        if old_enable is not None:
            os.environ["CORONA_ENABLE_OBJECT_EMBEDDING"] = old_enable
    print("[OK] search tool precheck skips provider key by default")


def test_search_tool_precheck_skips_provider_key_even_when_enabled():
    config = types.SimpleNamespace(
        object_recognition={
            "enable": True,
            "enable_embedding": True,
            "provider": "dashscope",
            "dashscope_api_key": "",
        },
        providers={"dashscope": types.SimpleNamespace(api_key="valid-key")},
    )
    helpers = _load_helpers(config)

    def fail_if_called(_name):
        raise AssertionError("search_similar_object must stay disabled for formal validation")

    helpers.get_tool = fail_if_called
    assert helpers.get_search_tool() is None
    print("[OK] search tool precheck skips provider key even when enabled")


def test_search_tool_precheck_skips_env_key_by_default():
    old_enable = os.environ.pop("CORONA_ENABLE_OBJECT_EMBEDDING", None)
    sentinel = object()
    config = types.SimpleNamespace(
        object_recognition={"enable": True, "provider": "dashscope", "dashscope_api_key": ""},
        providers={"dashscope": types.SimpleNamespace(api_key="")},
    )
    helpers = _load_helpers(config)
    old_value = os.environ.get("DASHSCOPE_API_KEY")
    os.environ["DASHSCOPE_API_KEY"] = "env-key"
    try:
        helpers.get_tool = lambda name: sentinel if name == "search_similar_object" else None
        assert helpers.get_search_tool() is None
    finally:
        if old_value is None:
            os.environ.pop("DASHSCOPE_API_KEY", None)
        else:
            os.environ["DASHSCOPE_API_KEY"] = old_value
        if old_enable is not None:
            os.environ["CORONA_ENABLE_OBJECT_EMBEDDING"] = old_enable
    print("[OK] search tool precheck skips environment key by default")


def test_search_tool_precheck_skips_env_key_even_when_enabled():
    config = types.SimpleNamespace(
        object_recognition={"enable": True, "provider": "dashscope", "dashscope_api_key": ""},
        providers={"dashscope": types.SimpleNamespace(api_key="")},
    )
    helpers = _load_helpers(config)
    old_key = os.environ.get("DASHSCOPE_API_KEY")
    old_enable = os.environ.get("CORONA_ENABLE_OBJECT_EMBEDDING")
    os.environ["DASHSCOPE_API_KEY"] = "env-key"
    os.environ["CORONA_ENABLE_OBJECT_EMBEDDING"] = "1"
    try:
        def fail_if_called(_name):
            raise AssertionError("search_similar_object must stay disabled for formal validation")

        helpers.get_tool = fail_if_called
        assert helpers.get_search_tool() is None
    finally:
        if old_key is None:
            os.environ.pop("DASHSCOPE_API_KEY", None)
        else:
            os.environ["DASHSCOPE_API_KEY"] = old_key
        if old_enable is None:
            os.environ.pop("CORONA_ENABLE_OBJECT_EMBEDDING", None)
        else:
            os.environ["CORONA_ENABLE_OBJECT_EMBEDDING"] = old_enable
    print("[OK] search tool precheck skips environment key even when enabled")


def test_rodin_fallback_is_disabled_when_hunyuan_unavailable():
    config = types.SimpleNamespace(
        hunyuan3d=types.SimpleNamespace(enable=False),
    )
    helpers = _load_helpers(config)

    def fail_on_rodin(name):
        if name == "rodin_generate_3d":
            raise AssertionError("Rodin fallback must stay disabled for formal validation")
        return None

    helpers.get_tool = fail_on_rodin
    assert helpers.get_3d_generate_tool() is None
    print("[OK] Rodin fallback stays disabled when Hunyuan is unavailable")


def test_hunyuan_tool_is_still_allowed():
    sentinel = object()
    config = types.SimpleNamespace(
        hunyuan3d=types.SimpleNamespace(enable=True),
    )
    helpers = _load_helpers(config)

    def get_tool(name):
        if name == "hunyuan_generate_3d":
            return sentinel
        if name == "rodin_generate_3d":
            raise AssertionError("Rodin fallback must not be queried when Hunyuan is available")
        return None

    helpers.get_tool = get_tool
    assert helpers.get_3d_generate_tool() is sentinel
    print("[OK] Hunyuan 3D tool remains enabled")


def test_store_tool_skips_by_default():
    old_enable = os.environ.pop("CORONA_ENABLE_OBJECT_EMBEDDING", None)
    config = types.SimpleNamespace(
        object_recognition={"enable": True, "provider": "dashscope", "dashscope_api_key": ""},
        providers={"dashscope": types.SimpleNamespace(api_key="valid-key")},
    )
    try:
        helpers = _load_helpers(config)

        called = {"value": False}

        def fail_if_called(_name):
            called["value"] = True
            raise AssertionError("get_tool should not be called when object embedding is disabled")

        helpers.get_tool = fail_if_called
        assert helpers.get_store_tool() is None
        assert called["value"] is False
    finally:
        if old_enable is not None:
            os.environ["CORONA_ENABLE_OBJECT_EMBEDDING"] = old_enable
    print("[OK] store tool skips by default")


if __name__ == "__main__":
    test_search_tool_precheck_skips_missing_dashscope_key()
    test_search_tool_precheck_skips_provider_key_by_default()
    test_search_tool_precheck_skips_provider_key_even_when_enabled()
    test_search_tool_precheck_skips_env_key_by_default()
    test_search_tool_precheck_skips_env_key_even_when_enabled()
    test_store_tool_skips_by_default()
    test_rodin_fallback_is_disabled_when_hunyuan_unavailable()
    test_hunyuan_tool_is_still_allowed()
    print("\n=== search tool precheck ALL PASS ===")

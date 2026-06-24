"""Offline self-check for image retry preflight behavior.

This test does not call any provider. It stubs the workflow dependencies and
verifies fatal image-provider configuration errors do not fan out to all items.
"""
from __future__ import annotations

import os
import sys
import types
import importlib.util
from pathlib import Path


sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..")))

_PKG = "cai_extensions.flows.model_retrieval_workflow"
if _PKG not in sys.modules:
    pkg = types.ModuleType(_PKG)
    pkg.__path__ = [os.path.dirname(__file__)]  # type: ignore[attr-defined]
    sys.modules[_PKG] = pkg


def _install_quasar_stubs() -> None:
    context_mod = types.ModuleType("Quasar.ai_tools.context")
    context_mod.set_current_session = lambda session_id: session_id
    context_mod.reset_current_session = lambda token: None

    state_mod = types.ModuleType("Quasar.ai_workflow.state")
    state_mod.ModelRetrievalWorkflowState = dict

    streaming_mod = types.ModuleType("Quasar.ai_workflow.streaming")
    streaming_mod.stream_output_node = lambda *_args, **_kwargs: (lambda fn: fn)
    streaming_mod.FormatterFunc = object
    streaming_mod.build_node_dialogue_entry = lambda *args, **kwargs: {}
    progress_mod = types.ModuleType("Quasar.ai_workflow.progress")
    progress_mod.publish_node_entries_event = lambda *_args, **_kwargs: None

    sys.modules.setdefault("Quasar", types.ModuleType("Quasar"))
    sys.modules.setdefault("Quasar.ai_tools", types.ModuleType("Quasar.ai_tools"))
    sys.modules["Quasar.ai_tools.context"] = context_mod
    sys.modules.setdefault("Quasar.ai_workflow", types.ModuleType("Quasar.ai_workflow"))
    sys.modules["Quasar.ai_workflow.state"] = state_mod
    sys.modules["Quasar.ai_workflow.streaming"] = streaming_mod
    sys.modules["Quasar.ai_workflow.progress"] = progress_mod


def _install_image_helper_stub(fake_tool) -> None:
    helpers_mod = types.ModuleType("cai_extensions.flows.integrated_multi_scene_workflow.helpers")
    helpers_mod.get_generate_image_tool = lambda: fake_tool
    helpers_mod.extract_image_url = lambda _raw: ""
    pkg_name = "cai_extensions.flows.integrated_multi_scene_workflow"
    sys.modules.setdefault(pkg_name, types.ModuleType(pkg_name))
    sys.modules["cai_extensions.flows.integrated_multi_scene_workflow.helpers"] = helpers_mod


def _install_dispatch_dependency_stubs() -> None:
    formatters_mod = types.ModuleType(f"{_PKG}.formatters")
    formatters_mod.NO_OUTPUT = lambda *_args, **_kwargs: None

    helpers_mod = types.ModuleType(f"{_PKG}.helpers")
    helpers_mod.normalize_object_id = lambda name, idx: f"obj-{idx}-{name}"

    test_cases_mod = types.ModuleType(f"{_PKG}.test_cases")
    test_cases_mod.get_test_case = lambda _key: {}

    sys.modules[f"{_PKG}.formatters"] = formatters_mod
    sys.modules[f"{_PKG}.helpers"] = helpers_mod
    sys.modules[f"{_PKG}.test_cases"] = test_cases_mod


def _load_dispatch_module(fake_tool):
    _install_quasar_stubs()
    _install_dispatch_dependency_stubs()
    _install_image_helper_stub(fake_tool)
    path = Path(__file__).with_name("dispatch.py")
    spec = importlib.util.spec_from_file_location(f"{_PKG}.dispatch", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[f"{_PKG}.dispatch"] = module
    spec.loader.exec_module(module)
    return module


class FatalImageTool:
    def __init__(self) -> None:
        self.calls = 0

    def invoke(self, _payload):
        self.calls += 1
        raise RuntimeError("图像生成失败: 无效的 URL")


def test_fatal_image_retry_error_stops_fanout():
    tool = FatalImageTool()
    dispatch = _load_dispatch_module(tool)
    recovered = dispatch._retry_failed_images([
        {"item_name": "黑木拱门", "image_prompt": "dark wooden arch"},
        {"item_name": "石灯", "image_prompt": "stone lantern"},
        {"item_name": "主柜台", "image_prompt": "market counter"},
    ], "session-test")
    assert recovered == {}
    assert tool.calls == 1
    print("[OK] fatal image retry error stops fanout and lets workflow fall back to text-to-3D")


def test_cached_local_model_skips_image_retry():
    tool = FatalImageTool()
    dispatch = _load_dispatch_module(tool)

    retried_names: list[str] = []
    dispatch._lookup_cached_model = lambda name: "E:/cache/models/lantern" if name == "灯笼" else ""
    dispatch._lookup_cached_image = lambda _name: ""

    def fake_retry(elements, _session_id):
        retried_names.extend(elem.get("item_name", "") for elem in elements)
        return {}

    dispatch._retry_failed_images = fake_retry
    result = dispatch.dispatch_node(
        {
            "session_id": "session-test",
            "approved_elements": [
                {"item_name": "灯笼", "image_prompt": "warm lantern"},
                {"item_name": "天使雕像", "image_prompt": "warm fantasy angel statue"},
            ],
            "generated_images": {},
        }
    )

    tasks = result["intermediate"]["retrieval_tasks"]
    assert retried_names == ["天使雕像"]
    assert tasks[0]["item_name"] == "灯笼"
    assert tasks[0]["local_model_cached"] is True
    assert tasks[0]["model_path"] == "E:/cache/models/lantern"
    assert tasks[0]["image_url"] == "__local_model__:灯笼"
    assert tasks[1]["item_name"] == "天使雕像"
    assert tasks[1]["image_url"].startswith("__text_to_3d__:")
    print("[OK] cached local models skip image retry and slow image compensation")


def test_cached_image_skips_image_retry():
    tool = FatalImageTool()
    dispatch = _load_dispatch_module(tool)

    dispatch._lookup_cached_model = lambda _name: ""
    dispatch._lookup_cached_image = lambda name: "E:/cache/images/sign.png" if name == "导视牌" else ""

    def fail_if_called(_elements, _session_id):
        raise AssertionError("cached image should not enter image retry")

    dispatch._retry_failed_images = fail_if_called
    result = dispatch.dispatch_node(
        {
            "session_id": "session-test",
            "approved_elements": [
                {"item_name": "导视牌", "image_prompt": "market sign"},
            ],
            "generated_images": {},
        }
    )

    tasks = result["intermediate"]["retrieval_tasks"]
    assert len(tasks) == 1
    assert tasks[0]["item_name"] == "导视牌"
    assert tasks[0]["image_url"] == "E:/cache/images/sign.png"
    print("[OK] cached images skip image retry")


def test_text_to_3d_hint_skips_image_retry_and_cached_image():
    tool = FatalImageTool()
    dispatch = _load_dispatch_module(tool)

    dispatch._lookup_cached_model = lambda _name: ""
    dispatch._lookup_cached_image = lambda name: "E:/cache/images/wire.png" if name == "铁丝网障碍" else ""

    def fail_if_called(_elements, _session_id):
        raise AssertionError("text_to_3d_preferred should not enter image retry")

    dispatch._retry_failed_images = fail_if_called
    result = dispatch.dispatch_node(
        {
            "session_id": "session-test",
            "approved_elements": [
                {
                    "item_name": "铁丝网障碍",
                    "image_prompt": "barbed wire obstacle, visible thickness and depth",
                    "generation_mode_hint": "text_to_3d_preferred",
                },
            ],
            "generated_images": {},
        }
    )

    tasks = result["intermediate"]["retrieval_tasks"]
    assert len(tasks) == 1
    assert tasks[0]["item_name"] == "铁丝网障碍"
    assert tasks[0]["image_url"].startswith("__text_to_3d__:")
    assert tasks[0]["generation_mode_hint"] == "text_to_3d_preferred"
    print("[OK] text-to-3D guardrail skips cached image and image retry")


if __name__ == "__main__":
    test_fatal_image_retry_error_stops_fanout()
    test_cached_local_model_skips_image_retry()
    test_cached_image_skips_image_retry()
    test_text_to_3d_hint_skips_image_retry_and_cached_image()
    print("\n=== dispatch image retry ALL PASS ===")

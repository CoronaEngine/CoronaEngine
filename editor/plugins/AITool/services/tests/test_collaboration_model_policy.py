from __future__ import annotations

import unittest
import sys
import threading
import time
from types import ModuleType
from unittest.mock import patch

from editor.plugins.AITool.services.collaboration_model_policy import (
    CollaborationModelSelection,
    StaticCollaborationModelSelector,
    default_collaboration_model_selector,
)
from editor.plugins.AITool.services.lanchat_agent_worker import LANChatAgentWorker
from editor.plugins.AITool.services.agent_collaboration.production_reasoners import (
    CollaborationReasoningError,
)


class CollaborationModelPolicyTests(unittest.TestCase):
    def test_default_selector_uses_deepseek_v4_pro(self) -> None:
        selection = default_collaboration_model_selector().select(
            "planning_artifact_reasoning"
        )

        self.assertEqual(selection.provider_name, "deepseek")
        self.assertEqual(selection.model_name, "deepseek-v4-pro")
        self.assertEqual(selection.temperature, 0.0)
        self.assertEqual(selection.request_timeout, 90.0)
        self.assertEqual(selection.output_mode, "json_object")
        self.assertEqual(selection.max_retries, 0)

        text_selection = default_collaboration_model_selector().select(
            "agent_visible_reasoning"
        )
        self.assertEqual(text_selection.output_mode, "text")
        self.assertEqual(text_selection.request_timeout, 60.0)
        self.assertEqual(text_selection.max_retries, 0)

    def test_static_selector_supports_purpose_override(self) -> None:
        default = CollaborationModelSelection("provider-a", "model-a")
        override = CollaborationModelSelection("provider-b", "model-b", request_timeout=30)
        selector = StaticCollaborationModelSelector(
            default,
            overrides={"program_artifact_reasoning": override},
        )

        self.assertIs(selector.select("planning_artifact_reasoning"), default)
        self.assertIs(selector.select("program_artifact_reasoning"), override)

    def test_worker_accepts_injected_selector(self) -> None:
        selection = CollaborationModelSelection("custom-provider", "custom-model")
        selector = StaticCollaborationModelSelector(selection)
        worker = LANChatAgentWorker(collaboration_model_selector=selector)

        resolved = worker._select_collaboration_model("agent_visible_reasoning")

        self.assertIs(resolved, selection)

    def test_tool_free_chat_passes_selected_model_to_quasar(self) -> None:
        selection = CollaborationModelSelection(
            "custom-provider",
            "custom-model",
            temperature=0.25,
            request_timeout=37,
        )
        worker = LANChatAgentWorker(
            collaboration_model_selector=StaticCollaborationModelSelector(selection)
        )
        captured: dict[str, object] = {}

        class _Model:
            def bind(self, **kwargs):
                captured["bind_kwargs"] = kwargs
                return self

            def invoke(self, messages):
                captured["messages"] = messages
                return type("Response", (), {"content": "selected model reply"})()

        def get_chat_model(**kwargs):
            captured["kwargs"] = kwargs
            return _Model()

        registry = ModuleType("Quasar.ai_models.base_pool.registry")
        registry.get_chat_model = get_chat_model
        base_pool = ModuleType("Quasar.ai_models.base_pool")
        ai_models = ModuleType("Quasar.ai_models")
        quasar = ModuleType("Quasar")
        modules = {
            "Quasar": quasar,
            "Quasar.ai_models": ai_models,
            "Quasar.ai_models.base_pool": base_pool,
            "Quasar.ai_models.base_pool.registry": registry,
        }
        trigger = {
            "room_id": "room-model-policy",
            "message_id": "message-model-policy",
            "correlation_id": "correlation-model-policy",
        }

        with (
            patch.object(worker, "_ensure_runtime_quasar_import_path"),
            patch.object(worker, "_ensure_runtime_ai_config_loaded"),
            patch.dict(sys.modules, modules),
        ):
            reply = worker._complete_tool_free_chat(
                trigger,
                purpose="planning_artifact_reasoning",
                system_prompt="system",
                user_prompt="user",
                max_calls=1,
            )

        self.assertEqual(reply, "selected model reply")
        self.assertEqual(captured["kwargs"], {
            "provider_name": "custom-provider",
            "model_name": "custom-model",
            "temperature": 0.25,
            "request_timeout": 37,
            "max_retries": 0,
        })
        self.assertNotIn("bind_kwargs", captured)
        summary = worker._model_call_ledger.summary(
            room_id="room-model-policy",
            message_id="message-model-policy",
        )
        self.assertEqual(summary["calls"][0]["provider"], "custom-provider")
        self.assertEqual(summary["calls"][0]["model"], "custom-model")

    def test_structured_selection_binds_json_object_mode(self) -> None:
        selection = CollaborationModelSelection(
            "deepseek",
            "deepseek-v4-pro",
            output_mode="json_object",
        )
        worker = LANChatAgentWorker(
            collaboration_model_selector=StaticCollaborationModelSelector(selection)
        )
        captured: dict[str, object] = {}

        class _Model:
            def bind(self, **kwargs):
                captured["bind"] = kwargs
                return self

            def invoke(self, _messages):
                return type("Response", (), {"content": "{}"})()

        registry = ModuleType("Quasar.ai_models.base_pool.registry")
        registry.get_chat_model = lambda **_kwargs: _Model()
        modules = {
            "Quasar": ModuleType("Quasar"),
            "Quasar.ai_models": ModuleType("Quasar.ai_models"),
            "Quasar.ai_models.base_pool": ModuleType("Quasar.ai_models.base_pool"),
            "Quasar.ai_models.base_pool.registry": registry,
        }
        with (
            patch.object(worker, "_ensure_runtime_quasar_import_path"),
            patch.object(worker, "_ensure_runtime_ai_config_loaded"),
            patch.dict(sys.modules, modules),
        ):
            worker._complete_tool_free_chat(
                {"room_id": "room-json", "message_id": "message-json"},
                purpose="program_artifact_reasoning",
                system_prompt="system",
                user_prompt="user",
                max_calls=1,
            )

        self.assertEqual(captured["bind"], {"response_format": {"type": "json_object"}})

    def test_structured_selection_fails_closed_without_bind_support(self) -> None:
        selection = CollaborationModelSelection(
            "deepseek",
            "deepseek-v4-pro",
            output_mode="json_object",
        )
        worker = LANChatAgentWorker(
            collaboration_model_selector=StaticCollaborationModelSelector(selection)
        )

        class _Model:
            def invoke(self, _messages):
                raise AssertionError("invoke must not run without structured output support")

        registry = ModuleType("Quasar.ai_models.base_pool.registry")
        registry.get_chat_model = lambda **_kwargs: _Model()
        modules = {
            "Quasar": ModuleType("Quasar"),
            "Quasar.ai_models": ModuleType("Quasar.ai_models"),
            "Quasar.ai_models.base_pool": ModuleType("Quasar.ai_models.base_pool"),
            "Quasar.ai_models.base_pool.registry": registry,
        }
        with (
            patch.object(worker, "_ensure_runtime_quasar_import_path"),
            patch.object(worker, "_ensure_runtime_ai_config_loaded"),
            patch.dict(sys.modules, modules),
            self.assertRaises(CollaborationReasoningError) as caught,
        ):
            worker._complete_tool_free_chat(
                {"room_id": "room-json-blocked", "message_id": "message-json-blocked"},
                purpose="program_artifact_reasoning",
                system_prompt="system",
                user_prompt="user",
                max_calls=1,
            )

        self.assertEqual(caught.exception.error_code, "structured_output_unavailable")

    def test_timeout_is_mapped_without_model_retry(self) -> None:
        selection = CollaborationModelSelection(
            "deepseek",
            "deepseek-v4-pro",
            request_timeout=90,
            max_retries=0,
        )
        worker = LANChatAgentWorker(
            collaboration_model_selector=StaticCollaborationModelSelector(selection)
        )
        calls = [0]

        class _Model:
            def invoke(self, _messages):
                calls[0] += 1
                raise TimeoutError("request timed out")

        registry = ModuleType("Quasar.ai_models.base_pool.registry")
        registry.get_chat_model = lambda **kwargs: (
            self.assertEqual(kwargs["max_retries"], 0) or _Model()
        )
        modules = {
            "Quasar": ModuleType("Quasar"),
            "Quasar.ai_models": ModuleType("Quasar.ai_models"),
            "Quasar.ai_models.base_pool": ModuleType("Quasar.ai_models.base_pool"),
            "Quasar.ai_models.base_pool.registry": registry,
        }
        with (
            patch.object(worker, "_ensure_runtime_quasar_import_path"),
            patch.object(worker, "_ensure_runtime_ai_config_loaded"),
            patch.dict(sys.modules, modules),
            self.assertRaises(CollaborationReasoningError) as caught,
        ):
            worker._complete_tool_free_chat(
                {"room_id": "room-timeout", "message_id": "message-timeout"},
                purpose="program_artifact_reasoning",
                system_prompt="system",
                user_prompt="user",
                max_calls=1,
            )

        self.assertEqual(calls[0], 1)
        self.assertEqual(caught.exception.error_code, "collaboration_stage_timeout")
        self.assertEqual(caught.exception.stage, "program")

    def test_application_deadline_returns_before_blocking_model_finishes(self) -> None:
        selection = CollaborationModelSelection(
            "deepseek",
            "deepseek-v4-pro",
            request_timeout=0.5,
            max_retries=0,
        )
        worker = LANChatAgentWorker(
            collaboration_model_selector=StaticCollaborationModelSelector(selection)
        )
        release = threading.Event()

        class _Model:
            def invoke(self, _messages):
                release.wait(3.0)
                return type("Response", (), {"content": "late"})()

        registry = ModuleType("Quasar.ai_models.base_pool.registry")
        registry.get_chat_model = lambda **_kwargs: _Model()
        modules = {
            "Quasar": ModuleType("Quasar"),
            "Quasar.ai_models": ModuleType("Quasar.ai_models"),
            "Quasar.ai_models.base_pool": ModuleType("Quasar.ai_models.base_pool"),
            "Quasar.ai_models.base_pool.registry": registry,
        }
        started = time.monotonic()
        try:
            with (
                patch.object(worker, "_ensure_runtime_quasar_import_path"),
                patch.object(worker, "_ensure_runtime_ai_config_loaded"),
                patch.dict(sys.modules, modules),
                self.assertRaises(CollaborationReasoningError) as caught,
            ):
                worker._complete_tool_free_chat(
                    {"room_id": "room-hard-timeout", "message_id": "message-hard-timeout"},
                    purpose="program_artifact_reasoning",
                    system_prompt="system",
                    user_prompt="user",
                    max_calls=1,
                )
        finally:
            release.set()

        self.assertLess(time.monotonic() - started, 1.5)
        self.assertEqual(caught.exception.error_code, "collaboration_stage_timeout")

    def test_invalid_selection_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "provider_name"):
            CollaborationModelSelection("", "model")
        with self.assertRaisesRegex(ValueError, "model_name"):
            CollaborationModelSelection("provider", "")
        with self.assertRaisesRegex(ValueError, "request_timeout"):
            CollaborationModelSelection("provider", "model", request_timeout=0)
        with self.assertRaisesRegex(ValueError, "max_retries"):
            CollaborationModelSelection("provider", "model", max_retries=-1)


if __name__ == "__main__":
    unittest.main()

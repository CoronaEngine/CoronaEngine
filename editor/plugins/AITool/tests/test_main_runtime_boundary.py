from pathlib import Path


def test_aitool_module_does_not_import_editor_runtime_container_at_module_scope():
    source = (Path(__file__).resolve().parents[1] / "main.py").read_text(encoding="utf-8")
    module_scope = source.split("@PluginBase.register_web", 1)[0]

    assert "from CoronaCore.core.corona_editor import CoronaEditor" not in module_scope


def test_aitool_runtime_does_not_pass_native_engine_to_network_worker():
    source = (Path(__file__).resolve().parents[1] / "main.py").read_text(encoding="utf-8")

    assert "import CoronaEngine as corona_engine" not in source
    assert "corona_engine=corona_engine" not in source


def test_media_helpers_have_a_service_owner_and_utils_use_it_directly():
    aitool_root = Path(__file__).resolve().parents[1]
    service_source = aitool_root / "services" / "media_storage.py"
    utility_source = (aitool_root / "utils" / "image_utils.py").read_text(
        encoding="utf-8"
    )
    main_source = (aitool_root / "main.py").read_text(encoding="utf-8")

    assert service_source.is_file()
    assert not (aitool_root / "compat" / "legacy_image_utils.py").exists()
    assert "from ..services.media_storage import" in utility_source
    assert "from .services.media_storage import" in main_source


def test_lanchat_worker_loads_ai_settings_from_configuration_owner():
    aitool_root = Path(__file__).resolve().parents[1]
    worker_source = (
        aitool_root / "services" / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "from ..configuration.local_secrets import load_ai_setting" in worker_source
    assert "from plugins.AITool.utils import load_ai_setting" not in worker_source


def test_agent_runtime_does_not_construct_concrete_cai_model_provider():
    aitool_root = Path(__file__).resolve().parents[1]
    runtime_source = (
        aitool_root / "services" / "agent_runtime" / "adapters.py"
    ).read_text(encoding="utf-8")
    worker_source = (aitool_root / "services" / "lanchat_agent_worker.py").read_text(
        encoding="utf-8"
    )
    composition_source = (aitool_root / "services" / "composition_root.py").read_text(
        encoding="utf-8"
    )

    assert "cai_extensions.agent.model_provider" not in runtime_source
    assert "model_provider_factory=" in worker_source
    assert "cai_extensions.agent.model_provider" in composition_source


def test_orchestrator_requires_agent_factory_from_integration_boundary():
    aitool_root = Path(__file__).resolve().parents[1]
    orchestrator_source = (
        aitool_root / "services" / "lanchat_agent_orchestrator.py"
    ).read_text(encoding="utf-8")
    worker_source = (aitool_root / "services" / "lanchat_agent_worker.py").read_text(
        encoding="utf-8"
    )

    assert "cai_extensions.agent.agent_adapter" not in orchestrator_source
    assert "agent_factory=self._agent_factory or self._default_agent_factory" in worker_source


def test_host_action_executor_does_not_import_concrete_engine_gate():
    aitool_root = Path(__file__).resolve().parents[1]
    executor_source = (
        aitool_root / "services" / "lanchat_host_action_executor.py"
    ).read_text(encoding="utf-8")

    assert "cai_extensions.agent.engine_write_gate" not in executor_source
    assert "_default_engine_gate" not in executor_source


def test_host_action_executor_receives_engine_gate_from_worker_composition_root():
    aitool_root = Path(__file__).resolve().parents[1]
    worker_source = (aitool_root / "services" / "lanchat_agent_worker.py").read_text(
        encoding="utf-8"
    )

    assert "engine_gate=" in worker_source


def test_worker_reuses_one_lazy_engine_gate_instance():
    aitool_root = Path(__file__).resolve().parents[1]
    worker_source = (aitool_root / "services" / "lanchat_agent_worker.py").read_text(
        encoding="utf-8"
    )

    assert "engine_write_gate import" not in worker_source
    assert "def _get_engine_write_gate" in worker_source
    assert "engine_gate=self._get_engine_write_gate()" in worker_source


def test_worker_engine_gate_loader_runs_once():
    from unittest.mock import patch

    from plugins.AITool.services import lanchat_agent_worker as worker_module

    worker = worker_module.LANChatAgentWorker.__new__(worker_module.LANChatAgentWorker)
    worker._engine_write_gate = worker_module._ENGINE_GATE_UNSET
    gate = object()

    with patch.object(worker_module, "create_engine_write_gate", return_value=gate) as factory:
        assert worker._get_engine_write_gate() is gate
        assert worker._get_engine_write_gate() is gate

    assert factory.call_count == 1


def test_aitool_composition_factories_have_a_dedicated_owner():
    aitool_root = Path(__file__).resolve().parents[1]
    composition_path = aitool_root / "services" / "composition_root.py"
    worker_source = (aitool_root / "services" / "lanchat_agent_worker.py").read_text(
        encoding="utf-8"
    )

    assert composition_path.is_file()
    composition_source = composition_path.read_text(encoding="utf-8")
    for symbol in (
        "create_legacy_model_provider",
        "create_engine_write_gate",
        "create_scene_element_classifier",
    ):
        assert f"def {symbol}" in composition_source
    assert "def _create_engine_write_gate" not in worker_source
    assert "def _create_scene_element_classifier" not in worker_source


def test_scene_runtime_does_not_import_concrete_scene_classifier():
    aitool_root = Path(__file__).resolve().parents[1]
    runtime_source = (
        aitool_root / "services" / "lanchat_scene_runtime.py"
    ).read_text(encoding="utf-8")

    assert "cai_extensions.agent.scene_element_classifier" not in runtime_source


def test_scene_runtime_classifier_is_configured_by_worker_composition_root():
    aitool_root = Path(__file__).resolve().parents[1]
    worker_source = (aitool_root / "services" / "lanchat_agent_worker.py").read_text(
        encoding="utf-8"
    )

    assert "create_scene_element_classifier" in worker_source
    assert "configure_lanchat_scene_runtime" in worker_source


def test_scene_runtime_uses_injected_classifier_for_disclosure():
    from plugins.AITool.services.lanchat_scene_runtime import LanChatSceneRuntime

    class FakeClassifier:
        @staticmethod
        def route_model_items(scene_goal, rows):  # noqa: ANN001
            return [], rows

        @staticmethod
        def summarize_classification(routes):  # noqa: ANN001
            return "injected classification"

    runtime = LanChatSceneRuntime(scene_element_classifier=FakeClassifier)

    assert runtime._classification_disclosure("goal", ["table"]) == "injected classification"


def test_agent_runtime_tools_do_not_load_cai_extensions_classifier():
    aitool_root = Path(__file__).resolve().parents[1]
    tools_source = (
        aitool_root / "services" / "agent_runtime" / "tools.py"
    ).read_text(encoding="utf-8")

    assert "cai_extensions" not in tools_source
    assert "_load_scene_element_classifier" not in tools_source


def test_agent_runtime_classifier_is_injected_from_worker_composition_root():
    aitool_root = Path(__file__).resolve().parents[1]
    core_source = (
        aitool_root / "services" / "agent_runtime" / "core.py"
    ).read_text(encoding="utf-8")
    worker_source = (aitool_root / "services" / "lanchat_agent_worker.py").read_text(
        encoding="utf-8"
    )

    assert "scene_element_classifier" in core_source
    assert 'kwargs["scene_element_classifier"]' in worker_source


def test_agent_runtime_classification_tool_uses_injected_classifier():
    from plugins.AITool.services.agent_runtime.core import ToolCall, ToolRegistry
    from plugins.AITool.services.agent_runtime.tools import register_agent_runtime_planning_tools

    class FakeRoute:
        name = "injected-model"
        target_pipeline = "model"

        @staticmethod
        def as_dict():
            return {"name": "injected-model", "target_pipeline": "model"}

    class FakeClassifier:
        @staticmethod
        def route_model_items(scene_goal, items):  # noqa: ANN001
            return [{"name": "injected-model"}], [FakeRoute()]

        @staticmethod
        def summarize_classification(routes):  # noqa: ANN001
            return "injected summary"

    registry = ToolRegistry()
    register_agent_runtime_planning_tools(
        registry,
        scene_element_classifier=FakeClassifier,
    )
    definition = registry.definition("runtime.elements.classify")
    assert definition is not None

    result = definition.handler(
        ToolCall(
            tool_call_id="classification-test",
            tool_name="runtime.elements.classify",
            args={"room_id": "room", "text": "goal", "items": ["table"]},
        )
    )

    assert result.success
    assert result.payload["model_items"] == ["injected-model"]
    assert result.state_patch is not None
    assert result.state_patch.changes["classification_summaries"] == {
        "classification-test": "injected summary"
    }


def test_runtime_query_policy_owns_worker_query_detection():
    from plugins.AITool.services import runtime_query_policy

    expected_symbols = (
        "runtime_command_from_text",
        "is_runtime_worker_drain_query",
        "runtime_worker_drain_limit_from_text",
        "is_runtime_provider_status_query",
        "is_runtime_enqueue_generation_query",
        "is_runtime_engine_write_status_query",
        "is_runtime_scene_snapshot_query",
        "is_runtime_tool_manifest_query",
        "is_runtime_operation_replay_query",
        "is_runtime_report_query",
        "is_runtime_sync_status_query",
        "is_runtime_r3_gate_query",
        "is_runtime_gm_summary_query",
        "is_runtime_status_summary_query",
        "is_runtime_status_query_text",
    )

    for symbol in expected_symbols:
        assert callable(getattr(runtime_query_policy, symbol))


def test_runtime_query_policy_preserves_command_and_query_semantics():
    from plugins.AITool.services.runtime_query_policy import (
        is_runtime_enqueue_generation_query,
        is_runtime_operation_replay_query,
        is_runtime_report_query,
        is_runtime_scene_snapshot_query,
        is_runtime_sync_status_query,
        is_runtime_tool_manifest_query,
        is_runtime_worker_drain_query,
        runtime_command_from_text,
        runtime_worker_drain_limit_from_text,
    )

    assert runtime_command_from_text("请暂停生成") == "pause"
    assert runtime_command_from_text("resume task") == "resume"
    assert runtime_command_from_text("普通聊天") == ""
    assert is_runtime_worker_drain_query("Runtime drain queue")
    assert runtime_worker_drain_limit_from_text("drain all") == 1000
    assert runtime_worker_drain_limit_from_text("drain one") == 1
    assert is_runtime_enqueue_generation_query("Runtime confirm_and_enqueue generation")
    assert is_runtime_scene_snapshot_query("Runtime scene snapshot")
    assert is_runtime_tool_manifest_query("show Runtime tool manifest")
    assert is_runtime_operation_replay_query("Runtime operation replay")
    assert is_runtime_report_query("Runtime final report")
    assert is_runtime_sync_status_query("Runtime sync status")


def test_worker_keeps_compatibility_forwarders_for_runtime_query_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "from .runtime_query_policy import" in worker_source
    for symbol in (
        "_runtime_command_from_text",
        "_is_runtime_worker_drain_query",
        "_is_runtime_status_query_text",
    ):
        assert f"def {symbol}" in worker_source


def test_runtime_result_policy_owns_graph_and_batch_normalization():
    from plugins.AITool.services import runtime_result_policy

    assert callable(runtime_result_policy.agent_runtime_graphs_from_result)
    assert callable(runtime_result_policy.agent_runtime_batches_from_result)


def test_runtime_result_policy_preserves_legacy_result_shapes():
    from plugins.AITool.services.runtime_result_policy import (
        agent_runtime_batches_from_result,
        agent_runtime_graphs_from_result,
    )

    graph = {"graph_id": "g1", "status": "queued"}
    batch = {"batch_id": "b1", "status": "planned"}
    assert agent_runtime_graphs_from_result({"graphs": [graph]}) == [graph]
    assert agent_runtime_graphs_from_result({"graph": graph}) == [graph]
    assert agent_runtime_graphs_from_result({"queued": {"graphs": [graph]}}) == [graph]
    assert agent_runtime_batches_from_result({"batches": [batch]}) == [batch]
    assert agent_runtime_batches_from_result({"batch": batch}) == [batch]
    assert agent_runtime_batches_from_result({"queued": {"batch": batch}}) == [batch]


def test_worker_keeps_compatibility_forwarders_for_runtime_result_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "from .runtime_result_policy import" in worker_source
    assert "def _agent_runtime_graphs_from_result" in worker_source
    assert "def _agent_runtime_batches_from_result" in worker_source


def test_runtime_report_policy_owns_scene_report_formatting():
    from plugins.AITool.services import runtime_report_policy

    for symbol in (
        "format_agent_runtime_short_list",
        "format_agent_runtime_scene_registry_report",
        "format_agent_runtime_scene_world_consistency_report",
        "format_agent_runtime_environment_report",
        "format_agent_runtime_scene_contract_report",
        "format_agent_runtime_semantic_arbitration_report",
        "format_agent_runtime_tool_execution_digest_report",
    ):
        assert callable(getattr(runtime_report_policy, symbol))


def test_runtime_report_policy_preserves_summary_and_secret_redaction_behavior():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_scene_contract_report,
        format_agent_runtime_scene_registry_report,
        format_agent_runtime_scene_world_consistency_report,
        format_agent_runtime_short_list,
        format_agent_runtime_tool_execution_digest_report,
    )

    assert format_agent_runtime_short_list(["a", "b", "c"], limit=2) == "a、b 等 3 项"
    assert format_agent_runtime_scene_registry_report({"entity_count": 2, "actor_count": 2}) == (
        "entities 2, actor 2, terrain 0, skybox 0"
    )
    assert format_agent_runtime_scene_world_consistency_report({
        "status": "consistent",
        "expected_entity_count": 2,
        "engine_actor_count": 2,
        "matched_entity_count": 2,
    }) == "对账通过，匹配 2/2，Engine 实体 2"
    contract = format_agent_runtime_scene_contract_report({
        "available": True,
        "scene_type": "prompt_scene",
        "environment_type": "forest",
        "terrain_type": "terrain",
        "boundary_type": "box",
        "mood": ["calm"],
        "style_keywords": ["stylized"],
        "avoid_keywords": [],
    })
    assert "resource-scene" in contract
    assert "prompt" not in contract.lower()
    digest = format_agent_runtime_tool_execution_digest_report({
        "available": True,
        "graph_count": 1,
        "attention_required": True,
        "attention_reasons": ["provider unavailable"],
        "latest_attention": {"status": "provider", "reason": "api-key"},
    })
    assert "resource" in digest
    assert "api-key" not in digest.lower()


def test_runtime_replay_report_policy_owns_replay_summary_formatting():
    from plugins.AITool.services import runtime_replay_report_policy

    for symbol in (
        "format_agent_runtime_replay_command_report",
        "format_agent_runtime_replay_tool_execution_report",
        "format_agent_runtime_replay_tool_queue_report",
        "format_agent_runtime_replay_state_patch_report",
        "format_agent_runtime_replay_guard_report",
    ):
        assert callable(getattr(runtime_replay_report_policy, symbol))


def test_runtime_replay_report_policy_preserves_summary_shapes():
    from plugins.AITool.services.runtime_replay_report_policy import (
        format_agent_runtime_replay_command_report,
        format_agent_runtime_replay_guard_report,
        format_agent_runtime_replay_state_patch_report,
        format_agent_runtime_replay_tool_execution_report,
        format_agent_runtime_replay_tool_queue_report,
    )

    assert "2 command(s)" in format_agent_runtime_replay_command_report({
        "command_count": 2,
        "cancelled_graph_total": 1,
        "latest_command": {"command": "cancel_task", "old_status": "running", "new_status": "cancelled"},
    })
    assert format_agent_runtime_replay_tool_execution_report({"started_count": 1, "succeeded_count": 1}).startswith(
        "started 1, succeeded 1"
    )
    assert "missing 1" in format_agent_runtime_replay_tool_queue_report({"missing_graph_count": 1})
    assert "reconciled 2" in format_agent_runtime_replay_state_patch_report({"reconciled": 2})
    assert "high-risk-confirm 1" in format_agent_runtime_replay_guard_report({
        "blocked_count": 1,
        "high_risk_confirmation_required_count": 1,
    })


def test_worker_keeps_compatibility_forwarders_for_runtime_replay_report_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "from .runtime_replay_report_policy import" in worker_source
    for symbol in (
        "_format_agent_runtime_replay_command_report",
        "_format_agent_runtime_replay_tool_execution_report",
        "_format_agent_runtime_replay_tool_queue_report",
        "_format_agent_runtime_replay_state_patch_report",
        "_format_agent_runtime_replay_guard_report",
    ):
        assert f"def {symbol}" in worker_source


def test_runtime_replay_lifecycle_policy_owns_plan_intervention_geometry_reports():
    from plugins.AITool.services import runtime_replay_lifecycle_policy

    for symbol in (
        "format_agent_runtime_replay_plan_lifecycle_report",
        "format_agent_runtime_replay_intervention_report",
        "format_agent_runtime_replay_geometry_report",
    ):
        assert callable(getattr(runtime_replay_lifecycle_policy, symbol))


def test_runtime_replay_lifecycle_policy_preserves_summary_shapes():
    from plugins.AITool.services.runtime_replay_lifecycle_policy import (
        format_agent_runtime_replay_geometry_report,
        format_agent_runtime_replay_intervention_report,
        format_agent_runtime_replay_plan_lifecycle_report,
    )

    assert "created 1" in format_agent_runtime_replay_plan_lifecycle_report({
        "created_count": 1,
        "confirmed_count": 1,
        "latest_plan_event": {"event": "plan_created", "status": "completed"},
    })
    assert "absorbed 2" in format_agent_runtime_replay_intervention_report({
        "routed_count": 1,
        "absorbed_count": 2,
    })
    assert "overlap 1" in format_agent_runtime_replay_geometry_report({
        "fact_count": 2,
        "overlap_issue_count": 1,
    })


def test_worker_keeps_compatibility_forwarders_for_runtime_replay_lifecycle_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "from .runtime_replay_lifecycle_policy import" in worker_source
    for symbol in (
        "_format_agent_runtime_replay_plan_lifecycle_report",
        "_format_agent_runtime_replay_intervention_report",
        "_format_agent_runtime_replay_geometry_report",
    ):
        assert f"def {symbol}" in worker_source


def test_runtime_replay_event_policy_owns_event_summary_formatting():
    from plugins.AITool.services import runtime_replay_event_policy

    assert callable(runtime_replay_event_policy.format_agent_runtime_replay_runtime_event_report)


def test_runtime_replay_event_policy_preserves_counts_and_redaction():
    from plugins.AITool.services.runtime_replay_event_policy import (
        format_agent_runtime_replay_runtime_event_report,
    )

    text = format_agent_runtime_replay_runtime_event_report({
        "emitted_count": 4,
        "emit_failed_count": 1,
        "disclosure_skipped_count": 2,
        "event_type_counts": {"report_ready": 1, "provider_raw_url_hidden": 2},
        "report_ready_count": 1,
        "report_attention_count": 1,
        "report_health_status_counts": {"partial": 1},
        "latest_report_ready": {
            "status": "partial",
            "environment_import_failure_code_counts": {"cpp_environment_component_import_failed": 1},
            "engine_write_bridge_failed_count": 1,
            "engine_write_bridge_error_code_counts": {"provider_raw_url_hidden": 1},
            "engine_write_readiness_mismatch_count": 1,
            "engine_write_readiness_mismatch_channels": ["layout_transform"],
        },
    })

    assert "emitted 4, failed 1, skipped 2" in text
    assert "report-ready 1/attention 1 partial:1" in text
    assert "engine-write-mismatch 1(layout-transform)" in text
    assert "provider" not in text
    assert "url" not in text


def test_worker_keeps_compatibility_forwarder_for_runtime_replay_event_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "from .runtime_replay_event_policy import" in worker_source
    assert "def _format_agent_runtime_replay_runtime_event_report" in worker_source


def test_runtime_replay_detail_policy_owns_failure_layout_vlm_review_reports():
    from plugins.AITool.services import runtime_replay_detail_policy

    for symbol in (
        "format_agent_runtime_replay_failure_strategy_report",
        "format_agent_runtime_replay_layout_report",
        "format_agent_runtime_replay_vlm_report",
        "format_agent_runtime_replay_review_advisory_report",
        "format_agent_runtime_replay_final_adjustment_report",
    ):
        assert callable(getattr(runtime_replay_detail_policy, symbol))


def test_runtime_replay_detail_policy_preserves_summary_shapes():
    from plugins.AITool.services.runtime_replay_detail_policy import (
        format_agent_runtime_replay_failure_strategy_report,
        format_agent_runtime_replay_final_adjustment_report,
        format_agent_runtime_replay_layout_report,
        format_agent_runtime_replay_review_advisory_report,
        format_agent_runtime_replay_vlm_report,
    )

    assert "retry 2" in format_agent_runtime_replay_failure_strategy_report({
        "retry_scheduled_count": 2,
        "dependency_skipped_count": 1,
    })
    assert "transforms 2/1" in format_agent_runtime_replay_layout_report({
        "transform_success_count": 2,
        "transform_failed_count": 1,
    })
    assert "status completed:1" in format_agent_runtime_replay_vlm_report({
        "checkpoint_count": 1,
        "status_counts": {"completed": 1},
    })
    assert "pending:1" in format_agent_runtime_replay_review_advisory_report({
        "proposal_created_count": 1,
        "pending_proposal_count": 1,
    })
    assert "conflicts 2" in format_agent_runtime_replay_final_adjustment_report({
        "confirmation_count": 1,
        "latest_confirmation": {"decision": "confirmed", "conflict_item_count": 2},
    })


def test_worker_keeps_compatibility_forwarders_for_runtime_replay_detail_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "from .runtime_replay_detail_policy import" in worker_source
    for symbol in (
        "_format_agent_runtime_replay_failure_strategy_report",
        "_format_agent_runtime_replay_layout_report",
        "_format_agent_runtime_replay_vlm_report",
        "_format_agent_runtime_replay_review_advisory_report",
        "_format_agent_runtime_replay_final_adjustment_report",
    ):
        assert f"def {symbol}" in worker_source


def test_runtime_replay_resource_policy_owns_environment_and_readiness_reports():
    from plugins.AITool.services import runtime_replay_resource_policy

    for symbol in (
        "format_agent_runtime_replay_environment_report",
        "format_agent_runtime_replay_resource_readiness_report",
    ):
        assert callable(getattr(runtime_replay_resource_policy, symbol))


def test_runtime_replay_resource_policy_preserves_counts_and_redaction():
    from plugins.AITool.services.runtime_replay_resource_policy import (
        format_agent_runtime_replay_environment_report,
        format_agent_runtime_replay_resource_readiness_report,
    )

    environment = format_agent_runtime_replay_environment_report({
        "ready_event_count": 2,
        "failed_event_count": 1,
        "import_event_count": 3,
        "import_failed_event_count": 1,
        "event_type_counts": {"environment_ready": 2},
        "latest_event_type": "environment_import_failed",
    })
    readiness = format_agent_runtime_replay_resource_readiness_report({
        "status_query_count": 2,
        "published_count": 1,
        "readiness_event_count": 1,
        "publish_status_counts": {"provider_raw_url_hidden": 1},
        "latest_readiness_event": {"status": "provider_api_key_hidden"},
    })

    assert "ready 2/1" in environment
    assert "import 3/1" in environment
    assert "latest environment-import-failed" in environment
    assert "queries 2" in readiness
    assert "published 1/0" in readiness
    assert "provider" not in readiness
    assert "api-key" not in readiness


def test_worker_keeps_compatibility_forwarders_for_runtime_replay_resource_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "from .runtime_replay_resource_policy import" in worker_source
    assert "def _format_agent_runtime_replay_environment_report" in worker_source
    assert "def _format_agent_runtime_replay_resource_readiness_report" in worker_source


def test_runtime_replay_transfer_policy_owns_asset_transfer_report():
    from plugins.AITool.services import runtime_replay_transfer_policy

    assert callable(runtime_replay_transfer_policy.format_agent_runtime_replay_asset_transfer_report)

    report = runtime_replay_transfer_policy.format_agent_runtime_replay_asset_transfer_report({
        "asset_event_count": 4,
        "asset_transfer_started_count": 1,
        "asset_transfer_progress_count": 2,
        "asset_transfer_completed_count": 1,
        "asset_transfer_failed_count": 0,
        "peer_asset_ready_count": 1,
        "latest_transfer_progress": 125,
        "latest_chunk_index": 2,
        "latest_chunk_count": 4,
        "latest_bytes_transferred": 2048,
        "latest_total_bytes": 4096,
        "latest_transfer_status": "in_progress",
    })

    assert report == (
        "events 4, started 1, progress 2, completed 1, failed 0, peer-ready 1, "
        "latest 100% chunk 2/4 2KB/4KB in-progress"
    )


def test_worker_keeps_compatibility_forwarder_for_runtime_replay_transfer_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "from .runtime_replay_transfer_policy import" in worker_source
    assert "def _format_agent_runtime_replay_asset_transfer_report" in worker_source


def test_runtime_replay_peer_sync_policy_owns_peer_sync_report():
    from plugins.AITool.services import runtime_replay_peer_sync_policy

    assert callable(runtime_replay_peer_sync_policy.format_agent_runtime_replay_peer_sync_report)

    report = runtime_replay_peer_sync_policy.format_agent_runtime_replay_peer_sync_report({
        "peer_event_count": 5,
        "peer_join_count": 2,
        "peer_leave_count": 1,
        "room_close_count": 1,
        "sync_reconcile_count": 3,
        "sync_reconcile_failed_count": 1,
        "state_reconcile_count": 2,
        "state_reconcile_failed_count": 0,
        "latest_peer_event_type": "peer_joined",
        "latest_room_status": "room_ready",
        "latest_reconcile_event": {"status": "completed"},
    })

    assert report == (
        "events 5, join 2, leave 1, room-close 1, reconcile 3/1, state 2/0, "
        "latest-peer peer-joined, room room-ready, latest-reconcile completed"
    )


def test_worker_keeps_compatibility_forwarder_for_runtime_replay_peer_sync_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "from .runtime_replay_peer_sync_policy import" in worker_source
    assert "def _format_agent_runtime_replay_peer_sync_report" in worker_source


def test_runtime_sync_policy_owns_actor_and_asset_row_previews():
    from plugins.AITool.services import runtime_sync_policy

    actor_text = runtime_sync_policy.format_agent_runtime_sync_actor_rows([
        {"actor_name": "Hero", "event_type": "actor_updated"},
        {"actor_id": "actor-2", "lifecycle_status": "removed"},
        "ignored",
    ])
    asset_text = runtime_sync_policy.format_agent_runtime_sync_asset_rows([
        {
            "asset_id": "mesh",
            "transfer_status": "in_progress",
            "progress": 50,
            "chunk_index": 2,
            "chunk_count": 4,
            "bytes_transferred": 2048,
            "total_bytes": 4096,
        },
    ])

    assert actor_text == "Hero:actor_updated, actor-2:removed"
    assert asset_text == "mesh:in_progress 50% chunk 2/4 2KB/4KB"


def test_worker_keeps_compatibility_forwarders_for_runtime_sync_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "from .runtime_sync_policy import" in worker_source
    assert "def _format_agent_runtime_sync_actor_rows" in worker_source
    assert "def _format_agent_runtime_sync_asset_rows" in worker_source


def test_runtime_replay_sync_policy_owns_sync_replay_report():
    from plugins.AITool.services import runtime_replay_sync_policy

    assert callable(runtime_replay_sync_policy.format_agent_runtime_sync_replay_report)

    report = runtime_replay_sync_policy.format_agent_runtime_sync_replay_report({
        "recorded_count": 3,
        "failed_count": 1,
        "actor_transform_count": 2,
        "actor_delete_count": 1,
        "peer_join_count": 1,
        "transfer_failed_count": 1,
        "transfer_progress_count": 2,
        "latest_transfer_progress": 125,
        "latest_chunk_index": 1,
        "latest_chunk_count": 2,
        "latest_bytes_transferred": 2048,
        "latest_total_bytes": 4096,
        "latest_event_type": "sync_finished",
        "failure_code_counts": {"transport_timeout": 2},
        "latest_failure_code": "provider_api_key_hidden",
    })

    assert report == (
        "recorded 3, failed 1, actor-transform 2, actor-delete 1, peer-join 1, "
        "transfer-failed 1, transfer-progress 2 latest 100% chunk 1/2 2KB/4KB, "
        "latest sync-finished, failure codes transport-timeout:2, "
        "latest failure resource-resource-hidden"
    )
    assert "provider" not in report
    assert "api-key" not in report


def test_worker_keeps_compatibility_forwarder_for_runtime_replay_sync_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "from .runtime_replay_sync_policy import" in worker_source
    assert "def _format_agent_runtime_sync_replay_report" in worker_source


def test_runtime_sync_policy_owns_health_and_asset_transfer_reports():
    from plugins.AITool.services import runtime_sync_policy

    health = runtime_sync_policy.format_agent_runtime_sync_health_report({
        "status": "degraded",
        "needs_attention": ["actor_missing", "provider_raw"],
        "actor_create_count": 1,
        "actor_transform_count": 2,
        "actor_delete_count": 3,
        "latest_active_actor_count": 4,
        "peer_join_count": 2,
        "peer_leave_count": 1,
        "room_close_count": 1,
    })
    transfer = runtime_sync_policy.format_agent_runtime_asset_transfer_report({
        "asset_count": 2,
        "ready_count": 1,
        "completed_count": 1,
        "transferring_count": 0,
        "failed_count": 1,
        "overall_progress": 125,
        "bytes_transferred": 1024,
        "total_bytes": 4096,
        "latest_assets": [{"asset_id": "mesh", "transfer_status": "done"}],
    })

    assert health == (
        "degraded, attention 2, actors create/transform/delete 1/2/3, active 4, "
        "peers join/leave 2/1, room-close 1, needs actor-missing,provider-raw"
    )
    assert transfer == (
        "assets 2, ready 1, completed 1, transferring 0, failed 1, "
        "progress 100%, bytes 1024/4096, latest mesh:done"
    )


def test_worker_keeps_compatibility_forwarders_for_runtime_sync_summary_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_sync_health_report" in worker_source
    assert "format_agent_runtime_asset_transfer_report" in worker_source
    assert "def _format_agent_runtime_sync_health_report" in worker_source
    assert "def _format_agent_runtime_asset_transfer_report" in worker_source


def test_runtime_message_delivery_policy_owns_delivery_report_redaction():
    from plugins.AITool.services import runtime_message_delivery_policy

    assert callable(runtime_message_delivery_policy.format_agent_runtime_message_delivery_report)

    report = runtime_message_delivery_policy.format_agent_runtime_message_delivery_report(
        {
            "requested_count": 2,
            "succeeded_count": 1,
            "failed_count": 1,
            "message_kind_counts": {"agent_reply": 1},
            "channel_counts": {"provider": 2},
            "latest_message_kind": "agent_reply",
            "latest_channel": "provider",
            "latest_stage": "raw",
            "latest_progress": 125,
            "failure_code_counts": {"api-key": 1},
            "latest_failure_code": "prompt",
        },
        redact_agent_reply=True,
    )

    assert "2" in report
    assert "failure codes" in report
    assert "adapter" in report
    assert "credential" in report
    assert "agent_reply" not in report
    assert "provider" not in report
    assert "prompt" not in report
    assert "raw" not in report


def test_worker_keeps_compatibility_forwarder_for_runtime_message_delivery_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "from .runtime_message_delivery_policy import" in worker_source
    assert "def _format_agent_runtime_message_delivery_report" in worker_source


def test_runtime_report_policy_owns_resource_flow_report():
    from plugins.AITool.services.runtime_report_policy import format_agent_runtime_resource_flow_report

    report = format_agent_runtime_resource_flow_report({
        "batch_count": 3,
        "completed_count": 2,
        "partial_count": 1,
        "failed_count": 1,
        "waiting_count": 0,
        "latest_batch": {
            "status": "partial",
            "batch_index": 2,
            "total_batches": 3,
            "requested_count": 4,
            "image_ready_count": 1,
            "model_ready_count": 2,
            "import_ready_count": 3,
            "review_status": "needs_review",
            "import_failure_code_counts": {"provider_raw": 1},
        },
        "needs_attention": ["import_timeout"],
    })

    assert report == (
        "batches 3, completed 2, partial 1, failed 1, waiting 0, "
        "latest 2/3:partial img/model/import 1/2/3 of 4, review needs-review, "
        "import-failures resource-resource:1, needs import-timeout"
    )
    assert "provider" not in report
    assert "raw" not in report


def test_worker_keeps_compatibility_forwarder_for_runtime_resource_flow_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_resource_flow_report" in worker_source
    assert "def _format_agent_runtime_resource_flow_report" in worker_source


def test_runtime_report_policy_owns_scene_snapshot_and_resource_stage_reports():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_resource_stage_report,
        format_agent_runtime_scene_snapshot_report,
    )

    snapshot = format_agent_runtime_scene_snapshot_report({
        "scoped_snapshot_count": 2,
        "observed_actor_count": 3,
        "observed_actor_total_count": 5,
        "latest_source": "runtime_scene_snapshot",
    })
    stage = format_agent_runtime_resource_stage_report({
        "event_count": 5,
        "by_phase": {
            "image": {"item_count": 1, "requested_count": 2, "failed_count": 0},
            "model": {"item_count": 2, "requested_count": 2, "failed_count": 1},
            "import": {"item_count": 0, "requested_count": 1, "failed_count": 1},
            "review": {"item_count": 3, "requested_count": 3, "failed_count": 0},
            "cache": {"item_count": 4, "requested_count": 5, "failed_count": 0},
        },
        "latest_events": [{"phase": "import", "status": "failed"}],
        "needs_attention": ["import_timeout"],
    })

    assert snapshot == "snapshots 2, observed 3/5, source runtime-scene-snapshot"
    assert stage == (
        "events 5, image 1/2 failed 0, model 2/2 failed 1, import 0/1 failed 1, "
        "review 3/3 failed 0, cache 4/5 failed 0, latest import:failed, needs import-timeout"
    )


def test_worker_keeps_compatibility_forwarders_for_runtime_report_policy_extensions():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_scene_snapshot_report" in worker_source
    assert "format_agent_runtime_resource_stage_report" in worker_source
    assert "def _format_agent_runtime_scene_snapshot_report" in worker_source
    assert "def _format_agent_runtime_resource_stage_report" in worker_source


def test_runtime_report_policy_owns_health_and_fact_source_reports():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_fact_source_boundary_report,
        format_agent_runtime_report_health_report,
    )

    health = format_agent_runtime_report_health_report({
        "status": "needs_review",
        "attention_required": True,
        "batch_failed_count": 1,
        "batch_partial_count": 2,
        "batch_waiting_count": 3,
        "import_failed_count": 4,
        "resource_phase_failed_count": 5,
        "resource_phase_partial_count": 6,
        "resource_phase_waiting_count": 7,
        "asset_failed_count": 8,
        "asset_incomplete_count": 9,
        "sync_health_status": "provider_partial",
        "import_failure_code_counts": {"provider_raw": 2},
        "sync_failure_code_counts": {"prompt_timeout": 1},
        "latest_sync_failure_code": "api-key-hidden",
        "engine_write_readiness_mismatch_count": 1,
        "engine_write_readiness_mismatch_channels": ["layout_transform"],
        "engine_write_runtime_state_only_count": 1,
        "engine_write_runtime_state_only_channels": ["provider_raw"],
        "worker_drain_failed_count": 1,
        "worker_drain_exception_count": 2,
        "worker_drain_status_failed_count": 3,
        "worker_drain_plan_resolve_failed_count": 4,
        "reasons": ["provider_unavailable"],
    })
    fact_source = format_agent_runtime_fact_source_boundary_report({
        "runtime_business_fact_count": 4,
        "mirrored_external_fact_count": 2,
        "runtime_plan_fact_count": 1,
        "runtime_batch_fact_count": 2,
        "runtime_resource_event_count": 3,
        "runtime_import_event_count": 4,
        "sync_event_count": 5,
        "engine_write_result_count": 6,
        "engine_write_boundary_fact_count": 8,
        "scene_snapshot_count": 7,
        "external_authoritative_available": True,
        "boundary_notes": ["snapshot_pending"],
    })

    assert "needs-review" in health
    assert "batch failed/partial/waiting 1/2/3" in health
    assert "engine-write mismatch 1(layout-transform)" in health
    assert "worker-drain failed/status-failed/exception/plan-resolve 1/3/2/4" in health
    assert "provider" not in health
    assert "prompt" not in health
    assert "raw" not in health
    assert "api-key" not in health
    assert fact_source == (
        "runtime 4, external 2, plan/batch 1/2, resource/import 3/4, "
        "sync/write/snapshot 5/6/7, write-boundary 8, external available, "
        "notes snapshot-pending"
    )


def test_worker_keeps_compatibility_forwarders_for_runtime_report_health_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_report_health_report" in worker_source
    assert "format_agent_runtime_fact_source_boundary_report" in worker_source
    assert "def _format_agent_runtime_report_health_report" in worker_source
    assert "def _format_agent_runtime_fact_source_boundary_report" in worker_source


def test_runtime_report_policy_owns_closure_and_import_stage_reports():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_closure_report,
        format_agent_runtime_import_stage_report,
    )

    closure = format_agent_runtime_closure_report(
        {"runtime_state_source": "runtime_state", "engine_write_boundary_fact_count": 4},
        {"applied": 1, "conflict": 2, "invalid": 3},
        operation_count=2,
        operation_total_count=3,
    )
    import_stage = format_agent_runtime_import_stage_report({
        "event_count": 5,
        "imported_count": 4,
        "requested_count": 6,
        "failed_count": 2,
        "latest_events": [{"status": "partial_import"}],
    })

    assert closure == (
        "state runtime_state, operation 2/3, patch applied/conflict/invalid 1/2/3, "
        "write-boundary 4"
    )
    assert import_stage == "events 5, imported 4/6, failed 2, latest partial-import"


def test_worker_keeps_compatibility_forwarders_for_runtime_closure_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_closure_report" in worker_source
    assert "format_agent_runtime_import_stage_report" in worker_source
    assert "def _format_agent_runtime_closure_report" in worker_source
    assert "def _format_agent_runtime_import_stage_report" in worker_source


def test_runtime_report_policy_owns_actor_import_boundary_and_queue_health_reports():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_actor_import_boundary_report,
        format_agent_runtime_tool_queue_health_report,
    )

    actor_import = format_agent_runtime_actor_import_boundary_report(
        {"requested_count": 4, "imported_count": 3, "failed_count": 1},
        {"entity_type_counts": {"actor": 5}},
        {
            "bridge_call_count": 2,
            "bridge_success_count": 1,
            "bridge_failed_count": 1,
            "status_counts": {"runtime_state_only": 3},
        },
    )
    queue = format_agent_runtime_tool_queue_health_report({
        "queue_count": 8,
        "active_count": 2,
        "queued_count": 3,
        "running_count": 2,
        "blocked_count": 1,
        "terminal_count": 5,
        "queue_pressure": 1.25,
    })

    assert actor_import == "requested/imported/failed 4/3/1, registered actor 5, bridge 1/2, failed 1"
    assert queue == "queue 8, active 2, queued/running 3/2, blocked 1, terminal 5, pressure 100%"


def test_worker_keeps_compatibility_forwarders_for_actor_import_queue_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_actor_import_boundary_report" in worker_source
    assert "format_agent_runtime_tool_queue_health_report" in worker_source
    assert "def _format_agent_runtime_actor_import_boundary_report" in worker_source
    assert "def _format_agent_runtime_tool_queue_health_report" in worker_source


def test_runtime_report_policy_owns_batch_tooling_and_resource_lifecycle_reports():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_batch_resource_lifecycle_report,
        format_agent_runtime_batch_tooling_report,
    )

    tooling = format_agent_runtime_batch_tooling_report({
        "fact_count": 6,
        "created_batch_fact_count": 4,
        "created_batch_count": 3,
        "prioritized_item_count": 2,
        "merged_intervention_fact_count": 5,
        "merged_intervention_item_count": 4,
        "absorbed_intervention_count": 1,
        "latest_fact_types": ["batch_plan_created", "tool_graph_queued"],
    })
    lifecycle = format_agent_runtime_batch_resource_lifecycle_report({
        "resource_event_count": 8,
        "image_ready_count": 2,
        "image_failed_count": 1,
        "model_ready_count": 3,
        "model_failed_count": 0,
        "import_ready_count": 4,
        "import_failed_count": 1,
        "environment_ready_count": 5,
        "environment_failed_count": 2,
        "emit_failed_count": 1,
        "latest_resource_event": {"stage": "model_load", "persisted": True},
    })

    assert tooling == (
        "facts 6, created-batches 3/4, priorities 2, merged 4/5, absorbed 1, "
        "latest batch-plan-created,tool-graph-queued"
    )
    assert lifecycle == "events 8, image 2/1, model 3/0, import 4/1, env 5/2, emit-failed 1, latest model-load:persisted"


def test_worker_keeps_compatibility_forwarders_for_batch_report_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_batch_tooling_report" in worker_source
    assert "format_agent_runtime_batch_resource_lifecycle_report" in worker_source
    assert "def _format_agent_runtime_batch_tooling_report" in worker_source
    assert "def _format_agent_runtime_batch_resource_lifecycle_report" in worker_source


def test_runtime_report_policy_owns_geometry_and_command_reports():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_command_report,
        format_agent_runtime_geometry_fact_report,
    )

    geometry = format_agent_runtime_geometry_fact_report({
        "fact_count": 3,
        "aabb_actor_count": 2,
        "aabb_skipped_count": 1,
        "overlap_issue_count": 2,
        "status_counts": {"needs_review": 1},
        "fact_type_counts": {"aabb_fact": 2},
    })
    commands = format_agent_runtime_command_report({
        "command_count": 2,
        "latest_commands": [
            {"command": "provider_raw", "old_status": "prompt_waiting", "new_status": "completed"},
        ],
    })

    assert geometry == "3 fact(s)；AABB actors 2；overlap issues 2；skipped 1；status needs-review:1；type aabb-fact:2"
    assert commands == "2 command(s)；latest runtime-runtime:runtime-waiting->completed"
    assert "provider" not in commands
    assert "prompt" not in commands
    assert "raw" not in commands


def test_worker_keeps_compatibility_forwarders_for_geometry_command_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_geometry_fact_report" in worker_source
    assert "format_agent_runtime_command_report" in worker_source
    assert "def _format_agent_runtime_geometry_fact_report" in worker_source
    assert "def _format_agent_runtime_command_report" in worker_source


def test_runtime_report_policy_owns_review_proposal_and_confirmation_reports():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_review_confirmation_report,
        format_agent_runtime_review_proposal_report,
    )

    proposal = format_agent_runtime_review_proposal_report({
        "proposal_count": 2,
        "item_count": 5,
        "status_counts": {"proposed": 1, "confirmed": 1},
    })
    confirmation = format_agent_runtime_review_confirmation_report({
        "confirmation_count": 2,
        "decision_counts": {"confirmed": 1, "rejected": 1},
    })

    assert proposal == "2 proposal(s)；items 5；waiting host confirmation；status confirmed:1,proposed:1"
    assert confirmation == "2 confirmation(s)；decision confirmed:1,rejected:1"


def test_worker_keeps_compatibility_forwarders_for_review_report_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_review_proposal_report" in worker_source
    assert "format_agent_runtime_review_confirmation_report" in worker_source
    assert "def _format_agent_runtime_review_proposal_report" in worker_source
    assert "def _format_agent_runtime_review_confirmation_report" in worker_source


def test_runtime_report_policy_owns_layout_report():
    from plugins.AITool.services.runtime_report_policy import format_agent_runtime_layout_report

    report = format_agent_runtime_layout_report(
        {
            "proposal_count": 2,
            "proposals": [
                {"status": "proposed", "delta_count": 2, "risk_level": "high"},
                {"status": "confirmed", "delta_count": 3, "risk_level": "medium"},
            ],
            "applied_delta_count": 2,
            "skipped_delta_count": 1,
            "transform_result_count": 3,
            "ground_snapped_count": 1,
            "overlap_resolved_count": 1,
            "layout_transform_failure_code_counts": {"provider_raw": 1},
        },
        {"confirmation_count": 1},
    )

    assert report == (
        "2 proposal(s)；deltas 5；applied 2；skipped 1；transforms 3；ground-snapped 1；"
        "overlap-resolved 1；transform-failures redacted:1；confirmations 1；"
        "risk high,medium；status confirmed:1,proposed:1"
    )
    assert "provider" not in report
    assert "raw" not in report


def test_worker_keeps_compatibility_forwarder_for_layout_report_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_layout_report" in worker_source
    assert "def _format_agent_runtime_layout_report" in worker_source


def test_runtime_report_policy_owns_engine_write_report_and_readiness():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_engine_write_readiness_report,
        format_agent_runtime_engine_write_report,
    )

    report = format_agent_runtime_engine_write_report({
        "import_result_count": 2,
        "transform_result_count": 3,
        "environment_import_result_count": 1,
        "delete_result_count": 1,
        "import_status_counts": {"completed": 2},
        "transform_status_counts": {"provider_raw": 1},
        "readiness_mismatch_count": 1,
        "readiness_mismatch_channels": ["layout_transform"],
        "status_export_count": 1,
        "latest_status_export": {
            "recorded": True,
            "engine_write_bridge_failed_count": 1,
            "engine_write_readiness_native_enabled_count": 1,
            "engine_write_readiness_native_enabled_channels": ["actor_import"],
            "engine_write_bridge_error_code_counts": {"provider_raw": 1},
        },
    })
    readiness = format_agent_runtime_engine_write_readiness_report({
        "channel_count": 3,
        "native_enabled_count": 1,
        "native_enabled_channels": ["actor_import"],
        "runtime_state_only_count": 1,
        "runtime_state_only_channels": ["layout_transform"],
        "fallback_count": 0,
        "disabled_count": 1,
        "unavailable_count": 1,
        "unavailable_channels": ["provider_raw"],
    })

    assert "import 2(completed:2)" in report
    assert "transform 3" in report
    assert "readiness-mismatch 1(layout-transform)" in report
    assert "status-export 1" in report
    assert "bridge-failed:1" in report
    assert "provider" not in report
    assert "raw" not in report
    assert readiness == (
        "channels 3, native 1(actor_import), runtime-state 1(layout_transform), "
        "fallback 0, disabled 1, unavailable 1(provider_raw)"
    )


def test_worker_keeps_compatibility_forwarders_for_engine_write_report_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_engine_write_report" in worker_source
    assert "format_agent_runtime_engine_write_readiness_report" in worker_source
    assert "def _format_agent_runtime_engine_write_report" in worker_source
    assert "def _format_agent_runtime_engine_write_readiness_report" in worker_source


def test_runtime_report_policy_owns_engine_write_boundary_report():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_engine_write_boundary_report,
    )

    report = format_agent_runtime_engine_write_boundary_report({
        "boundary_fact_count": 4,
        "import_boundary_count": 2,
        "transform_boundary_count": 1,
        "delete_boundary_count": 1,
        "write_source_counts": {"runtime_state": 2, "provider_raw": 3},
        "status_counts": {"runtime_state_only": 1, "completed": 2},
        "bridge_call_count": 2,
        "bridge_success_count": 2,
        "bridge_failed_count": 0,
        "bridge_skipped_count": 1,
        "bridge_error_code_counts": {"provider_raw": 2},
        "bridge_skip_reason_counts": {"not_ready": 1},
    })

    assert report == (
        "boundary 4, import/transform/delete 2/1/1, "
        "sources runtime_runtime:3,runtime_state:2, statuses completed:2,runtime_state_only:1, "
        "bridge 2/2/0, skipped 1(not_ready:1), errors runtime_runtime:2, "
        "native verified"
    )
    assert "provider" not in report
    assert "raw" not in report


def test_worker_keeps_compatibility_forwarder_for_engine_write_boundary_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_engine_write_boundary_report" in worker_source
    assert "def _format_agent_runtime_engine_write_boundary_report" in worker_source


def test_runtime_report_policy_owns_resource_report_and_readiness():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_resource_readiness_report,
        format_agent_runtime_resource_report,
    )

    report = format_agent_runtime_resource_report({
        "scene_snapshot": {"status": "enabled"},
        "image_resource": {"status": "unavailable", "reason": "provider_timeout"},
        "model_resource": {"mode": "fallback", "reason": "not_configured"},
        "actor_import": {"status": "disabled"},
    })
    readiness = format_agent_runtime_resource_readiness_report({
        "channel_count": 3,
        "requested_count": 2,
        "enabled_count": 1,
        "unavailable_count": 1,
        "unavailable_channels": ["provider_raw"],
    })

    assert report == (
        "scene-snapshot:enabled、image-resource:unavailable(adapter-timeout)、"
        "model-resource:fallback(not-configured)、actor-import:disabled"
    )
    assert readiness == (
        "channels 3, requested 2, enabled 1, unavailable 1, unavailable provider-raw"
    )


def test_worker_keeps_compatibility_forwarders_for_resource_report_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_resource_report" in worker_source
    assert "format_agent_runtime_resource_readiness_report" in worker_source
    assert "def _format_agent_runtime_resource_report" in worker_source
    assert "def _format_agent_runtime_resource_readiness_report" in worker_source


def test_runtime_report_policy_owns_review_report():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_review_report,
    )

    report = format_agent_runtime_review_report({
        "review_count": 2,
        "issue_count": 3,
        "advisory_count": 1,
        "status_counts": {"needs_adjustment": 1, "completed": 1},
        "checkpoint_counts": {"structure_review": 2},
    })

    assert report == (
        "2 review(s)；issues 3；advisory 1；"
        "status completed:1,needs-adjustment:1；checkpoint structure-review:2"
    )


def test_worker_keeps_compatibility_forwarder_for_review_report_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_review_report" in worker_source
    assert "def _format_agent_runtime_review_report" in worker_source


def test_runtime_sync_policy_owns_sync_report():
    from plugins.AITool.services.runtime_sync_policy import (
        format_agent_runtime_sync_report,
    )

    report = format_agent_runtime_sync_report({
        "event_count": 3,
        "actor_event_count": 2,
        "asset_event_count": 1,
        "latest_actors": [{"actor_id": "hero", "event_type": "updated"}],
        "latest_assets": [{"asset_id": "mesh-1", "status": "completed", "progress": 100}],
    })

    assert report == (
        "events 3, actors 2, assets 1, "
        "latest actors hero:updated, latest assets mesh-1:completed 100%"
    )


def test_worker_keeps_compatibility_forwarder_for_sync_report_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_sync_report" in worker_source
    assert "def _format_agent_runtime_sync_report" in worker_source


def test_runtime_report_policy_owns_context_report():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_context_report,
    )

    report = format_agent_runtime_context_report({
        "context_count": 2,
        "context_type_counts": {"scene_update": 1, "provider_prompt": 1},
        "speaker_type_counts": {"user": 1, "system": 1},
        "latest_context": [{
            "context_type": "scene_update",
            "speaker_type": "user",
            "message": "raw provider prompt",
        }],
    })

    assert report == (
        "2 context(s)；types runtime-runtime:1,scene-update:1；"
        "speakers system:1,user:1；latest scene-update/user:runtime runtime runtime"
    )
    assert "prompt" not in report
    assert "provider" not in report
    assert "raw" not in report


def test_worker_keeps_compatibility_forwarder_for_context_report_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_context_report" in worker_source
    assert "def _format_agent_runtime_context_report" in worker_source


def test_runtime_replay_report_policy_owns_worker_drain_report():
    from plugins.AITool.services.runtime_replay_report_policy import (
        format_agent_runtime_worker_drain_replay_report,
    )

    report = format_agent_runtime_worker_drain_replay_report({
        "requested_count": 4,
        "message_drained_count": 3,
        "drained_graph_total": 5,
        "failed_count": 1,
        "exception_count": 1,
        "status_failed_count": 1,
        "plan_resolve_failed_count": 1,
        "latest_drain_event": {"event": "status_failed"},
    })

    assert report == (
        "requested 4, drained 3/5, failed 1, exception 1, status-failed 1, "
        "plan-resolve-failed 1, latest status-failed"
    )


def test_worker_keeps_compatibility_forwarder_for_worker_drain_replay_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_worker_drain_replay_report" in worker_source
    assert "def _format_agent_runtime_worker_drain_replay_report" in worker_source


def test_runtime_replay_report_policy_owns_tool_graph_and_gm_summary_reports():
    from plugins.AITool.services.runtime_replay_report_policy import (
        format_agent_runtime_gm_summary_replay_report,
        format_agent_runtime_tool_graph_replay_report,
    )

    tool_graph = format_agent_runtime_tool_graph_replay_report(
        {"started_count": 3, "completed_count": 2, "finalized_count": 1},
        {"queued_count": 4, "dequeued_count": 3, "rejected_count": 1, "blocked_count": 2},
    )
    gm_summary = format_agent_runtime_gm_summary_replay_report({
        "exported_count": 2,
        "failed_count": 1,
        "available_count": 1,
        "scene_plan_count": 3,
        "resource_readiness_publish_total": 4,
        "resource_readiness_query_total": 5,
    })

    assert tool_graph == (
        "batch start/done/final 3/2/1, queue queued/dequeued/rejected/blocked 4/3/1/2"
    )
    assert gm_summary == (
        "exported 2, failed 1, available 1, scene-plan 3, readiness publish/query 4/5"
    )


def test_worker_keeps_compatibility_forwarders_for_tool_graph_and_gm_summary_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_tool_graph_replay_report" in worker_source
    assert "format_agent_runtime_gm_summary_replay_report" in worker_source
    assert "def _format_agent_runtime_tool_graph_replay_report" in worker_source
    assert "def _format_agent_runtime_gm_summary_replay_report" in worker_source


def test_runtime_report_policy_owns_intervention_digest():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_intervention_digest,
    )

    report = format_agent_runtime_intervention_digest({
        "pending_count": 4,
        "accepted_count": 2,
        "deferred_count": 1,
        "absorbable_pending_count": 3,
        "non_absorbable_pending_count": 1,
    })

    assert report == "pending 4, accepted 2, deferred 1, absorbable 3, needs-confirmation 1"


def test_worker_keeps_compatibility_forwarder_for_intervention_digest_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_intervention_digest" in worker_source
    assert "def _format_agent_runtime_intervention_digest" in worker_source


def test_runtime_sync_policy_owns_gm_sync_replay_digest():
    from plugins.AITool.services.runtime_sync_policy import (
        format_agent_runtime_gm_sync_replay_digest,
    )

    report = format_agent_runtime_gm_sync_replay_digest({
        "recorded_count": 5,
        "failed_count": 1,
        "actor_transform_count": 2,
        "actor_delete_count": 1,
        "asset_transfer_progress_count": 3,
        "asset_transfer_completed_count": 2,
        "asset_transfer_failed_count": 1,
        "peer_asset_ready_count": 1,
        "peer_join_count": 2,
        "peer_leave_count": 1,
        "sync_reconcile_count": 4,
        "sync_reconcile_failed_count": 1,
        "failure_code_counts": {"network_timeout": 1, "timeout": 2},
        "latest_failure_code": "network_timeout",
    })

    assert report == (
        "recorded 5/1；actor transform/delete 2/1；asset progress 3；"
        "asset completed/failed 2/1；peer-ready 1；peer join/leave 2/1；"
        "reconcile 4/1；failure codes network-timeout:1, timeout:2；"
        "latest failure network-timeout"
    )


def test_worker_keeps_compatibility_forwarder_for_gm_sync_replay_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_gm_sync_replay_digest" in worker_source
    assert "def _format_agent_runtime_gm_sync_replay_digest" in worker_source


def test_runtime_replay_event_policy_owns_gm_runtime_event_digest():
    from plugins.AITool.services.runtime_replay_event_policy import (
        format_agent_runtime_gm_runtime_event_replay_digest,
    )

    report = format_agent_runtime_gm_runtime_event_replay_digest({
        "emitted_count": 2,
        "emit_failed_count": 1,
        "disclosure_skipped_count": 1,
        "report_ready_count": 1,
        "report_attention_count": 1,
        "report_health_status_counts": {"ok": 2},
        "latest_report_ready": {
            "environment_import_failure_code_counts": {"timeout": 1},
            "engine_write_bridge_failed_count": 1,
            "engine_write_readiness_mismatch_count": 1,
            "engine_write_readiness_mismatch_channels": ["provider_raw"],
        },
        "latest_disclosure_skip": {"event_type": "provider_prompt", "audience": "user"},
    })

    assert report == (
        "emitted 2, failed 1, skipped 1, report-ready 1/attention 1 ok:2, "
        "env-import-failures timeout:1, engine-write-failures 1, "
        "engine-write-mismatch 1(resource-resource), latest-skip resource-resource:user"
    )


def test_worker_keeps_compatibility_forwarder_for_gm_runtime_event_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_gm_runtime_event_replay_digest" in worker_source
    assert "def _format_agent_runtime_gm_runtime_event_replay_digest" in worker_source


def test_runtime_report_policy_owns_intervention_reply():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_intervention_reply,
    )

    reply = format_agent_runtime_intervention_reply({
        "plan": {"plan_id": "plan-1"},
        "patch": {
            "patch_type": "add_actor",
            "status": "recorded",
            "items": ["hero", "enemy"],
        },
    })

    assert reply == "【AgentRuntime 介入结果】ScenePlan plan-1 已记录 add-actor，状态 recorded，对象 2 个。"


def test_worker_keeps_compatibility_forwarder_for_intervention_reply_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_intervention_reply" in worker_source
    assert "def _format_agent_runtime_intervention_reply" in worker_source


def test_runtime_report_policy_owns_layout_confirmation_reply():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_layout_confirmation_reply,
    )

    reply = format_agent_runtime_layout_confirmation_reply({
        "graph": {"status": "completed"},
        "proposal": {
            "plan_id": "plan-1",
            "proposal_id": "proposal-1",
            "applied_deltas": [{"id": "a"}],
            "skipped_deltas": [{"id": "b"}],
            "engine_transform_results": [
                {"status": "success", "ground_snapped": True},
                {"status": "failed", "overlap_resolved": True},
            ],
        },
    })

    assert reply == (
        "【AgentRuntime 布局结果】ScenePlan plan-1 建议 proposal-1 已通过 ToolCallGraph 确认，"
        "graph completed，应用 1 项，跳过 1 项，引擎写入成功 1 项、失败 1 项，贴地 1 项，重叠修正 1 项。"
    )


def test_worker_keeps_compatibility_forwarder_for_layout_confirmation_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_layout_confirmation_reply" in worker_source
    assert "def _format_agent_runtime_layout_confirmation_reply" in worker_source


def test_runtime_replay_report_policy_owns_replay_aggregate_composition():
    from plugins.AITool.services.runtime_replay_report_policy import (
        format_agent_runtime_replay_report,
    )

    report = format_agent_runtime_replay_report({
        "entry_count": 3,
        "event_counts": {"scene_plan_created": 2, "tool_graph_completed": 1},
        "latest_events": [{"event": "scene_plan_created"}, {"event": "provider_raw"}],
        "environment_component_replay_summary": {"import_event_count": 2},
        "runtime_event_replay_summary": {"disclosure_skipped_count": 1},
        "worker_drain_replay_summary": {"failed_count": 1},
        "engine_write_boundary_summary": {"boundary_fact_count": 1},
    }, runtime_event_text="emitted 1, failed 0, skipped 1",
       worker_drain_text="requested 1, drained 0, failed 1, exception 0",
       engine_write_boundary_text="boundary 1")

    assert report == (
        "entries 3；events scene-plan-created:2,tool-graph-completed:1；"
        "environment env-import:2；runtime-events emitted 1, failed 0, skipped 1；"
        "worker-drain requested 1, drained 0, failed 1, exception 0；"
        "engine_write_boundary boundary 1；recent scene-plan-created,runtime-runtime"
    )


def test_worker_keeps_compatibility_forwarder_for_replay_aggregate_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_replay_report" in worker_source
    assert "def _format_agent_runtime_replay_report" in worker_source


def test_runtime_report_policy_owns_execution_reply_composition():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_execution_reply,
    )

    common = {
        "plan_id": "plan-1",
        "batch_count": 2,
        "graph_status_text": "completed:2",
        "health_text": "healthy",
        "registry_text": "实体注册：3 个",
        "classification_text": "Classification：model/substrate 2/1",
        "flow_text": "Flow：completed done",
        "tool_state_text": "Tool/State：tools ok/fail/block 2/0/0",
        "guard_text": "Guard：block/write/system 0/0/0",
        "queue_text": "Queue：total/queued/running/active/block 0/0/0/0/0",
        "drain_text": "Drain：drained，drained 2",
        "batch_tooling_text": "BatchTooling：facts/created/prioritized/merged/absorbed 1/1/1/0/0",
        "report_source_text": "ReportSource：state runtime，operation 1/1",
        "engine_text": "Engine写入：bridge 2/2 成功",
    }

    reply = format_agent_runtime_execution_reply({**common, "state": "completed"})

    assert reply == (
        "【AgentRuntime 执行结果】ScenePlan plan-1 已执行 Runtime 批次 2 个，"
        "执行图 completed:2，报告健康：healthy。实体注册：3 个；Classification：model/substrate 2/1；"
        "Flow：completed done；Tool/State：tools ok/fail/block 2/0/0；Guard：block/write/system 0/0/0；"
        "Queue：total/queued/running/active/block 0/0/0/0/0；Drain：drained，drained 2；"
        "BatchTooling：facts/created/prioritized/merged/absorbed 1/1/1/0/0；"
        "ReportSource：state runtime，operation 1/1；Engine写入：bridge 2/2 成功。"
    )


def test_worker_keeps_compatibility_forwarder_for_execution_reply_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_execution_reply" in worker_source
    assert "def _format_agent_runtime_execution_reply" in worker_source


def test_runtime_report_policy_owns_intervention_summary_labels():
    from plugins.AITool.services.runtime_report_policy import (
        format_agent_runtime_intervention_batch_summary,
        format_agent_runtime_intervention_summary,
    )

    summary = format_agent_runtime_intervention_summary({
        "pending_count": 2,
        "accepted_count": 1,
        "deferred_count": 3,
        "latest_pending": [{"items": ["hero", "enemy"]}],
    })
    batches = format_agent_runtime_intervention_batch_summary({
        "batch_count": 2,
        "status_counts": {"queued": 1, "completed": 1},
        "latest_batches": [{
            "batch_index": 1,
            "total_batches": 2,
            "requested_items": ["hero", "enemy", "terrain", "skybox"],
        }],
    })

    assert summary == "待处理 2；已吸收 1；延后 3；最近待处理：hero、enemy"
    assert batches == "2 batch(es)；状态 {'queued': 1, 'completed': 1}；最近第 1/2 批：hero、enemy、terrain 等 4 项"


def test_worker_keeps_compatibility_forwarders_for_intervention_summary_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_intervention_summary" in worker_source
    assert "format_agent_runtime_intervention_batch_summary" in worker_source
    assert "def _format_agent_runtime_intervention_summary" in worker_source
    assert "def _format_agent_runtime_intervention_batch_summary" in worker_source


def test_runtime_replay_event_policy_owns_event_rows():
    from plugins.AITool.services.runtime_replay_event_policy import (
        format_agent_runtime_event_rows,
    )

    rows = format_agent_runtime_event_rows([
        {"title": "Import", "message": "done", "progress": 120},
        {"title": "", "message": "waiting"},
        {"title": "empty", "message": "", "progress": 50},
    ])

    assert rows == [
        ("Import 100%: done", {"title": "Import", "message": "done", "progress": 120}),
        ("状态更新: waiting", {"title": "", "message": "waiting"}),
        ("empty 50%", {"title": "empty", "message": "", "progress": 50}),
    ]


def test_worker_keeps_compatibility_forwarders_for_event_rows_policy():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "format_agent_runtime_event_rows" in worker_source
    assert "def _format_agent_runtime_event_rows" in worker_source


def test_worker_does_not_duplicate_staticmethod_decorator_for_replay_report():
    from pathlib import Path

    worker_source = (
        Path(__file__).resolve().parents[1]
        / "services"
        / "lanchat_agent_worker.py"
    ).read_text(encoding="utf-8")

    assert "@staticmethod\n    @staticmethod\n    def _format_agent_runtime_replay_report" not in worker_source

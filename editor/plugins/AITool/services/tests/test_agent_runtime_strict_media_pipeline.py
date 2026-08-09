from __future__ import annotations

import unittest

from editor.plugins.AITool.services.agent_runtime import (
    AgentRuntime,
    AgentRuntimeFlags,
    make_image_resource_provider,
    make_model_resource_provider,
)
from editor.plugins.AITool.services.lanchat_agent_worker import LANChatAgentWorker


class StrictMediaPipelineTests(unittest.TestCase):
    @staticmethod
    def _media_resolver(file_id: str, timeout: float = 0.0) -> dict:
        _ = timeout
        content = f"png-bytes:{file_id}".encode("utf-8")
        return {
            "image_url": "data:image/png;base64,cG5nLWJ5dGVz",
            "content_bytes": content,
        }

    def test_adapter_preserves_image_to_model_lineage(self) -> None:
        model_payloads: list[dict] = []

        image_provider = make_image_resource_provider(
            image_tool=lambda payload: {
                "llm_content": [{
                    "part": [{
                        "content_type": "image",
                        "content_url": f"fileid://{payload['object_id']}.png",
                    }],
                }],
            },
            media_resolver=self._media_resolver,
        )

        def model_tool(payload: dict) -> dict:
            model_payloads.append(dict(payload))
            return {"model_folder": "assets/model/runtime_key"}

        model_provider = make_model_resource_provider(
            model_tool=model_tool,
            require_image_input=True,
        )
        images = image_provider({
            "batch_id": "batch-lineage",
            "model_items": ["key"],
        })
        models = model_provider({
            "batch_id": "batch-lineage",
            "model_items": ["key"],
            "image_resources": images,
        })

        self.assertEqual(model_payloads[0]["mode"], "image_to_3d")
        self.assertEqual(models["key"]["generation_mode"], "image_to_3d")
        self.assertEqual(models["key"]["source_image_ref"], images["key"]["resource_ref"])
        self.assertEqual(models["key"]["source_image_hash"], images["key"]["content_hash"])
        self.assertTrue(images["key"]["prompt_hash"].startswith("sha256:"))
        self.assertTrue(images["key"]["content_hash"].startswith("sha256:"))

    def test_fileid_timeout_and_missing_hash_are_explicit(self) -> None:
        timeout_provider = make_image_resource_provider(
            image_tool=lambda _payload: {"content_url": "fileid://pending"},
            media_resolver=lambda _file_id, timeout=0.0: (_ for _ in ()).throw(TimeoutError()),
        )
        timeout_result = timeout_provider({"batch_id": "batch-timeout", "model_items": ["key"]})
        self.assertEqual(timeout_result["key"]["failure_code"], "image_resource_timeout")

        hashless_provider = make_image_resource_provider(
            image_tool=lambda _payload: {"content_url": "https://example.invalid/image.png"},
        )
        hashless_result = hashless_provider({"batch_id": "batch-hashless", "model_items": ["door"]})
        self.assertEqual(hashless_result["door"]["failure_code"], "image_content_hash_missing")

    def test_strict_model_adapter_does_not_fall_back_to_text_to_3d(self) -> None:
        calls: list[dict] = []
        provider = make_model_resource_provider(
            model_tool=lambda payload: calls.append(dict(payload)) or {"model_folder": "unused"},
            require_image_input=True,
        )

        resources = provider({
            "batch_id": "batch-missing-image",
            "model_items": ["door"],
            "image_resources": {},
        })

        self.assertEqual(calls, [])
        self.assertEqual(resources["door"]["status"], "failed")
        self.assertEqual(resources["door"]["failure_code"], "source_image_lineage_missing")

    def test_strict_runtime_blocks_when_real_image_provider_is_missing(self) -> None:
        runtime = AgentRuntime(
            model_resource_provider=lambda payload: {},
            strict_image_to_model_pipeline=True,
        )
        plan = runtime.propose_scene_plan(
            room_id="room-strict-missing-image",
            text="设计一个卧室，有钥匙和门",
            owner_agent="GM",
        )
        runtime.confirm_scene_plan(plan.plan_id, confirmed_by="host")

        result = runtime.execute_planned_batches(plan.plan_id, max_items_per_batch=2)
        graph = result["graphs"][0]
        image_nodes = [
            node for node in graph["nodes"].values()
            if node["tool_name"] == "runtime.asset.image.prepare"
        ]
        model_nodes = [
            node for node in graph["nodes"].values()
            if node["tool_name"] == "runtime.asset.model.prepare"
        ]

        self.assertEqual(graph["status"], "failed")
        self.assertEqual(image_nodes[0]["status"], "failed")
        self.assertEqual(model_nodes[0]["status"], "skipped")

    def test_strict_runtime_carries_lineage_into_asset_facts(self) -> None:
        image_provider = make_image_resource_provider(
            image_tool=lambda payload: {
                "image_url": f"fileid://{payload['object_id']}.png",
            },
            media_resolver=self._media_resolver,
        )
        model_provider = make_model_resource_provider(
            model_tool=lambda _payload: {"model_folder": "assets/model/demo.glb"},
            require_image_input=True,
        )
        runtime = AgentRuntime(
            image_resource_provider=image_provider,
            model_resource_provider=model_provider,
            strict_image_to_model_pipeline=True,
        )
        plan = runtime.propose_scene_plan(
            room_id="room-strict-lineage",
            text="设计一个卧室，有钥匙和门",
            owner_agent="GM",
        )
        runtime.confirm_scene_plan(plan.plan_id, confirmed_by="host")

        result = runtime.execute_planned_batches(plan.plan_id, max_items_per_batch=2)
        state = runtime.query_state("room-strict-lineage")["room"]
        batch_id = result["batches"][0]["batch_id"]
        model_resource = next(iter(state["model_resource_plans"][batch_id].values()))
        asset = state["assets"][model_resource["name"]]

        self.assertEqual(result["graphs"][0]["status"], "completed")
        self.assertEqual(model_resource["generation_mode"], "image_to_3d")
        self.assertEqual(asset["source_image_hash"], model_resource["source_image_hash"])
        self.assertEqual(asset["source_image_ref"], model_resource["source_image_ref"])

        rows = LANChatAgentWorker._runtime_media_lineage_rows(state, plan.plan_id)
        self.assertTrue(rows)
        row = rows[0]
        self.assertEqual(row["image_mode"], "text_to_image")
        self.assertEqual(row["model_mode"], "image_to_3d")
        self.assertEqual(row["source_image_ref"], row["image_ref"])
        self.assertEqual(row["source_image_hash"], row["image_hash"])
        self.assertTrue(row["actor_id"])

        worker = LANChatAgentWorker(
            agent_runtime=runtime,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
            async_agent_execution=False,
        )
        with self.assertLogs(
            "editor.plugins.AITool.services.lanchat_agent_worker",
            level="INFO",
        ) as captured:
            worker._log_media_lineage_evidence(
                room_id="room-strict-lineage",
                plan_id=plan.plan_id,
            )
            worker._log_media_lineage_evidence(
                room_id="room-strict-lineage",
                plan_id=plan.plan_id,
            )
        lineage_logs = [line for line in captured.output if "[R3MediaLineageTrace]" in line]
        self.assertEqual(len(lineage_logs), len(rows))


if __name__ == "__main__":
    unittest.main()

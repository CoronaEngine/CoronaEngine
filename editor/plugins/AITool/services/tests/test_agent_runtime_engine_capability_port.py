from __future__ import annotations

import unittest

from editor.plugins.AITool.services.agent_collaboration.walking_skeleton import (
    EngineCapabilityManifest,
    RuntimeEngineCapabilityPort,
    build_skeleton_manifest,
)
from editor.plugins.AITool.services.agent_runtime.adapters import (
    make_engine_capability_manifest_reader,
)


class _CapabilityTool:
    def __init__(self, result) -> None:
        self._result = result
        self.calls: list[dict[str, object]] = []

    def invoke(self, payload: dict[str, object]):
        self.calls.append(dict(payload))
        if isinstance(self._result, BaseException):
            raise self._result
        return self._result


class EngineCapabilityPortTests(unittest.TestCase):
    def test_reader_normalizes_legacy_manifest_and_port_returns_manifest(self) -> None:
        tool = _CapabilityTool(
            {
                "status": "success",
                "capability_contract_version": "r3-engine-week1-v1",
                "engine_bridge_version": "native-bridge-v2",
                "scene_snapshot_schema_version": "r3-scene-world-v1",
                "operations": ["scene_snapshot.read", "gameplay.apply_manifest"],
                "gameplay_primitives": ["unlock", "on_collect"],
            }
        )
        port = RuntimeEngineCapabilityPort(
            manifest_reader=make_engine_capability_manifest_reader(capability_tool=tool),
        )

        result = port.get_manifest()

        self.assertEqual(tool.calls, [{}])
        self.assertIsInstance(result, EngineCapabilityManifest)
        self.assertEqual(result.bridge_version, "native-bridge-v2")
        self.assertEqual(result.supported_operations, ("gameplay.apply_manifest", "scene_snapshot.read"))
        self.assertEqual(result.supported_gameplay_primitives, ("on_collect", "unlock"))

    def test_reader_connection_failure_becomes_specific_blocked_result(self) -> None:
        port = RuntimeEngineCapabilityPort(
            manifest_reader=make_engine_capability_manifest_reader(
                capability_tool=_CapabilityTool(ConnectionError("offline")),
            ),
        )

        result = port.get_manifest()

        self.assertEqual(result.status, "pending_runtime_verification")
        self.assertEqual(result.error_code, "bridge_not_connected")
        self.assertEqual(result.owner_domain, "engine")
        self.assertTrue(result.next_action)
        self.assertEqual(result.missing_requirements[0].requirement_id, "engine.capability_manifest")

    def test_incompatible_snapshot_version_fails_closed(self) -> None:
        port = RuntimeEngineCapabilityPort(
            manifest_reader=lambda: {
                "contract_version": "r3-engine-week1-v1",
                "bridge_version": "native-bridge-v2",
                "snapshot_schema_version": "r3-scene-world-v0",
                "supported_operations": [],
                "supported_gameplay_primitives": [],
            },
        )

        result = port.get_manifest()

        self.assertEqual(result.status, "pending_runtime_verification")
        self.assertEqual(result.error_code, "engine_snapshot_schema_version_incompatible")
        self.assertEqual(result.owner_domain, "engine")
        self.assertTrue(result.next_action)
        self.assertTrue(result.missing_requirements)

    def test_capability_fill_preserves_the_b4_runner_skeleton_contract_hash(self) -> None:
        self.assertEqual(
            build_skeleton_manifest().contract_hash(),
            "sha256:6144cabd279c57c8e843c585156e775f213d2575f1c61152100a426f5729e1cd",
        )


if __name__ == "__main__":
    unittest.main()

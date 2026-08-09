from __future__ import annotations

import json
from pathlib import Path
import sys
import unittest


REPO_ROOT = Path(__file__).resolve().parents[5]
EDITOR_ROOT = REPO_ROOT / "editor"
if str(EDITOR_ROOT) not in sys.path:
    sys.path.insert(0, str(EDITOR_ROOT))

from plugins.AITool.services.agent_runtime import AgentRuntimeFlags  # noqa: E402
from plugins.AITool.services.agent_runtime.core import SyncEventValidator  # noqa: E402
from plugins.AITool.services.lanchat_agent_worker import LANChatAgentWorker  # noqa: E402


class _FakeRuntime:
    def __init__(self) -> None:
        self.calls: list[dict] = []

    def handle_message(self, **kwargs):  # noqa: ANN001
        self.calls.append(dict(kwargs))
        return {
            "recorded": True,
            "sync_event": {
                "event_type": str(kwargs.get("sync_event", {}).get("event") or ""),
            },
            "sync_status": {"event_count": len(self.calls)},
        }


class _FakeNativeSyncEngine:
    def __init__(self, events: list[dict]) -> None:
        self.events = list(events)

    def network_pop_lanchat_sync_event(self):  # noqa: ANN201
        return self.events.pop(0) if self.events else None


class LanChatNativeSyncBridgeTests(unittest.TestCase):
    def test_worker_polls_native_sync_events_and_expands_actor_metadata(self) -> None:
        runtime = _FakeRuntime()
        engine = _FakeNativeSyncEngine([
            {
                "channel": "lanchat_sync",
                "event": "actor_create_received",
                "room_id": "room-sync",
                "payload_json": json.dumps({
                    "actor_id": "actor-cupid",
                    "asset_id": "asset-cupid",
                    "status": "received",
                    "actor_json": json.dumps({
                        "actor_guid": "actor-cupid",
                        "runtime_plan_id": "plan-room-sync",
                        "runtime_batch_id": "batch-cupid",
                        "entity_id": "entity-cupid",
                        "semantic_role": "statue",
                        "entity_version": 3,
                        "source_scene_version": 4,
                    }),
                }),
            },
        ])
        worker = LANChatAgentWorker(
            corona_engine=engine,
            agent_runtime=runtime,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        self.assertTrue(worker._process_sync_events(max_events=8))
        self.assertEqual(len(runtime.calls), 1)
        call = runtime.calls[0]
        self.assertEqual(call["action"], "runtime_sync_event")
        self.assertEqual(call["room_id"], "room-sync")
        self.assertEqual(call["sync_event"]["actor_id"], "actor-cupid")
        self.assertEqual(call["sync_event"]["plan_id"], "plan-room-sync")
        self.assertEqual(call["sync_event"]["batch_id"], "batch-cupid")
        self.assertEqual(call["sync_event"]["entity_id"], "entity-cupid")
        self.assertEqual(call["sync_event"]["actor_version"], 3)
        self.assertEqual(call["sync_event"]["scene_version"], 4)
        self.assertNotIn("actor_json", call["sync_event"])
        self.assertNotIn("payload_json", call["sync_event"])

    def test_native_sources_expose_and_produce_sync_queue_events(self) -> None:
        header = (REPO_ROOT / "include/corona/systems/network/network_system.h").read_text(encoding="utf-8")
        network = (REPO_ROOT / "src/systems/network/network_system.cpp").read_text(encoding="utf-8")
        bindings = (REPO_ROOT / "src/systems/script/python/editor_network_bindings.cpp").read_text(encoding="utf-8")

        self.assertIn("struct LanChatSyncEvent", header)
        self.assertIn("lanchat_pop_sync_event", header)
        self.assertIn('"actor_create_received"', network)
        self.assertIn('"actor_imported"', network)
        self.assertIn('"actor_transform"', network)
        self.assertIn('"actor_deleted"', network)
        self.assertIn('"asset_transfer_completed"', network)
        self.assertIn('"scene_snapshot_received"', network)
        self.assertIn('"scene_snapshot_peer_ack"', network)
        self.assertIn('snapshot_kind == "peer_ack"', network)
        self.assertIn('"host_identity_fingerprint"', network)
        self.assertIn('"peer_identity_fingerprint"', network)
        self.assertIn('snapshot.value("plan_id"', network)
        self.assertIn('is_message_from_connected_host(sender_peer_id)', network)
        self.assertIn('? "remote_host"', network)
        self.assertIn(': "remote_peer"', network)
        self.assertIn("is_coalescible_sync_event", network)
        self.assertIn("kSoftMaxPendingLanChatSyncEvents", network)
        self.assertIn("kHardMaxPendingLanChatSyncEvents", network)
        self.assertIn("actor_id_from_payload", network)
        self.assertNotIn("kMaxPendingLanChatSyncEvents = 256", network)
        self.assertIn('m.def("network_pop_lanchat_sync_event"', bindings)

    def test_peer_snapshot_ack_fields_survive_safe_sync_storage(self) -> None:
        stored = SyncEventValidator.safe_storage_event({
            "room_id": "room-sync",
            "event_type": "scene_snapshot_peer_ack",
            "plan_id": "plan-sync",
            "peer_id": "peer-1",
            "authority": "remote_peer",
            "snapshot_kind": "peer_ack",
            "scene_version": 7,
            "host_identity_fingerprint": "scene-id-v1-host",
            "peer_identity_fingerprint": "scene-id-v1-peer",
            "entity_count": 8,
            "applied_entity_count": 7,
            "partial_entity_count": 1,
            "identity_drift_count": 0,
            "version_drift_count": 1,
            "missing_fields_explicit": True,
        })

        self.assertEqual(stored["authority"], "remote_peer")
        self.assertEqual(stored["snapshot_kind"], "peer_ack")
        self.assertEqual(stored["scene_version"], 7)
        self.assertIsInstance(stored["scene_version"], int)
        self.assertEqual(stored["entity_count"], 8)
        self.assertEqual(stored["applied_entity_count"], 7)
        self.assertEqual(stored["version_drift_count"], 1)
        self.assertTrue(stored["missing_fields_explicit"])

    def test_lanchat_room_panel_emits_identity_fingerprinted_peer_ack(self) -> None:
        panel = (
            REPO_ROOT / "editor/Frontend/src/views/sidebar/lanchat/RoomPanel.vue"
        ).read_text(encoding="utf-8")

        self.assertIn("function snapshotIdentityRows", panel)
        self.assertIn("function snapshotIdentityFingerprint", panel)
        self.assertIn("snapshot_kind: 'host_snapshot'", panel)
        self.assertIn("identity_fingerprint: snapshotIdentityFingerprint(identityRows)", panel)
        self.assertIn("function buildPeerSnapshotAck", panel)
        self.assertIn("snapshot_kind: 'peer_ack'", panel)
        self.assertIn("function refreshPeerSnapshotAcks", panel)
        self.assertIn("await refreshPeerSnapshotAcks()", panel)
        self.assertIn(
            "await broadcastCurrentSceneSnapshot(currentModelTransferSceneName(), false, false);",
            panel,
        )

    def test_received_actor_is_not_engine_imported_until_identity_event(self) -> None:
        engine = _FakeNativeSyncEngine([
            {
                "event": "actor_create_received",
                "room_id": "room-lifecycle",
                "payload_json": json.dumps({
                    "actor_id": "actor-remote",
                    "asset_id": "asset-remote",
                    "status": "received",
                }),
            },
        ])
        worker = LANChatAgentWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        self.assertTrue(worker._process_sync_events(max_events=1))
        received = worker._agent_runtime.query_state("room-lifecycle")["room"]["actors"]["actor-remote"]
        self.assertEqual(received["sync_lifecycle_status"], "received")
        self.assertNotEqual(received.get("engine_lifecycle_status"), "engine_imported")

        engine.events.append({
            "event": "actor_imported",
            "room_id": "room-lifecycle",
            "payload_json": json.dumps({
                "actor_id": "actor-remote",
                "status": "engine_imported",
            }),
        })
        self.assertTrue(worker._process_sync_events(max_events=1))
        imported = worker._agent_runtime.query_state("room-lifecycle")["room"]["actors"]["actor-remote"]
        self.assertEqual(imported["sync_lifecycle_status"], "active")
        self.assertEqual(imported["engine_lifecycle_status"], "engine_imported")
        self.assertFalse(bool(imported.get("bounds_ready")))


if __name__ == "__main__":
    unittest.main()

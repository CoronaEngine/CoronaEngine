from pathlib import Path
import unittest

REPO_ROOT = next(path for path in Path(__file__).resolve().parents
                 if (path / "src/systems/network/network_system.cpp").is_file())

class NativeNetworkSyncContractTests(unittest.TestCase):
    def test_object_sync_does_not_depend_on_chat_event_queues(self):
        source = (REPO_ROOT / "src/systems/network/network_system.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "include/corona/systems/network/network_system.h").read_text(encoding="utf-8")
        self.assertNotIn("lanchat", source.lower())
        self.assertNotIn("LanChat", header)
        for operation in ("register_actor_identity", "process_pending_actor_create",
                          "process_pending_actor_transform_update", "claim_actor_ownership"):
            self.assertIn(operation, source)
        self.assertIn("Network::classify_scene_snapshot(snapshot_kind)", source)
        self.assertIn("Network::SceneSnapshotDisposition::PeerAcknowledgement", source)
        self.assertIn("stop_session_after_peer_disconnect = true", source)

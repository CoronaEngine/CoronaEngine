import sys
from pathlib import Path
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[4]
EDITOR_ROOT = PROJECT_ROOT / "editor"
AI_TOOL_ROOT = EDITOR_ROOT / "plugins" / "AITool"
for path in (PROJECT_ROOT, EDITOR_ROOT, AI_TOOL_ROOT):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

from cai_extensions.agent import collaboration


class FakeNetworkApi:
    def __init__(self):
        self.calls = []

    def lock_object(self, object_id, user_id, operation):
        self.calls.append(("lock", object_id, user_id, operation))
        return {"ok": True}

    def unlock_object(self, object_id, user_id):
        self.calls.append(("unlock", object_id, user_id))
        return {"ok": True}

    def get_lock_owner(self, object_id):
        self.calls.append(("owner", object_id))
        return {"owner": "alice"}

    def broadcast_intent(self, user_id, tooltip, position, status):
        self.calls.append(("intent", user_id, tooltip, position, status))
        return {"ok": True}

    def check_preview_collision(self, user_id, position, delta):
        self.calls.append(("collision", user_id, position, delta))
        return {"conflict_user_id": "bob"}


class CollaborationApiBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.api = FakeNetworkApi()
        collaboration.EDITOR_API_OVERRIDE = type("EditorApi", (), {"network": self.api})

    def tearDown(self):
        collaboration.EDITOR_API_OVERRIDE = None

    def test_collaboration_operations_use_network_aggregate_api(self):
        manager = collaboration.CollaborationManager()

        self.assertTrue(manager.lock_object("cube", "alice", "move"))
        self.assertTrue(manager.unlock_object("cube", "alice"))
        self.assertEqual(manager.is_locked("cube"), "alice")
        manager.broadcast_intent("alice", "moving", [1, 2], "placing")
        self.assertEqual(
            manager.check_preview_collision("alice", [1, 2], exclude_user=False),
            "bob",
        )
        self.assertEqual(
            [call[0] for call in self.api.calls],
            ["lock", "unlock", "owner", "intent", "collision"],
        )


if __name__ == "__main__":
    unittest.main()

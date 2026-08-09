from __future__ import annotations

import unittest
from unittest import mock

from runtime import network_sync_policy


class _Scene:
    route = "Scene/test.scene"

    def __init__(self) -> None:
        self._actors: list[object] = []

    def get_actors(self) -> list[object]:
        return list(self._actors)


class _Actor:
    def __init__(self, scene: _Scene, *, actor_version: int = 1) -> None:
        self.parent = scene
        self.name = "chair"
        self.actor_type = "model"
        self.actor_guid = "runtime-actor-chair"
        self.actor_version = actor_version
        self.route = "Models/chair.obj"
        self.model_path = "Models/chair.obj"
        self._suppress_network_broadcast = False
        self._geometry = object()
        scene._actors.append(self)

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "actor_type": self.actor_type,
            "actor_guid": self.actor_guid,
            "version": self.actor_version,
            "path": self.route,
            "model": self.model_path,
            "scene": self.parent.route,
            "geometry": {"position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1]},
        }


class NetworkSyncPolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        network_sync_policy.reset_for_tests()

    def tearDown(self) -> None:
        network_sync_policy.reset_for_tests()

    def test_actor_create_is_deduped_across_transactions_by_guid(self) -> None:
        scene = _Scene()
        actor = _Actor(scene)
        emitted: list[dict] = []

        network_sync_policy.publish_actor_created(actor, prepare=None, emit=emitted.append)
        with network_sync_policy.deferred_actor_broadcasts(transaction_key="batch-2"):
            network_sync_policy.publish_actor_created(actor, prepare=None, emit=emitted.append)

        self.assertEqual(len(emitted), 1)

    def test_new_actor_version_does_not_republish_actor_create(self) -> None:
        scene = _Scene()
        actor = _Actor(scene, actor_version=1)
        emitted: list[dict] = []

        network_sync_policy.publish_actor_created(actor, prepare=None, emit=emitted.append)
        actor.actor_version = 2
        network_sync_policy.publish_actor_created(actor, prepare=None, emit=emitted.append)

        self.assertEqual([item["version"] for item in emitted], [1])

    def test_sync_pause_notification_uses_compatibility_adapter(self) -> None:
        with mock.patch(
            "CoronaCore.core.editor_api.emit_compat_editor_event"
        ) as emit:
            network_sync_policy.set_engine_sync_paused(True)
            network_sync_policy.set_engine_sync_paused(False)

        self.assertEqual(
            emit.call_args_list,
            [
                mock.call("network-sync-pause-request", [{"paused": True}]),
                mock.call("network-sync-pause-request", [{"paused": False}]),
            ],
        )


if __name__ == "__main__":
    unittest.main()

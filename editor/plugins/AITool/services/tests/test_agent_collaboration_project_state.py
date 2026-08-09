from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import unittest

from editor.plugins.AITool.services.agent_collaboration import (
    InvalidProjectStateTransitionError,
    ProjectAlreadyExistsError,
    ProjectPatchConflictError,
    ProjectStatePatch,
    ProjectStateStore,
    ProjectVersionConflictError,
)


def _patch(
    *,
    patch_id: str,
    expected_version: int,
    changes: dict,
    project_id: str = "project-1",
    source: str = "unit-test",
) -> ProjectStatePatch:
    return ProjectStatePatch(
        patch_id=patch_id,
        project_id=project_id,
        expected_project_version=expected_version,
        source=source,
        changes=changes,
    )


class ProjectStateStoreTests(unittest.TestCase):
    def setUp(self) -> None:
        self.store = ProjectStateStore()
        self.store.create_project(
            project_id="project-1",
            room_id="room-1",
            source="project.create",
        )

    def test_create_and_update_preserve_explicit_version_source(self) -> None:
        updated = self.store.apply_patch(_patch(
            patch_id="patch-bind-scene",
            expected_version=1,
            source="planning-agent:task-1",
            changes={
                "active_scene_plan_id": "scene-plan-1",
                "scene_world_version": 3,
                "validation_status": "valid",
            },
        ))

        self.assertEqual(updated.project_version, 2)
        self.assertEqual(updated.active_scene_plan_id, "scene-plan-1")
        self.assertEqual(updated.scene_world_version, 3)
        history = self.store.history("project-1")
        self.assertEqual(len(history), 2)
        self.assertEqual(history[-1].source, "planning-agent:task-1")
        self.assertEqual(history[-1].from_version, 1)
        self.assertEqual(history[-1].to_version, 2)
        self.assertEqual(
            history[-1].changed_fields,
            ("active_scene_plan_id", "scene_world_version", "validation_status"),
        )

    def test_stale_expected_version_fails_without_mutation(self) -> None:
        self.store.apply_patch(_patch(
            patch_id="patch-first",
            expected_version=1,
            changes={"active_task_graph_id": "task-graph-1"},
        ))
        before = self.store.get("project-1")
        before_history = self.store.history("project-1")

        with self.assertRaises(ProjectVersionConflictError):
            self.store.apply_patch(_patch(
                patch_id="patch-stale",
                expected_version=1,
                changes={"active_task_graph_id": "task-graph-stale"},
            ))

        self.assertEqual(self.store.get("project-1"), before)
        self.assertEqual(self.store.history("project-1"), before_history)

    def test_patch_retry_is_idempotent_and_conflicting_reuse_is_rejected(self) -> None:
        patch = _patch(
            patch_id="patch-idempotent",
            expected_version=1,
            changes={"artifact_refs": ["artifact-b", "artifact-a", "artifact-a"]},
        )
        first = self.store.apply_patch(patch)
        second = self.store.apply_patch(patch)

        self.assertIs(first, second)
        self.assertEqual(first.artifact_refs, ("artifact-a", "artifact-b"))
        self.assertEqual(len(self.store.history("project-1")), 2)
        with self.assertRaises(ProjectPatchConflictError):
            self.store.apply_patch(_patch(
                patch_id="patch-idempotent",
                expected_version=1,
                changes={"artifact_refs": ["artifact-c"]},
            ))

    def test_world_version_is_monotonic(self) -> None:
        self.store.apply_patch(_patch(
            patch_id="patch-world-v4",
            expected_version=1,
            changes={"scene_world_version": 4},
        ))

        with self.assertRaises(InvalidProjectStateTransitionError):
            self.store.apply_patch(_patch(
                patch_id="patch-world-v3",
                expected_version=2,
                changes={"scene_world_version": 3},
            ))
        self.assertEqual(self.store.get("project-1").scene_world_version, 4)

    def test_noop_patch_does_not_invent_a_new_project_version(self) -> None:
        current = self.store.get("project-1")
        result = self.store.apply_patch(_patch(
            patch_id="patch-noop",
            expected_version=1,
            changes={"validation_status": "pending"},
        ))

        self.assertIs(result, current)
        self.assertEqual(result.project_version, 1)
        self.assertEqual(len(self.store.history("project-1")), 1)

    def test_concurrent_compare_and_swap_allows_exactly_one_writer(self) -> None:
        patches = (
            _patch(
                patch_id="patch-concurrent-a",
                expected_version=1,
                changes={"active_task_graph_id": "graph-a"},
            ),
            _patch(
                patch_id="patch-concurrent-b",
                expected_version=1,
                changes={"active_task_graph_id": "graph-b"},
            ),
        )

        def apply(patch: ProjectStatePatch) -> str:
            try:
                return self.store.apply_patch(patch).active_task_graph_id
            except ProjectVersionConflictError:
                return "version_conflict"

        with ThreadPoolExecutor(max_workers=2) as executor:
            results = list(executor.map(apply, patches))

        self.assertEqual(results.count("version_conflict"), 1)
        self.assertIn(self.store.get("project-1").active_task_graph_id, {"graph-a", "graph-b"})
        self.assertEqual(self.store.get("project-1").project_version, 2)

    def test_projects_are_isolated_and_immutable_fields_cannot_be_patched(self) -> None:
        self.store.create_project(
            project_id="project-2",
            room_id="room-2",
            source="project.create",
        )
        self.store.apply_patch(_patch(
            patch_id="patch-project-2",
            project_id="project-2",
            expected_version=1,
            changes={"active_task_graph_id": "graph-2"},
        ))

        self.assertEqual(self.store.get("project-1").active_task_graph_id, "")
        self.assertEqual(self.store.get("project-2").active_task_graph_id, "graph-2")
        with self.assertRaises(ValueError):
            _patch(
                patch_id="patch-illegal",
                expected_version=1,
                changes={"room_id": "other-room"},
            )
        with self.assertRaises(ProjectAlreadyExistsError):
            self.store.create_project(
                project_id="project-1",
                room_id="room-1",
                source="duplicate",
            )


if __name__ == "__main__":
    unittest.main()

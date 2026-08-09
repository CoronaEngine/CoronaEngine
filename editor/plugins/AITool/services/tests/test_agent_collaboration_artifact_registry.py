from __future__ import annotations

import unittest

import editor.plugins.AITool.services.agent_collaboration.artifact_registry as artifact_registry_module
from editor.plugins.AITool.services.agent_collaboration import (
    ArtDirection,
    ArtifactDependencyError,
    ArtifactEnvelope,
    ArtifactNotUsableError,
    ArtifactRegistrationConflictError,
    ArtifactRegistry,
    ArtifactVersionConflictError,
    EntityBindingPlan,
    GameDesignBrief,
    GameplayEntitySlot,
    GameplayLogicPlan,
    GameplayPrimitiveSpec,
    InvalidArtifactError,
    ProjectStatePatch,
    ProjectStateStore,
)
from editor.plugins.AITool.services.tests.support._test_import_guard import assert_module_has_no_forbidden_imports


def _payload(artifact_type: str, label: str):
    if artifact_type == "GameDesignBrief":
        return GameDesignBrief(
            project_goal=f"goal {label}",
            player_experience=("explore",),
            core_rules=("host authoritative",),
            acceptance_criteria=("shared world",),
        )
    if artifact_type == "ArtDirection":
        return ArtDirection(
            style_keywords=(label, "coherent"),
            palette=("amber",),
            lighting=("warm",),
            avoid_keywords=("horror",),
        )
    if artifact_type == "GameplayLogicPlan":
        return GameplayLogicPlan(
            states=("ready", label),
            entity_slots=(
                GameplayEntitySlot("item", "collectible_item", ("collectible",)),
                GameplayEntitySlot("player", "player_spawn", ("player",)),
            ),
            primitives=(
                GameplayPrimitiveSpec("collect-item", "on_collect", "item", "player", {"state_key": "ready", "set_value": True}),
            ),
            triggers=("enter",),
            rules=("authoritative state",),
            win_conditions=("goal reached",),
            lose_conditions=("timeout",),
        )
    raise AssertionError(artifact_type)


def _artifact(
    *,
    artifact_id: str,
    artifact_type: str,
    role: str,
    version: int,
    base_project_version: int,
    dependencies: tuple[str, ...] = (),
    label: str = "v1",
    status: str = "validated",
    base_world_version: int = 0,
    snapshot_source: str = "none",
) -> ArtifactEnvelope:
    return ArtifactEnvelope(
        artifact_id=artifact_id,
        artifact_type=artifact_type,
        version=version,
        producer_role=role,
        source_task_id=f"task-{artifact_id}-{version}",
        base_project_version=base_project_version,
        base_world_version=base_world_version,
        dependencies=dependencies,
        snapshot_source=snapshot_source,
        non_executable=True,
        status=status,
        payload=_payload(artifact_type, label),
    )


class ArtifactRegistryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.projects = ProjectStateStore()
        self.projects.create_project(project_id="project-1", room_id="room-1", source="test")
        self.registry = ArtifactRegistry(self.projects)

    def test_atomic_batch_registration_updates_project_state_once(self) -> None:
        brief = _artifact(
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            version=1,
            base_project_version=1,
        )
        art = _artifact(
            artifact_id="art",
            artifact_type="ArtDirection",
            role="art",
            version=1,
            base_project_version=1,
            dependencies=("brief@1",),
        )

        records = self.registry.register_many(
            project_id="project-1",
            artifacts=(art, brief),
            expected_project_version=1,
            patch_id="patch-register-initial",
            source="test",
        )

        self.assertEqual([str(record.ref) for record in records], ["art@1", "brief@1"])
        self.assertEqual(self.projects.get("project-1").project_version, 2)
        self.assertEqual(self.projects.get("project-1").artifact_refs, ("art@1", "brief@1"))
        self.assertEqual(self.registry.dependents("project-1", "brief@1"), (records[0].ref,))
        self.assertTrue(self.registry.current("project-1", "art").usable)

    def test_runtime_artifact_must_match_current_project_world_version(self) -> None:
        self.projects.apply_patch(ProjectStatePatch(
            patch_id="patch-world-v2",
            project_id="project-1",
            expected_project_version=1,
            source="test",
            changes={"scene_world_version": 2},
        ))
        stale = _artifact(
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            version=1,
            base_project_version=2,
            base_world_version=1,
            snapshot_source="runtime",
        )

        with self.assertRaisesRegex(ArtifactVersionConflictError, "base_world_version 1"):
            self.registry.register(
                project_id="project-1",
                artifact=stale,
                expected_project_version=2,
                patch_id="patch-stale-world-artifact",
                source="test",
            )

        current = _artifact(
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            version=1,
            base_project_version=2,
            base_world_version=2,
            snapshot_source="runtime",
        )
        record = self.registry.register(
            project_id="project-1",
            artifact=current,
            expected_project_version=2,
            patch_id="patch-current-world-artifact",
            source="test",
        )

        self.assertEqual(record.artifact.base_world_version, 2)
        self.assertEqual(self.projects.get("project-1").artifact_refs, ("brief@1",))

    def test_runtime_artifact_becomes_unusable_after_world_version_advances(self) -> None:
        self.projects.apply_patch(ProjectStatePatch(
            patch_id="patch-world-v1",
            project_id="project-1",
            expected_project_version=1,
            source="test",
            changes={"scene_world_version": 1},
        ))
        artifact = _artifact(
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            version=1,
            base_project_version=2,
            base_world_version=1,
            snapshot_source="runtime",
        )
        self.registry.register(
            project_id="project-1",
            artifact=artifact,
            expected_project_version=2,
            patch_id="patch-register-world-v1",
            source="test",
        )
        self.projects.apply_patch(ProjectStatePatch(
            patch_id="patch-world-v2-after-register",
            project_id="project-1",
            expected_project_version=3,
            source="test",
            changes={"scene_world_version": 2},
        ))

        with self.assertRaisesRegex(ArtifactNotUsableError, "stale world version"):
            self.registry.current("project-1", "brief")
        self.assertEqual(self.registry.list_current("project-1", include_stale=False), ())
        self.assertEqual(
            self.registry.current("project-1", "brief", require_usable=False).artifact.base_world_version,
            1,
        )

    def test_new_upstream_version_marks_direct_and_transitive_dependents_stale(self) -> None:
        brief = _artifact(
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            version=1,
            base_project_version=1,
        )
        art = _artifact(
            artifact_id="art",
            artifact_type="ArtDirection",
            role="art",
            version=1,
            base_project_version=1,
            dependencies=("brief@1",),
        )
        self.registry.register_many(
            project_id="project-1",
            artifacts=(brief, art),
            expected_project_version=1,
            patch_id="patch-v1",
            source="test",
        )
        logic = _artifact(
            artifact_id="logic",
            artifact_type="GameplayLogicPlan",
            role="program",
            version=1,
            base_project_version=2,
            dependencies=("art@1",),
        )
        self.registry.register(
            project_id="project-1",
            artifact=logic,
            expected_project_version=2,
            patch_id="patch-logic-v1",
            source="test",
        )

        brief_v2 = _artifact(
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            version=2,
            base_project_version=3,
            label="v2",
        )
        self.registry.register(
            project_id="project-1",
            artifact=brief_v2,
            expected_project_version=3,
            patch_id="patch-brief-v2",
            source="test",
        )

        self.assertEqual(self.registry.get("project-1", "brief@1").registry_status, "superseded")
        art_record = self.registry.current("project-1", "art", require_usable=False)
        logic_record = self.registry.current("project-1", "logic", require_usable=False)
        self.assertEqual(art_record.registry_status, "stale")
        self.assertEqual(art_record.stale_reasons[0].code, "dependency_superseded")
        self.assertEqual(logic_record.registry_status, "stale")
        self.assertIn("dependency_stale", {reason.code for reason in logic_record.stale_reasons})
        transitive_reason = next(
            reason for reason in logic_record.stale_reasons if reason.code == "dependency_stale"
        )
        self.assertIsNone(transitive_reason.replacement_ref)
        with self.assertRaises(ArtifactNotUsableError):
            self.registry.current("project-1", "logic")
        self.assertFalse(self.registry.get("project-1", "brief@1").usable)
        self.assertEqual(self.projects.get("project-1").validation_status, "stale")
        self.assertEqual(
            self.projects.get("project-1").artifact_refs,
            ("art@1", "brief@2", "logic@1"),
        )

    def test_gameplay_logic_revision_stales_entity_binding_plan(self) -> None:
        logic_v1 = _artifact(
            artifact_id="logic",
            artifact_type="GameplayLogicPlan",
            role="program",
            version=1,
            base_project_version=1,
        )
        self.registry.register(
            project_id="project-1",
            artifact=logic_v1,
            expected_project_version=1,
            patch_id="patch-logic-v1",
            source="test",
        )
        binding = ArtifactEnvelope(
            artifact_id="binding",
            artifact_type="EntityBindingPlan",
            version=1,
            producer_role="program",
            source_task_id="task-binding-1",
            base_project_version=2,
            base_world_version=1,
            dependencies=("logic@1",),
            snapshot_source="mock",
            non_executable=True,
            status="validated",
            payload=EntityBindingPlan(
                snapshot_plan_id="plan-fixture",
                snapshot_version=1,
                bindings=({
                    "slot_id": "key",
                    "entity_id": "entity-key",
                    "entity_version": 1,
                    "asset_id": "asset-key",
                    "semantic_role": "collectible_key",
                    "required_capabilities": ["collectible"],
                },),
            ),
        )
        self.registry.register(
            project_id="project-1",
            artifact=binding,
            expected_project_version=2,
            patch_id="patch-binding-v1",
            source="test",
        )
        logic_v2 = _artifact(
            artifact_id="logic",
            artifact_type="GameplayLogicPlan",
            role="program",
            version=2,
            base_project_version=3,
            label="v2",
        )

        self.registry.register(
            project_id="project-1",
            artifact=logic_v2,
            expected_project_version=3,
            patch_id="patch-logic-v2",
            source="test",
        )

        binding_record = self.registry.current("project-1", "binding", require_usable=False)
        self.assertEqual(binding_record.registry_status, "stale")
        self.assertEqual(binding_record.stale_reasons[0].code, "dependency_superseded")
        self.assertEqual(binding_record.stale_reasons[0].dependency_ref.artifact_id, "logic")
        self.assertEqual(binding_record.stale_reasons[0].replacement_ref.version, 2)
        with self.assertRaises(ArtifactNotUsableError):
            self.registry.current("project-1", "binding")

    def test_republishing_dependents_clears_current_stale_state_but_preserves_audit(self) -> None:
        self._build_stale_chain()
        art_v2 = _artifact(
            artifact_id="art",
            artifact_type="ArtDirection",
            role="art",
            version=2,
            base_project_version=4,
            dependencies=("brief@2",),
            label="v2",
        )
        logic_v2 = _artifact(
            artifact_id="logic",
            artifact_type="GameplayLogicPlan",
            role="program",
            version=2,
            base_project_version=4,
            dependencies=("art@2",),
            label="v2",
        )

        self.registry.register_many(
            project_id="project-1",
            artifacts=(logic_v2, art_v2),
            expected_project_version=4,
            patch_id="patch-downstream-v2",
            source="test",
        )

        self.assertTrue(self.registry.current("project-1", "art").usable)
        self.assertTrue(self.registry.current("project-1", "logic").usable)
        self.assertEqual(self.registry.get("project-1", "art@1").registry_status, "superseded")
        self.assertTrue(self.registry.get("project-1", "art@1").stale_reasons)
        self.assertEqual(self.projects.get("project-1").validation_status, "pending")

    def test_same_registration_is_idempotent_after_project_version_advances(self) -> None:
        brief = _artifact(
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            version=1,
            base_project_version=1,
        )
        first = self.registry.register(
            project_id="project-1",
            artifact=brief,
            expected_project_version=1,
            patch_id="patch-idempotent",
            source="test",
        )
        replay = self.registry.register(
            project_id="project-1",
            artifact=brief,
            expected_project_version=1,
            patch_id="patch-idempotent",
            source="test",
        )

        self.assertIs(first, replay)
        self.assertEqual(self.projects.get("project-1").project_version, 2)
        self.assertEqual(len(self.projects.history("project-1")), 2)

    def test_same_ref_with_changed_content_is_rejected(self) -> None:
        first = _artifact(
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            version=1,
            base_project_version=1,
        )
        changed = _artifact(
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            version=1,
            base_project_version=1,
            label="changed",
        )
        self.registry.register(
            project_id="project-1",
            artifact=first,
            expected_project_version=1,
            patch_id="patch-first",
            source="test",
        )

        with self.assertRaises(ArtifactRegistrationConflictError):
            self.registry.register(
                project_id="project-1",
                artifact=changed,
                expected_project_version=1,
                patch_id="patch-changed",
                source="test",
            )

    def test_version_and_dependency_guards_leave_both_stores_unchanged(self) -> None:
        missing_dependency = _artifact(
            artifact_id="art",
            artifact_type="ArtDirection",
            role="art",
            version=1,
            base_project_version=1,
            dependencies=("brief@1",),
        )
        with self.assertRaises(ArtifactDependencyError):
            self.registry.register(
                project_id="project-1",
                artifact=missing_dependency,
                expected_project_version=1,
                patch_id="patch-missing",
                source="test",
            )
        with self.assertRaises(ArtifactVersionConflictError):
            self.registry.register(
                project_id="project-1",
                artifact=_artifact(
                    artifact_id="brief",
                    artifact_type="GameDesignBrief",
                    role="planning",
                    version=2,
                    base_project_version=1,
                ),
                expected_project_version=1,
                patch_id="patch-skipped-version",
                source="test",
            )

        self.assertEqual(self.projects.get("project-1").project_version, 1)
        self.assertEqual(self.registry.list_current("project-1"), ())

    def test_invalid_artifact_is_rejected_without_project_update(self) -> None:
        invalid = ArtifactEnvelope(
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            version=1,
            producer_role="planning",
            source_task_id="task-invalid",
            base_project_version=1,
            base_world_version=0,
            status="validated",
            payload={"project_goal": "missing required lists"},
        )
        with self.assertRaises(InvalidArtifactError):
            self.registry.register(
                project_id="project-1",
                artifact=invalid,
                expected_project_version=1,
                patch_id="patch-invalid",
                source="test",
            )
        self.assertEqual(self.projects.get("project-1").project_version, 1)

    def test_red_track_module_does_not_import_runtime_or_lanchat(self) -> None:
        forbidden = (
            "editor.plugins.AITool.services.agent_runtime",
            "editor.plugins.AITool.services.lanchat",
        )
        assert_module_has_no_forbidden_imports(self, artifact_registry_module, forbidden)

    def _build_stale_chain(self) -> None:
        brief = _artifact(
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            version=1,
            base_project_version=1,
        )
        art = _artifact(
            artifact_id="art",
            artifact_type="ArtDirection",
            role="art",
            version=1,
            base_project_version=1,
            dependencies=("brief@1",),
        )
        self.registry.register_many(
            project_id="project-1",
            artifacts=(brief, art),
            expected_project_version=1,
            patch_id="patch-chain-v1",
            source="test",
        )
        logic = _artifact(
            artifact_id="logic",
            artifact_type="GameplayLogicPlan",
            role="program",
            version=1,
            base_project_version=2,
            dependencies=("art@1",),
        )
        self.registry.register(
            project_id="project-1",
            artifact=logic,
            expected_project_version=2,
            patch_id="patch-chain-logic",
            source="test",
        )
        brief_v2 = _artifact(
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            version=2,
            base_project_version=3,
            label="v2",
        )
        self.registry.register(
            project_id="project-1",
            artifact=brief_v2,
            expected_project_version=3,
            patch_id="patch-chain-brief-v2",
            source="test",
        )


if __name__ == "__main__":
    unittest.main()

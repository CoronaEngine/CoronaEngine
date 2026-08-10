"""Deterministic Engine lifecycle fixture. Never wire this into production."""

from __future__ import annotations

from dataclasses import dataclass, replace
from typing import Literal

from ...agent_collaboration.walking_skeleton import EngineCapabilityManifest
from ...integration_contracts import BlockedResult, MissingRequirement
from ...schema_versions import (
    ENGINE_ADAPTER_CONTRACT_VERSION,
    SCENE_WORLD_SNAPSHOT_SCHEMA_VERSION,
)


LifecycleScenario = Literal["normal", "late_ready", "partial", "failure", "capability_missing"]


@dataclass(frozen=True)
class MockActorHandle:
    request_id: str
    actor_id: str
    entity_id: str
    lifecycle_status: str
    geometry_ready: bool
    actual_aabb: dict[str, tuple[float, float, float]] | None
    grounding_status: str
    support_status: str
    render_ready: bool
    sync_status: str


@dataclass(frozen=True)
class MockSceneSnapshot:
    room_id: str
    scene_version: int
    actors: tuple[MockActorHandle, ...]
    snapshot_source: Literal["mock"] = "mock"
    snapshot_schema_version: str = SCENE_WORLD_SNAPSHOT_SCHEMA_VERSION

    def __post_init__(self) -> None:
        if self.snapshot_source != "mock":
            raise ValueError("EngineTestDouble snapshots must use snapshot_source=mock")

    def as_dict(self) -> dict[str, object]:
        return {
            "room_id": self.room_id,
            "scene_version": self.scene_version,
            "snapshot_source": self.snapshot_source,
            "snapshot_schema_version": self.snapshot_schema_version,
            "actors": tuple(self.actors),
        }


class EngineTestDouble:
    """A controllable, read/write-free model of Engine materialization facts.

    This fixture exposes the same capability result union as
    ``RuntimeEngineCapabilityPort``. Lifecycle methods are deliberately local
    test helpers: they do not resemble a production provider or Engine API.
    """

    _DEFAULT_PRIMITIVES = ("complete_objective", "on_collect", "on_enter", "set_state", "unlock")

    def __init__(
        self,
        *,
        room_id: str = "room.engine-test-double",
        scenario: LifecycleScenario = "normal",
        late_ready_cycles: int = 0,
        failure_mode: Literal["create_rejected", "timeout"] = "create_rejected",
        supported_primitives: tuple[str, ...] | None = None,
    ) -> None:
        if scenario not in {"normal", "late_ready", "partial", "failure", "capability_missing"}:
            raise ValueError("unsupported EngineTestDouble scenario")
        if late_ready_cycles < 0:
            raise ValueError("late_ready_cycles cannot be negative")
        self._room_id = str(room_id)
        self._scenario = scenario
        self._late_ready_remaining = int(late_ready_cycles)
        self._failure_mode = failure_mode
        if supported_primitives is None:
            supported_primitives = (
                ("on_enter",) if scenario == "capability_missing" else self._DEFAULT_PRIMITIVES
            )
        self._supported_primitives = tuple(sorted({str(item) for item in supported_primitives if str(item)}))
        self._actors_by_request: dict[str, MockActorHandle] = {}
        self._scene_version = 0

    def get_manifest(self) -> EngineCapabilityManifest | BlockedResult:
        return EngineCapabilityManifest(
            contract_version=ENGINE_ADAPTER_CONTRACT_VERSION,
            bridge_version="engine-test-double-v1",
            snapshot_schema_version=SCENE_WORLD_SNAPSHOT_SCHEMA_VERSION,
            supported_operations=("actor_create", "scene_snapshot_read"),
            supported_gameplay_primitives=self._supported_primitives,
        )

    def create_actor(
        self,
        *,
        request_id: str,
        entity_id: str,
        primitive: str = "on_enter",
    ) -> MockActorHandle | BlockedResult:
        if request_id in self._actors_by_request:
            return self._actors_by_request[request_id]
        if primitive not in self._supported_primitives:
            return self._blocked(
                "engine_capability_primitive_missing",
                "engine.capability_primitive",
                "Requested gameplay primitive is absent from the Engine capability manifest.",
                "Select a supported primitive or wait for an Engine capability update.",
            )
        if self._scenario == "failure":
            error_code = "engine_create_timeout" if self._failure_mode == "timeout" else "engine_create_rejected"
            return self._blocked(
                error_code,
                "engine.actor_create",
                "Engine Test Double rejected actor creation.",
                "Retry only after resolving the recorded Engine creation failure.",
            )
        handle = MockActorHandle(
            request_id=request_id,
            actor_id=f"mock_actor_{entity_id}",
            entity_id=entity_id,
            lifecycle_status="create_accepted",
            geometry_ready=False,
            actual_aabb=None,
            grounding_status="needs_review",
            support_status="unknown",
            render_ready=False,
            sync_status="pending",
        )
        self._actors_by_request[request_id] = handle
        return handle

    def advance(self, request_id: str) -> MockActorHandle | BlockedResult:
        actor = self._actors_by_request.get(request_id)
        if actor is None:
            return self._blocked(
                "engine_actor_request_not_found",
                "engine.actor_request",
                "No accepted actor request exists for this lifecycle advance.",
                "Create the actor with a stable request_id before advancing it.",
            )
        if self._scenario == "late_ready" and self._late_ready_remaining > 0:
            self._late_ready_remaining -= 1
            updated = replace(actor, lifecycle_status="waiting_for_geometry")
            self._actors_by_request[request_id] = updated
            return updated
        if not actor.geometry_ready:
            updated = replace(actor, lifecycle_status="geometry_ready", geometry_ready=True)
        elif actor.actual_aabb is None and self._scenario != "partial":
            updated = replace(
                actor,
                lifecycle_status="grounded",
                actual_aabb={"min": (-0.5, 0.0, -0.5), "max": (0.5, 1.0, 0.5)},
                grounding_status="grounded",
                support_status="floor_supported",
                sync_status="ready",
            )
        elif not actor.render_ready:
            updated = replace(
                actor,
                lifecycle_status="render_ready" if self._scenario != "partial" else "partial",
                render_ready=True,
                sync_status="partial" if self._scenario == "partial" else actor.sync_status,
            )
            self._scene_version += 1
        else:
            updated = actor
        self._actors_by_request[request_id] = updated
        return updated

    def get_snapshot(self, *, expected_version: int | None = None) -> MockSceneSnapshot | BlockedResult:
        if expected_version is not None and expected_version != self._scene_version:
            return self._blocked(
                "engine_snapshot_version_conflict",
                "engine.snapshot_version",
                "Caller expected a different mock SceneWorldSnapshot version.",
                "Refresh the snapshot and retry with its current scene_version.",
            )
        return MockSceneSnapshot(
            room_id=self._room_id,
            scene_version=self._scene_version,
            actors=tuple(self._actors_by_request.values()),
        )

    @staticmethod
    def _blocked(
        error_code: str,
        requirement_id: str,
        summary: str,
        next_action: str,
    ) -> BlockedResult:
        return BlockedResult(
            node_id="engine_test_double",
            status="blocked",
            error_code=error_code,
            summary=summary,
            missing_requirements=(
                MissingRequirement(
                    requirement_id=requirement_id,
                    owner_domain="engine",
                    description=summary,
                ),
            ),
            owner_domain="engine",
            retryable=True,
            next_action=next_action,
            evidence_refs=("test-double:engine",),
        )


__all__ = ["EngineTestDouble", "MockActorHandle", "MockSceneSnapshot"]

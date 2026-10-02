"""会话事件及其带类型的不可变载荷。"""

from dataclasses import dataclass

from .state import DropState, WorldOrb
from .types import DayPhase, DropId, ItemKind, OrbId, Position, WorldId


@dataclass(frozen=True, slots=True)
class DayNightChanged:
    day: int
    phase: DayPhase


@dataclass(frozen=True, slots=True)
class BossSpawned:
    position: Position


@dataclass(frozen=True, slots=True)
class BossChaseStarted:
    pass


@dataclass(frozen=True, slots=True)
class BossDefeated:
    position: Position


@dataclass(frozen=True, slots=True)
class DropSpawned:
    drop: DropState


@dataclass(frozen=True, slots=True)
class DropCollected:
    id: DropId
    item: ItemKind


@dataclass(frozen=True, slots=True)
class MerchantSpawned:
    position: Position


@dataclass(frozen=True, slots=True)
class MerchantDeparted:
    pass


@dataclass(frozen=True, slots=True)
class ItemGranted:
    item: ItemKind
    quantity: int
    orb: WorldOrb | None = None


@dataclass(frozen=True, slots=True)
class CreativeWorldCreated:
    world_id: WorldId
    orb_id: OrbId


@dataclass(frozen=True, slots=True)
class WorldChanged:
    from_world: WorldId
    to_world: WorldId
    player_position: Position


type EventPayload = (
    DayNightChanged | BossSpawned | BossChaseStarted | BossDefeated
    | DropSpawned | DropCollected | MerchantSpawned | MerchantDeparted
    | ItemGranted | CreativeWorldCreated | WorldChanged
)


@dataclass(frozen=True, slots=True)
class GameEvent:
    world_id: WorldId
    story_time_ns: int
    payload: EventPayload

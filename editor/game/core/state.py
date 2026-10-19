"""各玩法系统的不可变状态快照及世界跳转结果。"""

from dataclasses import dataclass

from .types import (
    BossPhase, DayPhase, DropId, ItemKind, MerchantPhase, OrbId, Position,
    WorldId, WorldKind, valid_id,
)


@dataclass(frozen=True, slots=True)
class ClockState:
    elapsed_ns: int = 0
    day: int = 1
    phase: DayPhase = DayPhase.DAY
    phase_elapsed_ns: int = 0


@dataclass(frozen=True, slots=True)
class ClockTransition:
    at_ns: int
    day: int
    phase: DayPhase


@dataclass(frozen=True, slots=True)
class WorldOrb:
    id: OrbId
    world_id: WorldId


@dataclass(frozen=True, slots=True)
class DropState:
    id: DropId
    item: ItemKind
    position: Position
    orb: WorldOrb | None = None
    collected: bool = False


@dataclass(frozen=True, slots=True)
class BossState:
    phase: BossPhase = BossPhase.NOT_SPAWNED
    position: Position = Position()


@dataclass(frozen=True, slots=True)
class MerchantState:
    phase: MerchantPhase = MerchantPhase.NOT_SPAWNED
    position: Position = Position()
    orb_claimed: bool = False
    fragment_claimed: bool = False


@dataclass(frozen=True, slots=True)
class InventoryState:
    orbs: tuple[WorldOrb, ...] = ()
    fragment_count: int = 0

    def find_orb(self, orb_id: OrbId) -> WorldOrb | None:
        if not valid_id(orb_id, OrbId):
            return None
        return next((orb for orb in self.orbs if orb.id == orb_id), None)


@dataclass(frozen=True, slots=True)
class CreativeWorldState:
    id: WorldId
    orb_id: OrbId
    kind: WorldKind = WorldKind.CREATIVE_SPACE
    initialized: bool = False
    editor_ui_enabled: bool = False
    story_rules_enabled: bool = False
    player_position: Position = Position()


@dataclass(frozen=True, slots=True)
class WorldNavigation:
    from_world: WorldId
    to_world: WorldId
    player_position: Position
    created: bool = False
    orb_id: OrbId | None = None

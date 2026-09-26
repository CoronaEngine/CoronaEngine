"""定义各玩法系统共享的数据类型和数值校验函数。
使用独立 ID 类型区分世界、小球和掉落，使用枚举表达阶段、道具和错误。
以冻结 dataclass 表达位置、命令结果、状态快照及事件载荷，并提供距离与时间换算。"""

from dataclasses import dataclass
from enum import Enum, auto
import math

NS_PER_SECOND = 1_000_000_000
MAX_TIME_NS = (1 << 63) - 1


@dataclass(frozen=True, slots=True)
class WorldId:
    value: int


@dataclass(frozen=True, slots=True)
class OrbId:
    value: int


@dataclass(frozen=True, slots=True)
class DropId:
    value: int


def valid_id(value: object, kind: type[WorldId] | type[OrbId] | type[DropId]) -> bool:
    return type(value) is kind and type(value.value) is int and value.value > 0


def finite_number(value: object) -> bool:
    if type(value) not in (int, float):
        return False
    try:
        return math.isfinite(value)
    except OverflowError:
        return False


@dataclass(frozen=True, slots=True)
class Position:
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0


def valid_position(value: object) -> bool:
    return type(value) is Position and all(
        finite_number(v) for v in (value.x, value.y, value.z)
    )


def horizontal_distance(a: Position, b: Position) -> float:
    return math.hypot(a.x - b.x, a.z - b.z)


def distance(a: Position, b: Position) -> float:
    return math.dist((a.x, a.y, a.z), (b.x, b.y, b.z))


def valid_delta_ns(value: object) -> bool:
    return type(value) is int and 0 <= value <= MAX_TIME_NS


def seconds_to_ns(seconds: object) -> int | None:
    """将秒数独立舍入到最近纳秒，半纳秒取偶数；非法或超出范围的输入返回 None。"""
    if not finite_number(seconds) or seconds < 0:
        return None
    if type(seconds) is int:
        ns = seconds * NS_PER_SECOND
    else:
        scaled = seconds * NS_PER_SECOND
        if not math.isfinite(scaled):
            return None
        ns = round(scaled)
    return ns if valid_delta_ns(ns) else None


class DayPhase(Enum):
    DAY = auto()
    NIGHT = auto()


class BossPhase(Enum):
    NOT_SPAWNED = auto()
    IDLE = auto()
    CHASING = auto()
    DEFEATED = auto()


class MerchantPhase(Enum):
    NOT_SPAWNED = auto()
    PRESENT = auto()
    DEPARTED = auto()


class ItemKind(Enum):
    WORLD_ORB = auto()
    WORLD_FRAGMENT = auto()


class WorldKind(Enum):
    STORY = auto()
    CREATIVE_SPACE = auto()


class Error(Enum):
    NONE = auto()
    INVALID_DELTA = auto()
    INVALID_POSITION = auto()
    INVALID_ID = auto()
    UNKNOWN_WORLD = auto()
    INVALID_ITEM = auto()
    PAUSED = auto()
    WRONG_WORLD = auto()
    BOSS_NOT_ALIVE = auto()
    BOSS_ALREADY_DEFEATED = auto()
    INVALID_BOSS_STATE = auto()
    DROP_NOT_FOUND = auto()
    ALREADY_COLLECTED = auto()
    OUT_OF_RANGE = auto()
    MERCHANT_UNAVAILABLE = auto()
    ALREADY_CLAIMED = auto()
    ORB_NOT_OWNED = auto()
    ALREADY_IN_CREATIVE_WORLD = auto()
    NOT_IN_CREATIVE_WORLD = auto()
    DUPLICATE_ORB = auto()


@dataclass(frozen=True, slots=True)
class Result[T]:
    error: Error = Error.NONE
    value: T | None = None

    @property
    def ok(self) -> bool:
        return self.error is Error.NONE


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

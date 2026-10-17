"""共享的身份、位置、枚举、命令结果及数值校验。"""

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

"""管理 Boss 的生命周期和水平追击行为。
Boss 仅生成一次，依次进入待机、追击或击败状态，击败后不再生成。
按传入的时间增量计算追击位移，保持生成高度，并通过安全区限制移动线段。"""

from copy import deepcopy
from dataclasses import replace
from random import Random

from ..core.config import GameConfig
from ..core.state import BossState
from ..core.types import (
    NS_PER_SECOND, BossPhase, Error, Position, Result, horizontal_distance, valid_delta_ns,
)
from ..world.safe_zone import SafeZone
from ..world.spawn import sample_boss_spawn


class BossSystem:
    def __init__(self, config: GameConfig = GameConfig()) -> None:
        if type(config) is not GameConfig:
            raise ValueError("config 必须是 GameConfig")
        config.validate()
        self._config = deepcopy(config)
        self._state = BossState()

    @property
    def state(self) -> BossState:
        return deepcopy(self._state)

    def spawn(self, player: Position, safe_zone: SafeZone, random: Random) -> Result[None]:
        if not self._config.accepts_position(player):
            return Result(Error.INVALID_POSITION)
        if self._state.phase is not BossPhase.NOT_SPAWNED:
            return Result(Error.INVALID_BOSS_STATE)
        position = sample_boss_spawn(player, self._config.boss_spawn_min_distance,
                                     self._config.boss_spawn_max_distance, safe_zone, random)
        self._state = BossState(BossPhase.IDLE, position)
        return Result()

    def start_chasing(self) -> Result[None]:
        if self._state.phase is not BossPhase.IDLE:
            return Result(Error.INVALID_BOSS_STATE)
        self._state = replace(self._state, phase=BossPhase.CHASING)
        return Result()

    def advance_ns(self, delta_ns: int, player: Position, safe_zone: SafeZone) -> Result[None]:
        if not valid_delta_ns(delta_ns):
            return Result(Error.INVALID_DELTA)
        if not self._config.accepts_position(player):
            return Result(Error.INVALID_POSITION)
        if not delta_ns or self._state.phase is not BossPhase.CHASING:
            return Result()
        start = self._state.position
        remaining = horizontal_distance(start, player)
        if not remaining:
            return Result()
        travel = min(remaining, self._config.boss_speed * (delta_ns / NS_PER_SECOND))
        ratio = travel / remaining
        target = Position(start.x + (player.x - start.x) * ratio, start.y,
                          start.z + (player.z - start.z) * ratio)
        self._state = replace(self._state, position=safe_zone.constrain_movement(start, target))
        return Result()

    def defeat(self) -> Result[None]:
        if self._state.phase is BossPhase.NOT_SPAWNED:
            return Result(Error.BOSS_NOT_ALIVE)
        if self._state.phase is BossPhase.DEFEATED:
            return Result(Error.BOSS_ALREADY_DEFEATED)
        self._state = replace(self._state, phase=BossPhase.DEFEATED)
        return Result()

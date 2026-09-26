"""管理商人的在场状态与免费赠品领取规则。
商人只能出现和离开一次，领取前检查道具类型、玩家位置及三维交互距离。
小球与碎片分别记录领取状态，重复领取或商人不在场时返回错误。"""

from copy import deepcopy
from dataclasses import replace
from random import Random

from .game_config import GameConfig
from .game_types import Error, ItemKind, MerchantPhase, MerchantState, Position, Result, distance
from .spawn_utils import sample_ring


class MerchantSystem:
    def __init__(self, config: GameConfig = GameConfig()) -> None:
        if type(config) is not GameConfig:
            raise ValueError("config 必须是 GameConfig")
        config.validate()
        self._config = deepcopy(config)
        self._state = MerchantState()

    @property
    def state(self) -> MerchantState:
        return deepcopy(self._state)

    def spawn(self, player: Position, random: Random) -> Result[None]:
        if not self._config.accepts_position(player):
            return Result(Error.INVALID_POSITION)
        if self._state.phase is not MerchantPhase.NOT_SPAWNED:
            return Result(Error.MERCHANT_UNAVAILABLE)
        position = sample_ring(player, self._config.merchant_spawn_min_distance,
                               self._config.merchant_spawn_max_distance, random)
        self._state = MerchantState(MerchantPhase.PRESENT, position)
        return Result()

    def depart(self) -> Result[None]:
        if self._state.phase is not MerchantPhase.PRESENT:
            return Result(Error.MERCHANT_UNAVAILABLE)
        self._state = replace(self._state, phase=MerchantPhase.DEPARTED)
        return Result()

    def can_claim(self, item: ItemKind, player: Position) -> Result[None]:
        if type(item) is not ItemKind:
            return Result(Error.INVALID_ITEM)
        if not self._config.accepts_position(player):
            return Result(Error.INVALID_POSITION)
        if self._state.phase is not MerchantPhase.PRESENT:
            return Result(Error.MERCHANT_UNAVAILABLE)
        claimed = (self._state.orb_claimed if item is ItemKind.WORLD_ORB
                   else self._state.fragment_claimed)
        if claimed:
            return Result(Error.ALREADY_CLAIMED)
        if distance(player, self._state.position) > self._config.interaction_distance:
            return Result(Error.OUT_OF_RANGE)
        return Result()

    def claim(self, item: ItemKind, player: Position) -> Result[None]:
        result = self.can_claim(item, player)
        if not result.ok:
            return result
        if item is ItemKind.WORLD_ORB:
            self._state = replace(self._state, orb_claimed=True)
        else:
            self._state = replace(self._state, fragment_claimed=True)
        return Result()

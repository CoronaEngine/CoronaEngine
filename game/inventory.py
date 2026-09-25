"""保存玩家拥有的独立世界小球和堆叠碎片。
小球按身份存储，同时检查小球 ID 与专属世界 ID，避免重复绑定进入背包。
碎片按非负整数数量累加；查询返回隔离快照，外部修改不会影响背包状态。"""

from copy import deepcopy

from .game_types import Error, InventoryState, OrbId, Result, WorldId, WorldOrb, valid_id


class Inventory:
    def __init__(self) -> None:
        self._orbs: dict[OrbId, WorldOrb] = {}
        self._fragment_count = 0

    @property
    def state(self) -> InventoryState:
        return deepcopy(InventoryState(tuple(self._orbs.values()), self._fragment_count))

    def find_orb(self, orb_id: OrbId) -> WorldOrb | None:
        if not valid_id(orb_id, OrbId):
            return None
        return deepcopy(self._orbs.get(orb_id))

    def add_orb(self, orb: WorldOrb) -> Result[None]:
        if (type(orb) is not WorldOrb or not valid_id(orb.id, OrbId)
                or not valid_id(orb.world_id, WorldId)):
            return Result(Error.INVALID_ITEM)
        # 小球身份与世界绑定都必须唯一，不能用另一颗小球重复指向已有世界。
        if orb.id in self._orbs or any(o.world_id == orb.world_id for o in self._orbs.values()):
            return Result(Error.DUPLICATE_ORB)
        stored = deepcopy(orb)
        self._orbs[stored.id] = stored
        return Result()

    def add_fragments(self, count: int = 1) -> Result[None]:
        if type(count) is not int or count < 0:
            return Result(Error.INVALID_ITEM)
        self._fragment_count += count
        return Result()

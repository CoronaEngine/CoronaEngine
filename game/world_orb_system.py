"""管理世界小球与专属创作世界的绑定和往返。
为每颗小球分配独立身份，首次进入时初始化空白世界，并分别保存各世界的玩家位置。
跳转时保存剧情返回位置，拒绝嵌套进入；只管理逻辑状态，不加载实际场景。"""

from collections.abc import Mapping
from copy import deepcopy
from dataclasses import replace
from types import MappingProxyType

from .game_config import GameConfig
from .game_types import (
    CreativeWorldState, Error, OrbId, Position, Result, WorldId, WorldNavigation,
    WorldOrb, valid_id,
)


class WorldOrbSystem:
    def __init__(self, story_world: WorldId, config: GameConfig = GameConfig()) -> None:
        if not valid_id(story_world, WorldId):
            raise ValueError("剧情世界必须使用正整数 WorldId")
        if type(config) is not GameConfig:
            raise ValueError("config 必须是 GameConfig")
        config.validate()
        self._config = deepcopy(config)
        self._story_world = deepcopy(story_world)
        self._active_world = self._story_world
        self._worlds: dict[WorldId, CreativeWorldState] = {}
        self._next_orb = 1
        self._next_world = 1
        self._return_position: Position | None = None

    @property
    def story_world(self) -> WorldId:
        return deepcopy(self._story_world)

    @property
    def active_world(self) -> WorldId:
        return deepcopy(self._active_world)

    @property
    def worlds(self) -> Mapping[WorldId, CreativeWorldState]:
        return MappingProxyType(deepcopy(self._worlds))

    def find_world(self, world_id: WorldId) -> CreativeWorldState | None:
        if not valid_id(world_id, WorldId):
            return None
        return deepcopy(self._worlds.get(world_id))

    def create_orb(self) -> Result[WorldOrb]:
        # 创作世界与剧情世界共用 WorldId 类型，分配时跳过来源剧情世界的身份。
        if self._next_world == self._story_world.value:
            self._next_world += 1
        orb = WorldOrb(OrbId(self._next_orb), WorldId(self._next_world))
        self._worlds[orb.world_id] = CreativeWorldState(orb.world_id, orb.id)
        self._next_orb += 1
        self._next_world += 1
        return Result(value=deepcopy(orb))

    def enter(self, orb: WorldOrb, story_position: Position) -> Result[WorldNavigation]:
        if (type(orb) is not WorldOrb or not valid_id(orb.id, OrbId)
                or not valid_id(orb.world_id, WorldId)):
            return Result(Error.INVALID_ITEM)
        if not self._config.accepts_position(story_position):
            return Result(Error.INVALID_POSITION)
        if self._active_world != self._story_world:
            return Result(Error.ALREADY_IN_CREATIVE_WORLD)
        world = self._worlds.get(orb.world_id)
        if world is None:
            return Result(Error.UNKNOWN_WORLD)
        if world.orb_id != orb.id:
            return Result(Error.INVALID_ITEM)
        navigation = WorldNavigation(self._active_world, world.id, world.player_position,
                                     not world.initialized, world.orb_id)
        # 世界在小球分配时登记，首次进入才初始化；再次进入保留该世界已有位置。
        self._worlds[world.id] = replace(world, initialized=True)
        self._return_position = deepcopy(story_position)
        self._active_world = world.id
        return Result(value=deepcopy(navigation))

    def update_position(self, world_id: WorldId, position: Position) -> Result[None]:
        if not valid_id(world_id, WorldId):
            return Result(Error.INVALID_ID)
        if not self._config.accepts_position(position):
            return Result(Error.INVALID_POSITION)
        if world_id != self._story_world and world_id not in self._worlds:
            return Result(Error.UNKNOWN_WORLD)
        if world_id != self._active_world:
            return Result(Error.WRONG_WORLD)
        if world_id != self._story_world:
            world = self._worlds[world_id]
            self._worlds[world_id] = replace(world, player_position=deepcopy(position))
        return Result()

    def return_to_story(self, creative_position: Position) -> Result[WorldNavigation]:
        if not self._config.accepts_position(creative_position):
            return Result(Error.INVALID_POSITION)
        if self._active_world == self._story_world or self._return_position is None:
            return Result(Error.NOT_IN_CREATIVE_WORLD)
        world = self._worlds[self._active_world]
        self._worlds[world.id] = replace(world, player_position=deepcopy(creative_position))
        navigation = WorldNavigation(self._active_world, self._story_world, self._return_position)
        self._active_world = self._story_world
        self._return_position = None
        return Result(value=deepcopy(navigation))

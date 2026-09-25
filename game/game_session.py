"""协调剧情会话中的计时、实体、奖励和世界跳转。
统一校验外部命令，按时间边界分段推进 Boss 与商人规则，并在暂停或创作期间冻结剧情。
负责掉落拾取和赠品入包，提供隔离状态快照以及可一次性取出的有序事件队列。"""

from collections.abc import Mapping
from copy import deepcopy
from dataclasses import replace
from random import Random

from .boss_system import BossSystem
from .game_config import GameConfig
from .game_types import (
    BossChaseStarted, BossDefeated, BossSpawned, BossState, ClockState, ClockTransition,
    CreativeWorldCreated, CreativeWorldState, DayNightChanged, DayPhase, DropCollected,
    DropId, DropSpawned, DropState, Error, EventPayload, GameEvent, InventoryState,
    ItemGranted, ItemKind, MerchantDeparted, MerchantSpawned, MerchantState, OrbId,
    Position, Result, WorldChanged, WorldId, WorldNavigation, distance, seconds_to_ns, valid_id,
)
from .inventory import Inventory
from .merchant_system import MerchantSystem
from .safe_zone import SafeZone
from .world_clock import WorldClock
from .world_orb_system import WorldOrbSystem


class GameSession:
    """由调用方串行驱动的内存会话，不读取系统时间、不访问引擎或磁盘。"""

    def __init__(self, story_world_id: WorldId = WorldId(1), spawn: Position = Position(),
                 config: GameConfig = GameConfig(), seed: int = 0) -> None:
        if type(config) is not GameConfig:
            raise ValueError("config 必须是 GameConfig")
        config.validate()
        if not valid_id(story_world_id, WorldId):
            raise ValueError("story_world_id 必须是正整数 WorldId")
        if not config.accepts_position(spawn):
            raise ValueError("spawn 必须是数值有效的 Position")
        if type(seed) is not int:
            raise ValueError("seed 必须是整数（不接受 bool）")
        self._config = deepcopy(config)
        self._clock = WorldClock(self._config)
        self._safe_zone = SafeZone(deepcopy(spawn), config.safe_zone_radius, config.safe_zone_clearance)
        self._boss = BossSystem(self._config)
        self._merchant = MerchantSystem(self._config)
        self._inventory = Inventory()
        self._orbs = WorldOrbSystem(story_world_id, self._config)
        self._random = Random(seed)
        self._player_position = deepcopy(spawn)
        self._paused = False
        self._drops: dict[DropId, DropState] = {}
        self._events: list[GameEvent] = []

    @property
    def config(self) -> GameConfig:
        return deepcopy(self._config)

    @property
    def clock(self) -> ClockState:
        return self._clock.state

    @property
    def paused(self) -> bool:
        return self._paused

    @property
    def story_running(self) -> bool:
        return not self._paused and self.active_world == self.story_world

    @property
    def story_world(self) -> WorldId:
        return self._orbs.story_world

    @property
    def active_world(self) -> WorldId:
        return self._orbs.active_world

    @property
    def player_position(self) -> Position:
        return deepcopy(self._player_position)

    @property
    def safe_zone(self) -> SafeZone:
        return deepcopy(self._safe_zone)

    @property
    def boss(self) -> BossState:
        return self._boss.state

    @property
    def merchant(self) -> MerchantState:
        return self._merchant.state

    @property
    def inventory(self) -> InventoryState:
        return self._inventory.state

    @property
    def drops(self) -> tuple[DropState, ...]:
        return deepcopy(tuple(self._drops.values()))

    @property
    def creative_worlds(self) -> Mapping[WorldId, CreativeWorldState]:
        return self._orbs.worlds

    def find_creative_world(self, world_id: WorldId) -> CreativeWorldState | None:
        return self._orbs.find_world(world_id)

    @property
    def events(self) -> tuple[GameEvent, ...]:
        return deepcopy(tuple(self._events))

    def take_events(self) -> tuple[GameEvent, ...]:
        events = self.events
        self._events.clear()
        return events

    def pause(self) -> Result[None]:
        self._paused = True
        return Result()

    def resume(self) -> Result[None]:
        self._paused = False
        return Result()

    def advance_seconds(self, seconds: float | int) -> Result[None]:
        delta_ns = seconds_to_ns(seconds)
        if delta_ns is None:
            return Result(Error.INVALID_DELTA)
        return self.advance_ns(delta_ns)

    def advance_ns(self, delta_ns: int) -> Result[None]:
        if not self._clock.can_advance(delta_ns):
            return Result(Error.INVALID_DELTA)
        if not self.story_running or not delta_ns:
            return Result()
        target = self._clock.state.elapsed_ns + delta_ns
        while self._clock.state.elapsed_ns < target:
            end = min(target, self._clock.next_boundary_ns)
            step = end - self._clock.state.elapsed_ns
            # 先按当前状态移动到边界，再执行阶段切换，避免追击提前作用于边界之前的时间。
            self._boss.advance_ns(step, self._player_position, self._safe_zone)
            transitions = self._clock.advance_ns(step).value
            assert transitions is not None
            for transition in transitions:
                self._on_transition(transition)
        return Result()

    def _emit(self, payload: EventPayload, world: WorldId | None = None) -> None:
        self._events.append(deepcopy(GameEvent(
            world if world is not None else self.story_world,
            self._clock.state.elapsed_ns, payload,
        )))

    def _on_transition(self, transition: ClockTransition) -> None:
        self._emit(DayNightChanged(transition.day, transition.phase))
        if transition.day == 1 and transition.phase is DayPhase.NIGHT:
            if self._boss.spawn(self._player_position, self._safe_zone, self._random).ok:
                self._emit(BossSpawned(self._boss.state.position))
        if transition.day == 2 and transition.phase is DayPhase.NIGHT:
            if self._boss.start_chasing().ok:
                self._emit(BossChaseStarted())
        if transition.day == 3 and transition.phase is DayPhase.DAY:
            if self._merchant.spawn(self._player_position, self._random).ok:
                self._emit(MerchantSpawned(self._merchant.state.position))
        if transition.day == 3 and transition.phase is DayPhase.NIGHT:
            if self._merchant.depart().ok:
                self._emit(MerchantDeparted())

    def _require_story(self) -> Result[None]:
        if self._paused:
            return Result(Error.PAUSED)
        if self.active_world != self.story_world:
            return Result(Error.WRONG_WORLD)
        return Result()

    def update_player_position(self, world_id: WorldId, position: Position) -> Result[None]:
        if self._paused:
            return Result(Error.PAUSED)
        result = self._orbs.update_position(world_id, position)
        if result.ok:
            self._player_position = deepcopy(position)
        return result

    def notify_boss_defeated(self) -> Result[None]:
        context = self._require_story()
        if not context.ok:
            return context
        result = self._boss.defeat()
        if not result.ok:
            return result
        position = self._boss.state.position
        orb = self._orbs.create_orb().value
        assert orb is not None
        drops = (
            DropState(DropId(1), ItemKind.WORLD_ORB, position, orb),
            DropState(DropId(2), ItemKind.WORLD_FRAGMENT, position),
        )
        self._emit(BossDefeated(position))
        for drop in drops:
            self._drops[drop.id] = drop
            self._emit(DropSpawned(drop))
        return Result()

    def collect_drop(self, drop_id: DropId) -> Result[None]:
        if not valid_id(drop_id, DropId):
            return Result(Error.INVALID_ID)
        context = self._require_story()
        if not context.ok:
            return context
        drop = self._drops.get(drop_id)
        if drop is None:
            return Result(Error.DROP_NOT_FOUND)
        if drop.collected:
            return Result(Error.ALREADY_COLLECTED)
        if distance(self._player_position, drop.position) > self._config.pickup_distance:
            return Result(Error.OUT_OF_RANGE)
        granted = (self._inventory.add_orb(drop.orb) if drop.item is ItemKind.WORLD_ORB
                   else self._inventory.add_fragments())
        if not granted.ok:
            return granted
        self._drops[drop.id] = replace(drop, collected=True)
        self._emit(DropCollected(drop.id, drop.item))
        self._emit(ItemGranted(drop.item, 1, drop.orb))
        return Result()

    def claim_merchant_gift(self, item: ItemKind) -> Result[None]:
        if type(item) is not ItemKind:
            return Result(Error.INVALID_ITEM)
        context = self._require_story()
        if not context.ok:
            return context
        allowed = self._merchant.can_claim(item, self._player_position)
        if not allowed.ok:
            return allowed
        orb = None
        # 会话串行处理领取；预检查通过后分配全新小球 ID，再将奖励和领取标记同步写入。
        if item is ItemKind.WORLD_ORB:
            orb = self._orbs.create_orb().value
            assert orb is not None
            granted = self._inventory.add_orb(orb)
        else:
            granted = self._inventory.add_fragments()
        assert granted.ok
        claimed = self._merchant.claim(item, self._player_position)
        assert claimed.ok
        self._emit(ItemGranted(item, 1, orb))
        return Result()

    def _apply_navigation(self, result: Result[WorldNavigation]) -> Result[None]:
        if not result.ok:
            return Result(result.error)
        navigation = result.value
        assert navigation is not None
        self._player_position = deepcopy(navigation.player_position)
        if navigation.created:
            self._emit(CreativeWorldCreated(navigation.to_world, navigation.orb_id), navigation.to_world)
        self._emit(WorldChanged(navigation.from_world, navigation.to_world,
                                navigation.player_position), navigation.to_world)
        return Result()

    def use_world_orb(self, orb_id: OrbId) -> Result[None]:
        if not valid_id(orb_id, OrbId):
            return Result(Error.INVALID_ID)
        if self._paused:
            return Result(Error.PAUSED)
        orb = self._inventory.find_orb(orb_id)
        if orb is None:
            return Result(Error.ORB_NOT_OWNED)
        return self._apply_navigation(self._orbs.enter(orb, self._player_position))

    def return_to_story(self) -> Result[None]:
        if self._paused:
            return Result(Error.PAUSED)
        return self._apply_navigation(self._orbs.return_to_story(self._player_position))

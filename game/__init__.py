"""提供剧情玩法包的公共入口。
统一导出会话、配置、身份类型、状态和事件，供调用方通过 game 包访问。
导入时不创建会话、不启动线程，也不调用引擎接口。"""

from .game_config import GameConfig
from .game_session import GameSession
from .game_types import (
    BossChaseStarted, BossDefeated, BossPhase, BossSpawned, BossState,
    ClockState, ClockTransition, CreativeWorldCreated, CreativeWorldState,
    DayNightChanged, DayPhase, DropCollected, DropId, DropSpawned, DropState,
    Error, EventPayload, GameEvent, InventoryState, ItemGranted, ItemKind,
    MerchantDeparted, MerchantPhase, MerchantSpawned, MerchantState,
    OrbId, Position, Result, WorldChanged, WorldId, WorldKind, WorldNavigation, WorldOrb,
)

__all__ = [
    "GameSession", "GameConfig", "WorldId", "OrbId", "DropId", "Position", "Result", "Error",
    "DayPhase", "BossPhase", "MerchantPhase", "ItemKind", "WorldKind", "WorldOrb",
    "ClockState", "ClockTransition", "BossState", "MerchantState", "DropState",
    "InventoryState", "CreativeWorldState", "WorldNavigation", "GameEvent", "EventPayload",
    "DayNightChanged", "BossSpawned", "BossChaseStarted", "BossDefeated", "DropSpawned",
    "DropCollected", "MerchantSpawned", "MerchantDeparted", "ItemGranted",
    "CreativeWorldCreated", "WorldChanged",
]

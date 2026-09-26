"""提供剧情玩法包的公共入口。
统一导出会话、配置、身份类型、状态和事件，供调用方通过 game 包访问。
导入时不创建会话、不启动线程，也不调用引擎接口。"""

from .core.config import GameConfig
from .core.events import (
    BossChaseStarted, BossDefeated, BossSpawned, CreativeWorldCreated, DayNightChanged,
    DropCollected, DropSpawned, EventPayload, GameEvent, ItemGranted, MerchantDeparted,
    MerchantSpawned, WorldChanged,
)
from .core.session import GameSession
from .core.state import (
    BossState, ClockState, ClockTransition, CreativeWorldState, DropState, InventoryState,
    MerchantState, WorldNavigation, WorldOrb,
)
from .core.types import (
    BossPhase, DayPhase, DropId, Error, ItemKind, MerchantPhase, OrbId, Position, Result,
    WorldId, WorldKind,
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

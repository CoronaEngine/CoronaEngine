"""生成玩家周围的水平圆环随机位置。
使用调用方提供的随机源进行圆环面积均匀采样，保持中心位置的高度。
Boss 生成点须位于安全区外；有限次采样未命中时，沿远离安全区的方向选点。"""

import math
from random import Random

from .game_types import Position, horizontal_distance
from .safe_zone import SafeZone


def sample_ring(center: Position, minimum: float, maximum: float, random: Random) -> Position:
    angle = random.random() * math.tau
    weight = random.random()
    radius = math.hypot(minimum * math.sqrt(1 - weight), maximum * math.sqrt(weight))
    return Position(center.x + math.cos(angle) * radius, center.y,
                    center.z + math.sin(angle) * radius)


def sample_boss_spawn(player: Position, minimum: float, maximum: float,
                      safe_zone: SafeZone, random: Random) -> Position:
    for _ in range(64):
        candidate = sample_ring(player, minimum, maximum, random)
        if safe_zone.allows_spawn(candidate):
            return candidate
    # 采样未命中时沿圆心到玩家的外向方向取上限距离，避免反复采样无法结束。
    length = horizontal_distance(player, safe_zone.center)
    ux = (player.x - safe_zone.center.x) / length if length else 1.0
    uz = (player.z - safe_zone.center.z) / length if length else 0.0
    return Position(player.x + maximum * ux, player.y, player.z + maximum * uz)

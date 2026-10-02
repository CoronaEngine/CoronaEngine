"""提供固定圆柱安全区的保护判定与移动限制。
仅使用水平坐标判断位置是否位于安全区内，高度不影响保护范围。
检测整条移动线段与安全区的交点，在首次接触前保留间隙，防止大步长穿越。"""

from dataclasses import dataclass
import math

from ..core.types import Position, finite_number, horizontal_distance, valid_position


@dataclass(frozen=True, slots=True)
class SafeZone:
    center: Position
    radius: float = 10.0
    clearance: float = 0.000001

    def __post_init__(self) -> None:
        if (not valid_position(self.center)
                or not finite_number(self.radius) or self.radius <= 0
                or not finite_number(self.clearance) or self.clearance <= 0
                or not math.isfinite(self.exclusion_radius)
                or self.exclusion_radius <= self.radius):
            raise ValueError("安全区中心、半径或间隙无效")

    @property
    def exclusion_radius(self) -> float:
        return self.radius + self.clearance

    def contains(self, position: Position) -> bool:
        return valid_position(position) and horizontal_distance(position, self.center) <= self.radius

    def allows_spawn(self, position: Position) -> bool:
        return (valid_position(position)
                and horizontal_distance(position, self.center) > self.exclusion_radius)

    def constrain_movement(self, start: Position, desired: Position) -> Position:
        if not valid_position(start) or not valid_position(desired):
            raise ValueError("移动线段端点必须是有限 Position")
        dx, dz = desired.x - start.x, desired.z - start.z
        length = math.hypot(dx, dz)
        if length == 0:
            return start
        ux, uz = dx / length, dz / length
        ox, oz = start.x - self.center.x, start.z - self.center.z
        projection = ox * ux + oz * uz
        radius = self.exclusion_radius
        start_distance = math.hypot(ox, oz)
        # 起点位于外侧间隙带时只允许远离圆心，防止数值误差让实体继续向内移动。
        if start_distance <= radius:
            return desired if start_distance > self.radius and projection >= 0 else start
        if projection >= 0:
            return desired
        perpendicular = abs(ox * uz - oz * ux)
        if perpendicular > radius:
            return desired
        # 用投影与半弦长求首次交点，再预留间隙；终点在圆外也可能需要截断。
        half_chord = radius * math.sqrt(max(0.0, 1.0 - (perpendicular / radius) ** 2))
        entry = -projection - half_chord
        if entry > length:
            return desired
        travel = min(length, max(0.0, entry - self.clearance))
        ratio = travel / length
        return Position(start.x + ux * travel,
                        start.y * (1 - ratio) + desired.y * ratio,
                        start.z + uz * travel)

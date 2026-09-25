"""定义并校验剧情玩法的集中配置。
配置涵盖昼夜时长、安全区、生成距离、追击速度和交互范围。
检查参数类型、取值范围及浮点精度，并判断玩家坐标能否安全参与几何计算。"""

from dataclasses import dataclass
import math

from .game_types import MAX_TIME_NS, NS_PER_SECOND, finite_number, valid_position


@dataclass(frozen=True, slots=True)
class GameConfig:
    day_duration_ns: int = 180 * NS_PER_SECOND
    night_duration_ns: int = 120 * NS_PER_SECOND
    safe_zone_radius: float = 10.0
    safe_zone_clearance: float = 0.000001
    boss_spawn_min_distance: float = 25.0
    boss_spawn_max_distance: float = 40.0
    boss_speed: float = 3.0
    merchant_spawn_min_distance: float = 3.0
    merchant_spawn_max_distance: float = 6.0
    interaction_distance: float = 3.0
    pickup_distance: float = 3.0

    def __post_init__(self) -> None:
        self.validate()

    def validate(self) -> None:
        durations = (self.day_duration_ns, self.night_duration_ns)
        if any(type(v) is not int or v <= 0 for v in durations):
            raise ValueError("昼夜时长必须是正整数纳秒（不接受 bool）")
        if 3 * sum(durations) > MAX_TIME_NS:
            raise ValueError("昼夜周期必须能在纳秒时间范围内表示完整三天")
        positive = (
            self.safe_zone_radius, self.safe_zone_clearance,
            self.boss_spawn_min_distance, self.boss_spawn_max_distance,
            self.merchant_spawn_max_distance, self.interaction_distance, self.pickup_distance,
        )
        nonnegative = (self.boss_speed, self.merchant_spawn_min_distance)
        if any(not finite_number(v) or v <= 0 for v in positive):
            raise ValueError("半径、间隙、生成上限和交互距离必须为有限正数")
        if any(not finite_number(v) or v < 0 for v in nonnegative):
            raise ValueError("速度和商人生成下限必须为有限非负数")
        outer = self.safe_zone_radius + self.safe_zone_clearance
        if (not math.isfinite(outer) or outer <= self.safe_zone_radius
                or self.boss_spawn_min_distance > self.boss_spawn_max_distance
                or self.merchant_spawn_min_distance > self.merchant_spawn_max_distance
                or self.boss_spawn_max_distance <= outer):
            raise ValueError("生成范围或安全区间隙无效，必须保证可生成在安全区外")
        # 限制几何数值的量级，避免生成偏移和线段计算溢出；随后检查安全间隙的精度。
        if max(*positive, *nonnegative) > 1e100:
            raise ValueError("空间配置超出浮点几何计算范围（1e100）")
        resolution = min(self.safe_zone_clearance, self.boss_spawn_min_distance,
                         self.merchant_spawn_max_distance) / 16
        scale = max(outer, self.boss_spawn_max_distance, self.merchant_spawn_max_distance)
        if resolution == 0 or math.ulp(float(scale)) > resolution:
            raise ValueError("生成范围过大或间隙过小，无法保持安全区及交互位置的数值精度")

    def accepts_position(self, position: object) -> bool:
        """检查坐标是否有限，并保证水平坐标的精度足以表达安全间隙和生成偏移。"""
        if not valid_position(position):
            return False
        resolution = min(
            self.safe_zone_clearance,
            self.boss_spawn_min_distance,
            self.merchant_spawn_max_distance,
        ) / 16
        return all(
            abs(v) <= 1e100 and math.ulp(float(v)) <= resolution
            for v in (position.x, position.z)
        )

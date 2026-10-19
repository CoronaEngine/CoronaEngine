"""验证配置范围、类型与浮点精度限制。"""

from dataclasses import fields

from game import GameConfig
from game.core.types import MAX_TIME_NS
from ..support import GameTestCase


class ConfigTests(GameTestCase):
    def test_config_rejects_unsafe_or_invalid_values(self) -> None:
        invalids = [
            dict(day_duration_ns=0), dict(night_duration_ns=-1), dict(day_duration_ns=MAX_TIME_NS),
            dict(boss_speed=-1), dict(safe_zone_radius=50), dict(safe_zone_clearance=0),
            dict(safe_zone_clearance=1e-30), dict(merchant_spawn_min_distance=7),
            dict(boss_spawn_min_distance=41), dict(boss_spawn_max_distance=0),
            dict(boss_spawn_max_distance=1e90), dict(merchant_spawn_max_distance=1e90),
            dict(safe_zone_clearance=5e-324),
        ]
        for field in fields(GameConfig):
            for bad in (None, True, '3', float('nan'), float('inf'), -float('inf'), 10**1000):
                invalids.append({field.name: bad})
        invalids.append(dict(day_duration_ns=1.0))
        for params in invalids:
            with self.subTest(params=params), self.assertRaises(ValueError):
                GameConfig(**params)
        self.assertEqual(GameConfig(boss_speed=0, merchant_spawn_min_distance=0).boss_speed, 0)

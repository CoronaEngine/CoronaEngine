"""验证随机生成点及极窄合法区域的回退采样。"""

from random import Random

from game import Position
from game.core.types import horizontal_distance
from game.world.safe_zone import SafeZone
from game.world.spawn import sample_boss_spawn
from ..support import GameTestCase


class SpawnTests(GameTestCase):
    def test_spawn_remains_outside_zone_for_many_seeds_and_players(self) -> None:
        zone = SafeZone(Position())
        for seed in range(300):
            player = Position(seed % 90 - 45, 123, seed % 30 - 15)
            spawn = sample_boss_spawn(player, 25, 40, zone, Random(seed))
            self.assertTrue(zone.allows_spawn(spawn))
            self.assertTrue(25 - 1e-10 <= horizontal_distance(player, spawn) <= 40 + 1e-10)
            self.assertEqual(spawn.y, player.y)

    def test_boss_spawn_fallback_stays_in_ring_with_extremely_narrow_legal_band(self) -> None:
        zone = SafeZone(Position())
        maximum = zone.exclusion_radius + 1e-8
        spawn = sample_boss_spawn(Position(), 0.1, maximum, zone, Random(3))
        self.assertTrue(zone.allows_spawn(spawn))
        self.assertTrue(0.1 <= horizontal_distance(Position(), spawn) <= maximum + 1e-10)

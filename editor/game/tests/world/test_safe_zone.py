"""验证安全区水平边界和多方向移动线段阻挡。"""

from random import Random

from game import GameSession, Position
from game.world.safe_zone import SafeZone
from game.world.spawn import sample_ring
from ..support import GameTestCase


class SafeZoneTests(GameTestCase):
    def test_safe_zone_uses_fixed_horizontal_center(self) -> None:
        center = Position(17, 99, -25)
        zone = SafeZone(center)
        self.assertTrue(zone.contains(Position(17, -100000, -25)))
        self.assertTrue(zone.contains(Position(27, 1e8, -25)))
        self.assertFalse(zone.contains(Position(27.01, 99, -25)))
        session = GameSession(spawn=center)
        self.assert_ok(session.update_player_position(session.story_world, Position(100, 200, 300)))
        self.assertEqual(session.safe_zone.center, center)

    def test_safe_zone_blocks_entire_segment_and_tunneling(self) -> None:
        zone = SafeZone(Position())
        blocked = zone.constrain_movement(Position(-20, 5, 0), Position(20, 5, 0))
        self.assertLess(blocked.x, -10)
        self.assertFalse(zone.contains(blocked))
        self.assertAlmostEqual(blocked.y, 5)
        self.assert_position_near(zone.constrain_movement(blocked, Position(20, 5, 0)), blocked)
        self.assertEqual(zone.constrain_movement(Position(-20, 0, 11), Position(20, 0, 11)),
                         Position(20, 0, 11))
        self.assertEqual(zone.constrain_movement(blocked, Position(-40, 5, 0)), Position(-40, 5, 0))
        tangent = zone.constrain_movement(Position(-20, 0, 10), Position(20, 0, 10))
        self.assertLess(tangent.x, 0)
        self.assertFalse(zone.contains(tangent))

    def test_safe_zone_clipped_segments_remain_outside_for_many_directions(self) -> None:
        zone = SafeZone(Position(3, 7, -9))
        random = Random(33)
        for _ in range(1000):
            start = sample_ring(zone.center, 11, 100, random)
            desired = sample_ring(zone.center, 0, 100, random)
            clipped = zone.constrain_movement(start, desired)
            self.assertFalse(zone.contains(clipped))
            dx, dz = clipped.x - start.x, clipped.z - start.z
            length_squared = dx * dx + dz * dz
            t = 0.0
            if length_squared:
                t = max(0, min(1, ((zone.center.x - start.x) * dx
                                   + (zone.center.z - start.z) * dz) / length_squared))
            self.assertFalse(zone.contains(Position(start.x + t * dx, start.y, start.z + t * dz)))

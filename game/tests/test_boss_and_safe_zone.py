"""验证 Boss 生命周期、生成位置和安全区阻挡。
检查一次生成、白天保留、按时追击、击败掉落及不再生成的规则。
通过多方向线段和多组随机种子，验证水平保护、切线阻挡、边界停留与离开后的追击。"""

from random import Random

from game import (
    BossChaseStarted, BossDefeated, BossPhase, BossSpawned, DropSpawned,
    Error, GameSession, ItemKind, Position, WorldId,
)
from game.game_types import NS_PER_SECOND as S, horizontal_distance
from game.safe_zone import SafeZone
from game.spawn_utils import sample_boss_spawn, sample_ring
from .support import GameTestCase, count_events


class BossAndSafeZoneTests(GameTestCase):
    def test_boss_spawns_once_and_survives_daytime(self) -> None:
        session = GameSession(seed=42)
        self.assertEqual(session.notify_boss_defeated().error, Error.BOSS_NOT_ALIVE)
        self.assert_ok(session.advance_ns(180 * S - 1))
        self.assertEqual(session.boss.phase, BossPhase.NOT_SPAWNED)
        self.assert_ok(session.advance_ns(1))
        self.assertEqual(session.boss.phase, BossPhase.IDLE)
        spawn = session.boss.position
        self.assertTrue(25 <= horizontal_distance(Position(), spawn) <= 40)
        self.assertTrue(session.safe_zone.allows_spawn(spawn))
        self.assert_ok(session.advance_seconds(120))
        self.assertEqual(session.boss.phase, BossPhase.IDLE)
        self.assertEqual(session.boss.position, spawn)
        self.assert_ok(session.advance_ns(180 * S - 1))
        self.assertEqual(session.boss.phase, BossPhase.IDLE)
        self.assert_ok(session.advance_ns(1))
        self.assertEqual(session.boss.phase, BossPhase.CHASING)
        self.assertEqual(session.boss.position, spawn)
        self.assert_ok(session.advance_seconds(1))
        self.assertAlmostEqual(horizontal_distance(session.boss.position, spawn), 3)
        self.assert_ok(session.advance_seconds(7200))
        self.assertEqual(count_events(session.events, BossSpawned), 1)
        self.assertEqual(count_events(session.events, BossChaseStarted), 1)

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

    def test_boss_waits_outside_safe_zone_then_resumes(self) -> None:
        session = GameSession(seed=7)
        self.assert_ok(session.advance_seconds(580))
        waiting = session.boss.position
        length = horizontal_distance(waiting, Position())
        self.assertTrue(10 < length < 10.001)
        self.assert_ok(session.advance_seconds(20))
        self.assert_position_near(waiting, session.boss.position)
        escaped = Position(waiting.x / length * 30, 900, waiting.z / length * 30)
        self.assert_ok(session.update_player_position(session.story_world, escaped))
        self.assert_ok(session.advance_seconds(1))
        self.assertAlmostEqual(horizontal_distance(waiting, session.boss.position), 3)
        self.assertEqual(session.boss.position.y, waiting.y)

    def test_spawn_remains_outside_zone_for_many_seeds_and_players(self) -> None:
        zone = SafeZone(Position())
        for seed in range(300):
            player = Position(seed % 90 - 45, 123, seed % 30 - 15)
            spawn = sample_boss_spawn(player, 25, 40, zone, Random(seed))
            self.assertTrue(zone.allows_spawn(spawn))
            self.assertTrue(25 - 1e-10 <= horizontal_distance(player, spawn) <= 40 + 1e-10)
            self.assertEqual(spawn.y, player.y)

    def test_defeat_drops_exactly_two_items_once_and_prevents_respawn(self) -> None:
        session = GameSession(seed=9)
        session.advance_seconds(180)
        death_position = session.boss.position
        self.assert_ok(session.notify_boss_defeated())
        self.assertEqual(session.boss.phase, BossPhase.DEFEATED)
        self.assertEqual([d.item for d in session.drops], [ItemKind.WORLD_ORB, ItemKind.WORLD_FRAGMENT])
        self.assertTrue(all(d.position == death_position for d in session.drops))
        self.assertEqual(session.inventory.orbs, ())
        self.assertEqual(session.inventory.fragment_count, 0)
        events = session.events
        self.assertEqual(session.notify_boss_defeated().error, Error.BOSS_ALREADY_DEFEATED)
        self.assertEqual(events, session.events)
        self.assert_ok(session.advance_seconds(86400))
        self.assertEqual(session.boss.phase, BossPhase.DEFEATED)
        self.assertEqual(len(session.drops), 2)
        for kind, count in ((BossSpawned, 1), (BossChaseStarted, 0), (BossDefeated, 1), (DropSpawned, 2)):
            self.assertEqual(count_events(session.events, kind), count)

    def test_defeat_during_chase_stops_movement(self) -> None:
        session = GameSession(WorldId(1), Position(100, 0, 0), seed=4)
        session.advance_seconds(481)
        self.assert_ok(session.notify_boss_defeated())
        position = session.boss.position
        session.advance_seconds(1000)
        self.assertEqual(session.boss.position, position)

    def test_chasing_boss_cannot_cross_safe_zone_to_player_on_opposite_side(self) -> None:
        session = GameSession(seed=101)
        session.advance_seconds(480)
        spawn = session.boss.position
        session.update_player_position(session.story_world, Position(-spawn.x, spawn.y, -spawn.z))
        session.advance_seconds(600)
        stopped = session.boss.position
        self.assertFalse(session.safe_zone.contains(stopped))
        self.assertGreater(stopped.x * spawn.x + stopped.z * spawn.z, 0)
        self.assertLess(horizontal_distance(stopped, Position()), 10.001)
        self.assertEqual(session.boss.phase, BossPhase.CHASING)

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

    def test_boss_spawn_fallback_stays_in_ring_with_extremely_narrow_legal_band(self) -> None:
        zone = SafeZone(Position())
        maximum = zone.exclusion_radius + 1e-8
        spawn = sample_boss_spawn(Position(), 0.1, maximum, zone, Random(3))
        self.assertTrue(zone.allows_spawn(spawn))
        self.assertTrue(0.1 <= horizontal_distance(Position(), spawn) <= maximum + 1e-10)

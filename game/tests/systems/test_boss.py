"""验证 Boss 生命周期、追击、安全区交互和击败掉落。"""

from game import (
    BossChaseStarted, BossDefeated, BossPhase, BossSpawned, DropSpawned,
    Error, GameSession, ItemKind, Position, WorldId,
)
from game.core.types import NS_PER_SECOND as S, horizontal_distance
from ..support import GameTestCase, count_events


class BossTests(GameTestCase):
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

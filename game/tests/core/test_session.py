"""验证会话调度、暂停恢复和有序事件输出。
比较大步长与分步推进的结果，检查暂停及创作世界停留对剧情时间的影响。
验证非法请求不改变状态、随机源可复现且彼此独立，以及生成使用边界时刻的玩家位置。"""

import random

from game import (
    BossChaseStarted, BossPhase, BossSpawned, DayNightChanged, DropId, Error,
    GameSession, ItemKind, MerchantDeparted, MerchantSpawned, OrbId, Position, WorldId,
)
from game.core.types import MAX_TIME_NS, NS_PER_SECOND as S, horizontal_distance
from ..support import GameTestCase, snapshot


class GameSessionTests(GameTestCase):
    def test_session_large_and_small_steps_have_equivalent_results(self) -> None:
        large, small = GameSession(seed=123), GameSession(seed=123)
        self.assert_ok(large.advance_seconds(800))
        for _ in range(8000):
            self.assert_ok(small.advance_ns(100_000_000))
        self.assertEqual(large.clock, small.clock)
        self.assertEqual(large.boss.phase, small.boss.phase)
        self.assert_position_near(large.boss.position, small.boss.position)
        self.assertEqual(large.merchant, small.merchant)
        self.assertEqual(large.events, small.events)
        expected = [
            (180, DayNightChanged), (180, BossSpawned), (300, DayNightChanged),
            (480, DayNightChanged), (480, BossChaseStarted), (600, DayNightChanged),
            (600, MerchantSpawned), (780, DayNightChanged), (780, MerchantDeparted),
        ]
        self.assertEqual([(e.story_time_ns // S, type(e.payload)) for e in large.events], expected)

    def test_pause_is_idempotent_and_freezes_time_and_commands(self) -> None:
        session = GameSession(seed=2)
        session.advance_seconds(179)
        self.assert_ok(session.pause())
        self.assert_ok(session.pause())
        self.assertTrue(session.paused)
        self.assertFalse(session.story_running)
        before = snapshot(session)
        self.assert_ok(session.advance_seconds(86400))
        commands = (
            lambda: session.update_player_position(session.story_world, Position(1)),
            session.notify_boss_defeated, lambda: session.collect_drop(DropId(1)),
            lambda: session.claim_merchant_gift(ItemKind.WORLD_ORB),
            lambda: session.use_world_orb(OrbId(1)), session.return_to_story,
        )
        for command in commands:
            self.assertEqual(command().error, Error.PAUSED)
        self.assertEqual(snapshot(session), before)
        self.assert_ok(session.resume())
        self.assert_ok(session.resume())
        self.assert_ok(session.advance_seconds(1))
        self.assertEqual(session.clock.elapsed_ns, 180 * S)
        self.assertEqual(session.boss.phase, BossPhase.IDLE)

    def test_manual_pause_in_creative_space_does_not_unpause_story(self) -> None:
        session = GameSession(seed=5)
        self.acquire_boss_loot(session)
        session.use_world_orb(session.inventory.orbs[0].id)
        session.pause()
        self.assertEqual(session.return_to_story().error, Error.PAUSED)
        session.resume()
        self.assertFalse(session.story_running)
        self.assert_ok(session.advance_seconds(100))
        self.assertEqual(session.clock.elapsed_ns, 180 * S)
        self.assert_ok(session.return_to_story())
        self.assert_ok(session.advance_seconds(1))
        self.assertEqual(session.clock.elapsed_ns, 181 * S)

    def test_invalid_requests_never_mutate_clock_position_inventory_or_events(self) -> None:
        session = GameSession(WorldId(42), seed=3)
        session.advance_seconds(10)
        before = snapshot(session)
        for invalid in (-1, float('nan'), float('inf'), 1e20, True, None, '10', [], 10**1000):
            self.assertEqual(session.advance_seconds(invalid).error, Error.INVALID_DELTA)
        for invalid in (-1, MAX_TIME_NS, True, 1.0, [], None):
            self.assertEqual(session.advance_ns(invalid).error, Error.INVALID_DELTA)
        for invalid in (Position(float('nan')), Position(0, float('inf')), Position(1e308),
                        Position(True), Position('1'), Position(10**1000), None, (0, 0, 0), []):
            self.assertEqual(session.update_player_position(session.story_world, invalid).error,
                             Error.INVALID_POSITION)
        self.assertEqual(session.update_player_position(WorldId(999), Position()).error, Error.UNKNOWN_WORLD)
        self.assertEqual(snapshot(session), before)

    def test_seed_is_repeatable_but_different_seeds_change_positions(self) -> None:
        first, same, different = GameSession(seed=44), GameSession(seed=44), GameSession(seed=45)
        global_state = random.getstate()
        for session in (first, same, different):
            session.advance_seconds(180)
        self.assertEqual(first.boss.position, same.boss.position)
        self.assertNotEqual(first.boss.position, different.boss.position)
        different.advance_seconds(1000)
        for session in (first, same):
            session.advance_seconds(420)
        self.assertEqual(first.merchant.position, same.merchant.position)
        self.assertEqual(first.events, same.events)
        self.assertEqual(random.getstate(), global_state)

    def test_events_are_typed_drained_and_not_repeated(self) -> None:
        session = GameSession(WorldId(81), seed=8)
        session.advance_seconds(180)
        events = session.take_events()
        self.assertEqual(len(events), 2)
        self.assertEqual(session.events, ())
        self.assertEqual(session.take_events(), ())
        session.advance_ns(0)
        session.advance_ns(1)
        self.assertEqual(session.events, ())
        for event in events:
            self.assertEqual(event.world_id, WorldId(81))
            self.assertEqual(event.story_time_ns, 180 * S)
        self.assertIsInstance(events[0].payload, DayNightChanged)
        self.assertIsInstance(events[1].payload, BossSpawned)

    def test_invalid_initial_world_or_spawn_throws_before_session_exists(self) -> None:
        for world in (WorldId(0), WorldId(-1), WorldId(True), WorldId(1.0), WorldId([]),
                      1, None, OrbId(1), DropId(1)):
            with self.subTest(world=world), self.assertRaises(ValueError):
                GameSession(world)
        for spawn in (Position(0, float('inf')), Position(float('nan')), Position(1e308), None, []):
            with self.subTest(spawn=spawn), self.assertRaises(ValueError):
                GameSession(spawn=spawn)
        for config in (None, {}, 1):
            with self.assertRaises(ValueError):
                GameSession(config=config)
        for seed in (None, True, '42', 1.5, float('nan')):
            with self.assertRaises(ValueError):
                GameSession(seed=seed)

    def test_double_seconds_adapter_accumulates_at_nanosecond_precision(self) -> None:
        session = GameSession(seed=8)
        for _ in range(1800):
            self.assert_ok(session.advance_seconds(0.1))
        self.assertEqual(session.clock.elapsed_ns, 180 * S)
        self.assertEqual(session.boss.phase, BossPhase.IDLE)

    def test_spawns_use_player_position_at_each_event_not_initial_spawn(self) -> None:
        session = GameSession(seed=3)
        boss_center = Position(100, 4, 75)
        session.advance_seconds(179)
        session.update_player_position(session.story_world, boss_center)
        session.advance_seconds(1)
        self.assertTrue(25 <= horizontal_distance(session.boss.position, boss_center) <= 40)
        self.assertEqual(session.boss.position.y, boss_center.y)
        session.advance_seconds(419)
        merchant_center = Position(-200, 7, 250)
        session.update_player_position(session.story_world, merchant_center)
        session.advance_seconds(1)
        self.assertTrue(3 <= horizontal_distance(session.merchant.position, merchant_center) <= 6)
        self.assertEqual(session.merchant.position.y, merchant_center.y)
        self.assertEqual(session.safe_zone.center, Position())

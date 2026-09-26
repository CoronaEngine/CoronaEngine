"""验证整数纳秒时钟、昼夜边界和配置校验。
覆盖初始阶段、关键节点前后、大步长事件顺序以及自定义昼夜时长对应的玩法行为。
检查秒到纳秒的舍入规则，并确认非法时间或配置不会被接受。"""

from dataclasses import fields

from game import BossPhase, DayPhase, Error, GameConfig, GameSession, MerchantPhase
from game.game_types import MAX_TIME_NS, NS_PER_SECOND as S, seconds_to_ns
from game.world_clock import WorldClock
from .support import GameTestCase


class WorldClockTests(GameTestCase):
    def test_clock_starts_at_day_one(self) -> None:
        clock = WorldClock()
        self.assertEqual((clock.state.elapsed_ns, clock.state.day, clock.state.phase,
                          clock.state.phase_elapsed_ns), (0, 1, DayPhase.DAY, 0))

    def test_clock_all_key_boundaries_are_exact(self) -> None:
        for boundary in (180, 300, 480, 600, 780):
            with self.subTest(boundary=boundary):
                clock = WorldClock()
                clock.advance_ns(boundary * S - 1)
                before = clock.state
                changes = clock.advance_ns(1).value
                self.assertEqual(len(changes), 1)
                self.assertEqual(changes[0].at_ns, boundary * S)
                self.assertEqual(clock.state.elapsed_ns, boundary * S)
                self.assertNotEqual(clock.state.phase, before.phase)
                self.assertEqual(clock.state.phase_elapsed_ns, 0)
                self.assertEqual(clock.advance_ns(1).value, ())
                self.assertEqual(clock.state.phase_elapsed_ns, 1)

    def test_clock_large_step_emits_ordered_boundaries(self) -> None:
        clock = WorldClock()
        changes = clock.advance_ns(780 * S).value
        self.assertEqual([(v.at_ns // S, v.day, v.phase) for v in changes], [
            (180, 1, DayPhase.NIGHT), (300, 2, DayPhase.DAY), (480, 2, DayPhase.NIGHT),
            (600, 3, DayPhase.DAY), (780, 3, DayPhase.NIGHT),
        ])
        self.assertEqual(clock.advance_ns(0).value, ())

    def test_clock_rejects_invalid_delta_without_mutating(self) -> None:
        clock = WorldClock()
        clock.advance_ns(179 * S)
        before = clock.state
        for invalid in (-1, True, False, 1.0, '1', None, [], float('nan'), float('inf'), MAX_TIME_NS):
            with self.subTest(invalid=invalid):
                self.assertEqual(clock.advance_ns(invalid).error, Error.INVALID_DELTA)
                self.assertEqual(clock.state, before)
        for invalid in (-1, float('nan'), float('inf'), 1e20, 10**1000, True, '1', None):
            self.assertIsNone(seconds_to_ns(invalid))
        self.assertEqual(seconds_to_ns(0.1), 100_000_000)
        self.assertEqual(seconds_to_ns(180), 180 * S)
        self.assertEqual(seconds_to_ns(0.5e-9), 0)
        self.assertEqual(seconds_to_ns(1.5e-9), 2)
        self.assertEqual(seconds_to_ns(2.5e-9), 2)

    def test_clock_and_events_support_configured_durations(self) -> None:
        session = GameSession(config=GameConfig(day_duration_ns=3 * S, night_duration_ns=2 * S))
        for delta, boss, merchant in (
            (3, BossPhase.IDLE, MerchantPhase.NOT_SPAWNED),
            (5, BossPhase.CHASING, MerchantPhase.NOT_SPAWNED),
            (2, BossPhase.CHASING, MerchantPhase.PRESENT),
            (3, BossPhase.CHASING, MerchantPhase.DEPARTED),
        ):
            self.assert_ok(session.advance_seconds(delta))
            self.assertEqual(session.boss.phase, boss)
            self.assertEqual(session.merchant.phase, merchant)
        self.assertEqual([e.story_time_ns for e in session.events],
                         [v * S for v in (3, 3, 5, 8, 8, 10, 10, 13, 13)])

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

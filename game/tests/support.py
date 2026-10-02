"""提供玩法测试共用的断言和操作辅助函数。
统计指定类型的事件、汇总会话状态快照，并封装位置近似比较与 Boss 掉落拾取流程。
帮助各测试一致地检查命令结果、状态变化和失败请求的无副作用行为。"""

import unittest

from game import GameSession, Position, Result
from game.core.events import EventPayload, GameEvent
from game.core.types import distance


def count_events(events: tuple[GameEvent, ...], kind: type[EventPayload]) -> int:
    return sum(isinstance(event.payload, kind) for event in events)


def snapshot(session: GameSession) -> tuple[object, ...]:
    return (session.config, session.clock, session.paused, session.story_running,
            session.story_world, session.active_world, session.player_position,
            session.safe_zone, session.boss, session.merchant, session.inventory,
            session.drops, dict(session.creative_worlds), session.events)


class GameTestCase(unittest.TestCase):
    def assert_ok(self, result: Result) -> None:
        self.assertTrue(result.ok, result.error)

    def assert_position_near(self, first: Position, second: Position, tolerance: float = 1e-8) -> None:
        self.assertLessEqual(distance(first, second), tolerance)

    def acquire_boss_loot(self, session: GameSession) -> None:
        self.assert_ok(session.advance_seconds(180))
        self.assert_ok(session.notify_boss_defeated())
        self.assert_ok(session.update_player_position(session.story_world, session.boss.position))
        for drop in session.drops:
            self.assert_ok(session.collect_drop(drop.id))

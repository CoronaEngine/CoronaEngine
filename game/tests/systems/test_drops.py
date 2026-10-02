"""验证掉落拾取距离、背包奖励和重复拾取保护。"""

from game import DropCollected, DropId, Error, GameSession, ItemGranted, Position
from ..support import GameTestCase, count_events, snapshot


class DropTests(GameTestCase):
    def test_drops_require_distance_and_are_collectible_exactly_once(self) -> None:
        session = GameSession(seed=42)
        self.assertEqual(session.collect_drop(DropId(1)).error, Error.DROP_NOT_FOUND)
        session.advance_seconds(180)
        session.notify_boss_defeated()
        p = session.boss.position
        before = snapshot(session)
        self.assertEqual(session.collect_drop(DropId(1)).error, Error.OUT_OF_RANGE)
        self.assertEqual(session.use_world_orb(session.drops[0].orb.id).error, Error.ORB_NOT_OWNED)
        self.assertEqual(snapshot(session), before)
        session.update_player_position(session.story_world, Position(p.x, p.y + 3.01, p.z))
        self.assertEqual(session.collect_drop(DropId(2)).error, Error.OUT_OF_RANGE)
        session.update_player_position(session.story_world, Position(p.x, p.y + 3, p.z))
        self.assert_ok(session.collect_drop(DropId(2)))
        self.assert_ok(session.collect_drop(DropId(1)))
        self.assertEqual(len(session.inventory.orbs), 1)
        self.assertEqual(session.inventory.fragment_count, 1)
        self.assertEqual(session.inventory.orbs[0], session.drops[0].orb)
        before = snapshot(session)
        for drop in session.drops:
            self.assertEqual(session.collect_drop(drop.id).error, Error.ALREADY_COLLECTED)
        self.assertEqual(session.collect_drop(DropId(999)).error, Error.DROP_NOT_FOUND)
        self.assertEqual(snapshot(session), before)
        self.assertEqual(count_events(session.events, DropCollected), 2)
        self.assertEqual(count_events(session.events, ItemGranted), 2)

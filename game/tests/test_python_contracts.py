"""验证公共接口的类型约束、快照隔离和使用示例。
覆盖非法 ID、嵌套只读对象、输入对象复制、奖励身份分配及事件载荷的稳定性。
同时检查随机分段推进、导入无会话或线程副作用，并实际执行 README 中的 Python 示例。"""

from dataclasses import FrozenInstanceError
from pathlib import Path
import random
import re
import subprocess
import sys

from game import (
    BossPhase, CreativeWorldCreated, DayNightChanged, DropCollected, DropId, DropSpawned,
    Error, GameConfig, GameSession, ItemGranted, ItemKind, OrbId, Position, WorldChanged, WorldId,
)
from game.game_types import NS_PER_SECOND as S
from .support import GameTestCase, count_events, snapshot


class PythonContractTests(GameTestCase):
    def test_id_types_are_distinct_and_malformed_ids_do_not_mutate(self) -> None:
        self.assertEqual(len({WorldId(1), OrbId(1), DropId(1)}), 3)
        session = GameSession()
        self.acquire_boss_loot(session)
        before = snapshot(session)
        common = (None, True, 1, '1', [], {}, -1, 1.0)
        for kind, command in (
            (WorldId, lambda v: session.update_player_position(v, Position())),
            (OrbId, session.use_world_orb), (DropId, session.collect_drop),
        ):
            invalids = common + tuple(kind(v) for v in (0, -1, True, 1.0, '1', [], float('nan')))
            invalids += tuple(other(1) for other in (WorldId, OrbId, DropId) if other is not kind)
            for invalid in invalids:
                with self.subTest(kind=kind, invalid=invalid):
                    self.assertEqual(command(invalid).error, Error.INVALID_ID)
                    self.assertEqual(snapshot(session), before)
        for invalid in (*common, OrbId(1), WorldId([])):
            self.assertIsNone(session.find_creative_world(invalid))
        for invalid in (*common, WorldId(1), OrbId([])):
            self.assertIsNone(session.inventory.find_orb(invalid))
        self.assertEqual(snapshot(session), before)

    def test_query_collections_and_nested_dataclasses_are_read_only(self) -> None:
        session = GameSession()
        self.acquire_boss_loot(session)
        orb = session.inventory.orbs[0]
        before = snapshot(session)
        objects = (
            (session.config, 'boss_speed', 900), (session.clock, 'elapsed_ns', 0),
            (session.player_position, 'x', 900), (session.safe_zone, 'radius', 900),
            (session.safe_zone.center, 'x', 900), (session.boss, 'phase', BossPhase.IDLE),
            (session.boss.position, 'x', 900), (session.merchant, 'orb_claimed', True),
            (session.inventory, 'fragment_count', 900), (orb.id, 'value', 900),
            (orb.world_id, 'value', 900), (session.drops[0], 'collected', False),
            (session.drops[0].orb, 'world_id', WorldId(900)),
            (session.creative_worlds[orb.world_id], 'initialized', True),
            (session.find_creative_world(orb.world_id).player_position, 'x', 900),
            (session.events[0], 'story_time_ns', 0), (session.events[1].payload.position, 'x', 900),
            (session.story_world, 'value', 900), (session.active_world, 'value', 900),
        )
        for obj, field, value in objects:
            with self.assertRaises(FrozenInstanceError):
                setattr(obj, field, value)
        for collection in (session.inventory.orbs, session.drops, session.events):
            with self.assertRaises(AttributeError):
                collection.clear()
        with self.assertRaises(TypeError):
            session.creative_worlds[orb.world_id] = None
        self.assertEqual(snapshot(session), before)
        # 绕过 frozen 修改快照，验证深拷贝仍能隔离会话内部状态。
        for obj, field, value in objects:
            object.__setattr__(obj, field, value)
        self.assertEqual(snapshot(session), before)

    def test_constructor_and_position_inputs_are_detached(self) -> None:
        world, spawn, config = WorldId(8), Position(5, 6, 7), GameConfig()
        session = GameSession(world, spawn, config, seed=7)
        before = snapshot(session)
        object.__setattr__(world, 'value', 99)
        object.__setattr__(spawn, 'x', 99)
        object.__setattr__(config, 'boss_speed', 99)
        self.assertEqual(snapshot(session), before)
        position = Position(10, 20, 30)
        session.update_player_position(session.story_world, position)
        object.__setattr__(position, 'x', 99)
        self.assertEqual(session.player_position, Position(10, 20, 30))
        self.acquire_boss_loot(session)
        orb = session.inventory.orbs[0]
        session.use_world_orb(orb.id)
        position = Position(3, 4, 5)
        session.update_player_position(orb.world_id, position)
        object.__setattr__(position, 'x', 99)
        self.assertEqual(session.find_creative_world(orb.world_id).player_position, Position(3, 4, 5))

    def test_failure_does_not_consume_randomness_or_reward_identity(self) -> None:
        first, second = GameSession(seed=12), GameSession(seed=12)
        self.assertEqual(first.collect_drop(DropId(2)).error, Error.DROP_NOT_FOUND)
        self.assertEqual(first.claim_merchant_gift(ItemKind.WORLD_ORB).error, Error.MERCHANT_UNAVAILABLE)
        self.assertEqual(first.use_world_orb(OrbId(1)).error, Error.ORB_NOT_OWNED)
        for session in (first, second):
            self.acquire_boss_loot(session)
            session.advance_seconds(420)
            session.update_player_position(session.story_world, session.merchant.position)
            session.claim_merchant_gift(ItemKind.WORLD_ORB)
        self.assertEqual(snapshot(first), snapshot(second))

    def test_drop_reservation_before_merchant_claim_does_not_collide(self) -> None:
        session = GameSession(WorldId(2))
        session.advance_seconds(180)
        session.notify_boss_defeated()
        reserved = session.drops[0].orb
        session.advance_seconds(420)
        session.update_player_position(session.story_world, session.merchant.position)
        session.claim_merchant_gift(ItemKind.WORLD_ORB)
        merchant = session.inventory.orbs[0]
        self.assertNotEqual(reserved.id, merchant.id)
        self.assertNotEqual(reserved.world_id, merchant.world_id)
        session.update_player_position(session.story_world, session.boss.position)
        session.collect_drop(DropId(1))
        session.use_world_orb(merchant.id)
        before = snapshot(session)
        self.assertEqual(session.use_world_orb(reserved.id).error, Error.ALREADY_IN_CREATIVE_WORLD)
        self.assertEqual(snapshot(session), before)
        self.assert_ok(session.return_to_story())
        self.assert_ok(session.use_world_orb(reserved.id))
        self.assertEqual(len(session.creative_worlds), 2)

    def test_navigation_and_reward_events_have_stable_typed_payloads(self) -> None:
        session = GameSession(WorldId(88))
        self.acquire_boss_loot(session)
        events = session.take_events()
        self.assertEqual([type(e.payload) for e in events[3:]],
                         [DropSpawned, DropSpawned, DropCollected, ItemGranted, DropCollected, ItemGranted])
        for event in events:
            self.assertEqual(event.world_id, session.story_world)
            self.assertEqual(event.story_time_ns, 180 * S)
        self.assertFalse(events[3].payload.drop.collected)
        self.assertTrue(session.drops[0].collected)
        orb = session.inventory.orbs[0]
        origin = session.player_position
        session.use_world_orb(orb.id)
        session.update_player_position(orb.world_id, Position(3, 2, 1))
        session.advance_seconds(10000)
        session.return_to_story()
        session.use_world_orb(orb.id)
        events = session.take_events()
        self.assertEqual([type(e.payload) for e in events],
                         [CreativeWorldCreated, WorldChanged, WorldChanged, WorldChanged])
        self.assertEqual([e.world_id for e in events],
                         [orb.world_id, orb.world_id, session.story_world, orb.world_id])
        self.assertEqual(events[2].payload.player_position, origin)
        self.assertTrue(all(e.story_time_ns == 180 * S for e in events))
        self.assertEqual(events[3].payload.player_position, Position(3, 2, 1))
        self.assertEqual(session.take_events(), ())
        before = snapshot(session)
        object.__setattr__(events[0].payload.world_id, 'value', 999)
        self.assertEqual(snapshot(session), before)

    def test_boundary_actions_before_and_after_single_nanosecond(self) -> None:
        for boundary in (180, 300, 480, 600, 780):
            large, split = GameSession(seed=23), GameSession(seed=23)
            large.advance_ns(boundary * S + 1)
            split.advance_ns(boundary * S - 1)
            before_count = count_events(split.events, DayNightChanged)
            split.advance_ns(1)
            self.assertEqual(count_events(split.events, DayNightChanged), before_count + 1)
            boundary_events = split.events
            split.advance_ns(1)
            self.assertEqual(split.events, boundary_events)
            self.assertEqual(large.clock, split.clock)
            self.assert_position_near(large.boss.position, split.boss.position)
            self.assertEqual(large.events, split.events)

    def test_random_partitions_and_changed_target_preserve_segment_behavior(self) -> None:
        large, split = GameSession(seed=8), GameSession(seed=8)
        partitions = random.Random(567)
        for end, target in ((179 * S, Position(100, 8, 75)), (479 * S, Position(-50, 9, -30)),
                            (481 * S, Position(3, 10, 2)), (800 * S, Position())):
            for session in (large, split):
                session.update_player_position(session.story_world, target)
            remaining = end - large.clock.elapsed_ns
            large.advance_ns(remaining)
            while remaining:
                step = min(remaining, partitions.randint(1, 3 * S))
                split.advance_ns(step)
                remaining -= step
            self.assertEqual(large.clock, split.clock)
            self.assert_position_near(large.boss.position, split.boss.position)
            self.assertEqual(large.merchant, split.merchant)
            self.assertEqual(large.events, split.events)

    def test_invalid_time_is_rejected_even_while_frozen(self) -> None:
        session = GameSession()
        self.acquire_boss_loot(session)
        for creative in (False, True):
            if creative:
                session.resume()
                session.use_world_orb(session.inventory.orbs[0].id)
            else:
                session.pause()
            before = snapshot(session)
            for value in (-1, float('nan'), float('inf'), True, '1'):
                self.assertEqual(session.advance_seconds(value).error, Error.INVALID_DELTA)
            self.assertEqual(snapshot(session), before)

    def test_configured_distances_and_zero_speed_are_used(self) -> None:
        config = GameConfig(safe_zone_radius=2, boss_spawn_min_distance=5,
                            boss_spawn_max_distance=5, boss_speed=0,
                            merchant_spawn_min_distance=1, merchant_spawn_max_distance=1,
                            interaction_distance=0.5, pickup_distance=0.25)
        session = GameSession(config=config)
        session.advance_seconds(180)
        position = session.boss.position
        session.advance_seconds(420)
        self.assertEqual(session.boss.position, position)
        merchant = session.merchant.position
        session.update_player_position(session.story_world, Position(merchant.x, merchant.y + 0.51, merchant.z))
        self.assertEqual(session.claim_merchant_gift(ItemKind.WORLD_FRAGMENT).error, Error.OUT_OF_RANGE)
        session.update_player_position(session.story_world, Position(merchant.x, merchant.y + 0.5, merchant.z))
        self.assert_ok(session.claim_merchant_gift(ItemKind.WORLD_FRAGMENT))
        session.notify_boss_defeated()
        session.update_player_position(session.story_world, Position(position.x, position.y + 0.26, position.z))
        self.assertEqual(session.collect_drop(DropId(2)).error, Error.OUT_OF_RANGE)
        session.update_player_position(session.story_world, Position(position.x, position.y + 0.25, position.z))
        self.assert_ok(session.collect_drop(DropId(2)))

    def test_import_does_not_create_session_threads_or_use_global_random(self) -> None:
        code = '''
import random
import threading
from unittest.mock import patch
before = threading.enumerate()
state = random.getstate()
with patch("random.Random", side_effect=AssertionError("import instantiated Random")):
    import game
assert threading.enumerate() == before
assert random.getstate() == state
assert not any(isinstance(v, game.GameSession) for v in vars(game).values())
'''
        result = subprocess.run([sys.executable, '-c', code], cwd=Path(__file__).resolve().parents[2],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, '')

    def test_readme_python_examples_run(self) -> None:
        readme = Path(__file__).resolve().parents[1] / 'README.md'
        blocks = re.findall(r'```python\n(.*?)```', readme.read_text(encoding='utf-8-sig'), re.DOTALL)
        self.assertGreater(len(blocks), 0)
        for code in blocks:
            result = subprocess.run([sys.executable, '-c', code], cwd=readme.parent.parent,
                                    capture_output=True, text=True, encoding='utf-8', timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)

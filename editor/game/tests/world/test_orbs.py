"""验证小球专属世界的身份分配、初始化和位置保存。
检查不同奖励来源的小球互不共用世界，多个创作世界往返时各自状态保持独立。
覆盖创作期间冻结剧情、返回位置恢复，以及错误绑定和嵌套跳转的拒绝行为。"""

from game import (
    CreativeWorldCreated, DropId, Error, GameSession, ItemKind, MerchantPhase,
    OrbId, Position, WorldChanged, WorldId, WorldKind, WorldOrb,
)
from game.core.types import NS_PER_SECOND as S
from game.world.orbs import WorldOrbSystem
from ..support import GameTestCase, count_events, snapshot


class WorldOrbTests(GameTestCase):
    def test_boss_and_merchant_orbs_bind_distinct_worlds(self) -> None:
        session = GameSession(WorldId(7), seed=11)
        self.acquire_boss_loot(session)
        session.advance_seconds(420)
        session.update_player_position(session.story_world, session.merchant.position)
        self.assert_ok(session.claim_merchant_gift(ItemKind.WORLD_ORB))
        self.assert_ok(session.claim_merchant_gift(ItemKind.WORLD_FRAGMENT))
        self.assertEqual(session.inventory.fragment_count, 2)
        first, second = session.inventory.orbs
        self.assertNotEqual(first.id, second.id)
        self.assertEqual(len({first.world_id, second.world_id, session.story_world}), 3)
        self.assertEqual(len(session.creative_worlds), 2)
        self.assertFalse(session.find_creative_world(first.world_id).initialized)
        self.assertFalse(session.find_creative_world(second.world_id).initialized)
        return_position = session.player_position
        positions = (Position(15, 3, 12), Position(-8, 4, 25))
        old_worlds = session.creative_worlds
        for orb, position in zip((first, second), positions):
            self.assert_ok(session.use_world_orb(orb.id))
            self.assertEqual(session.player_position, Position())
            self.assert_ok(session.update_player_position(orb.world_id, position))
            self.assert_ok(session.return_to_story())
            self.assertEqual(session.player_position, return_position)
        self.assertFalse(old_worlds[first.world_id].initialized)
        self.assert_ok(session.use_world_orb(first.id))
        self.assertEqual(session.player_position, positions[0])
        self.assert_ok(session.return_to_story())
        self.assert_ok(session.use_world_orb(second.id))
        self.assertEqual(session.player_position, positions[1])
        self.assertEqual(len(session.inventory.orbs), 2)
        self.assertEqual(count_events(session.events, CreativeWorldCreated), 2)
        self.assertEqual(count_events(session.events, WorldChanged), 7)

    def test_creative_world_pauses_story_and_never_spawns_story_entities(self) -> None:
        session = GameSession(seed=55)
        session.advance_seconds(600)
        session.update_player_position(session.story_world, session.merchant.position)
        session.claim_merchant_gift(ItemKind.WORLD_ORB)
        orb = session.inventory.orbs[0]
        story_position, boss_position = session.player_position, session.boss.position
        self.assert_ok(session.use_world_orb(orb.id))
        self.assertFalse(session.story_running)
        self.assertFalse(session.paused)
        world = session.find_creative_world(orb.world_id)
        self.assertTrue(world.initialized)
        self.assertFalse(world.editor_ui_enabled)
        self.assertFalse(world.story_rules_enabled)
        self.assertEqual(world.kind, WorldKind.CREATIVE_SPACE)
        session.take_events()
        self.assert_ok(session.advance_seconds(48 * 3600))
        self.assertEqual(session.clock.elapsed_ns, 600 * S)
        self.assertEqual(session.events, ())
        self.assertEqual(session.boss.position, boss_position)
        self.assertEqual(session.merchant.phase, MerchantPhase.PRESENT)
        self.assertEqual(session.notify_boss_defeated().error, Error.WRONG_WORLD)
        self.assertEqual(session.claim_merchant_gift(ItemKind.WORLD_FRAGMENT).error, Error.WRONG_WORLD)
        self.assertEqual(session.collect_drop(DropId(1)).error, Error.WRONG_WORLD)
        self.assert_ok(session.return_to_story())
        self.assertEqual(session.player_position, story_position)
        self.assertTrue(session.story_running)
        session.advance_seconds(180)
        self.assertEqual(session.clock.elapsed_ns, 780 * S)
        self.assertEqual(session.merchant.phase, MerchantPhase.DEPARTED)

    def test_invalid_orb_world_and_nested_navigation_preserve_state(self) -> None:
        session = GameSession(seed=22)
        self.assertEqual(session.use_world_orb(OrbId(1)).error, Error.ORB_NOT_OWNED)
        self.assertEqual(session.return_to_story().error, Error.NOT_IN_CREATIVE_WORLD)
        self.assertEqual(len(session.creative_worlds), 0)
        self.acquire_boss_loot(session)
        orb = session.inventory.orbs[0]
        before = snapshot(session)
        self.assertEqual(session.update_player_position(WorldId(999), Position(12)).error, Error.UNKNOWN_WORLD)
        self.assertEqual(session.update_player_position(orb.world_id, Position(12)).error, Error.WRONG_WORLD)
        self.assertEqual(snapshot(session), before)
        story_position = session.player_position
        self.assert_ok(session.use_world_orb(orb.id))
        before = snapshot(session)
        self.assertEqual(session.use_world_orb(orb.id).error, Error.ALREADY_IN_CREATIVE_WORLD)
        self.assertEqual(session.update_player_position(session.story_world, Position(4)).error, Error.WRONG_WORLD)
        self.assertEqual(session.use_world_orb(OrbId(999)).error, Error.ORB_NOT_OWNED)
        self.assertEqual(snapshot(session), before)
        self.assert_ok(session.return_to_story())
        self.assertEqual(session.player_position, story_position)

    def test_world_registry_rejects_mismatched_orb_binding(self) -> None:
        system = WorldOrbSystem(WorldId(1))
        orb = system.create_orb().value
        self.assertEqual(system.enter(WorldOrb(OrbId(999), orb.world_id), Position()).error, Error.INVALID_ITEM)
        self.assertEqual(system.enter(WorldOrb(orb.id, WorldId(999)), Position()).error, Error.UNKNOWN_WORLD)
        self.assertFalse(system.find_world(orb.world_id).initialized)
        self.assertEqual(system.active_world, WorldId(1))
        result = system.enter(orb, Position(5, 2, 3))
        self.assert_ok(result)
        self.assertTrue(result.value.created)
        self.assertEqual((result.value.from_world, result.value.to_world), (WorldId(1), orb.world_id))
        self.assert_ok(system.return_to_story(Position(9, 0, 2)))
        revisit = system.enter(orb, Position())
        self.assert_ok(revisit)
        self.assertFalse(revisit.value.created)
        self.assertEqual(revisit.value.player_position, Position(9, 0, 2))

    def test_world_registry_skips_story_id_when_allocating_many_orbs(self) -> None:
        system = WorldOrbSystem(WorldId(3))
        for i in range(1, 101):
            orb = system.create_orb().value
            self.assertEqual(orb.id, OrbId(i))
            self.assertNotEqual(orb.world_id, WorldId(3))
            self.assertEqual(system.find_world(orb.world_id).orb_id, orb.id)
        self.assertEqual(len(system.worlds), 100)

    def test_merchant_first_then_boss_still_allocates_unique_orbs_and_worlds(self) -> None:
        session = GameSession(WorldId(2), seed=90)
        session.advance_seconds(600)
        session.update_player_position(session.story_world, session.merchant.position)
        self.assert_ok(session.claim_merchant_gift(ItemKind.WORLD_ORB))
        merchant_orb = session.inventory.orbs[0]
        session.use_world_orb(merchant_orb.id)
        session.return_to_story()
        self.assert_ok(session.notify_boss_defeated())
        session.update_player_position(session.story_world, session.boss.position)
        self.assert_ok(session.collect_drop(DropId(1)))
        boss_orb = session.inventory.orbs[-1]
        self.assertNotEqual(boss_orb.id, merchant_orb.id)
        self.assertEqual(len({boss_orb.world_id, merchant_orb.world_id, session.story_world}), 3)
        self.assert_ok(session.use_world_orb(boss_orb.id))
        self.assertTrue(session.find_creative_world(merchant_orb.world_id).initialized)
        self.assertTrue(session.find_creative_world(boss_orb.world_id).initialized)
        self.assertEqual(len(session.inventory.orbs), 2)

"""验证商人赠品、掉落拾取和背包存储规则。
覆盖商人出现与离开的边界、三维交互距离、两种赠品分别限领和重复拾取保护。
检查小球身份去重、碎片大整数累加，以及非法道具或数量被拒绝后的状态不变性。"""

from random import Random

from game import (
    DropCollected, DropId, Error, GameSession, ItemGranted, ItemKind, MerchantDeparted,
    MerchantPhase, MerchantSpawned, OrbId, Position, WorldId, WorldOrb,
)
from game.game_types import NS_PER_SECOND as S, horizontal_distance
from game.inventory import Inventory
from game.merchant_system import MerchantSystem
from .support import GameTestCase, count_events, snapshot


class MerchantAndInventoryTests(GameTestCase):
    def test_merchant_arrives_at_600_and_leaves_at_780_once(self) -> None:
        session = GameSession(seed=42)
        self.assertEqual(session.claim_merchant_gift(ItemKind.WORLD_ORB).error, Error.MERCHANT_UNAVAILABLE)
        session.advance_ns(600 * S - 1)
        self.assertEqual(session.merchant.phase, MerchantPhase.NOT_SPAWNED)
        session.advance_ns(1)
        self.assertEqual(session.merchant.phase, MerchantPhase.PRESENT)
        position = session.merchant.position
        self.assertTrue(3 <= horizontal_distance(Position(), position) <= 6)
        session.advance_ns(180 * S - 1)
        self.assertEqual(session.merchant.phase, MerchantPhase.PRESENT)
        session.advance_ns(1)
        self.assertEqual(session.merchant.phase, MerchantPhase.DEPARTED)
        session.update_player_position(session.story_world, position)
        self.assertEqual(session.claim_merchant_gift(ItemKind.WORLD_ORB).error, Error.MERCHANT_UNAVAILABLE)
        session.advance_seconds(72 * 3600)
        self.assertEqual(session.merchant.phase, MerchantPhase.DEPARTED)
        self.assertEqual(count_events(session.events, MerchantSpawned), 1)
        self.assertEqual(count_events(session.events, MerchantDeparted), 1)
        self.assertEqual(session.inventory.orbs, ())

    def test_merchant_gifts_can_be_claimed_separately_only_once(self) -> None:
        for first, second in ((ItemKind.WORLD_ORB, ItemKind.WORLD_FRAGMENT),
                              (ItemKind.WORLD_FRAGMENT, ItemKind.WORLD_ORB)):
            session = GameSession(seed=42)
            session.advance_seconds(600)
            p = session.merchant.position
            session.update_player_position(session.story_world, Position(p.x + 3.01, p.y, p.z))
            before = snapshot(session)
            self.assertEqual(session.claim_merchant_gift(first).error, Error.OUT_OF_RANGE)
            self.assertEqual(snapshot(session), before)
            session.update_player_position(session.story_world, p)
            self.assert_ok(session.claim_merchant_gift(first))
            self.assertEqual(session.merchant.orb_claimed, first is ItemKind.WORLD_ORB)
            self.assertEqual(session.merchant.fragment_claimed, first is ItemKind.WORLD_FRAGMENT)
            self.assertEqual(session.claim_merchant_gift(first).error, Error.ALREADY_CLAIMED)
            self.assert_ok(session.claim_merchant_gift(second))
            self.assertEqual(len(session.inventory.orbs), 1)
            self.assertEqual(session.inventory.fragment_count, 1)
            orb = session.inventory.orbs[0]
            self.assertNotEqual(orb.world_id, session.story_world)
            before = snapshot(session)
            self.assertEqual(session.claim_merchant_gift(second).error, Error.ALREADY_CLAIMED)
            for invalid in (99, None, [], 'WORLD_ORB', True):
                self.assertEqual(session.claim_merchant_gift(invalid).error, Error.INVALID_ITEM)
            self.assertEqual(snapshot(session), before)
            self.assertEqual(len(session.creative_worlds), 1)
            self.assertEqual(count_events(session.events, ItemGranted), 2)

    def test_merchant_exact_distance_is_allowed_and_vertical_distance_counts(self) -> None:
        merchant = MerchantSystem()
        random = Random(4)
        self.assert_ok(merchant.spawn(Position(), random))
        p = merchant.state.position
        self.assertEqual(merchant.can_claim(ItemKind.WORLD_ORB, Position(p.x, p.y + 3.01, p.z)).error,
                         Error.OUT_OF_RANGE)
        self.assert_ok(merchant.claim(ItemKind.WORLD_ORB, Position(p.x, p.y + 3, p.z)))
        self.assertFalse(merchant.spawn(Position(), random).ok)
        self.assert_ok(merchant.depart())
        self.assertFalse(merchant.depart().ok)
        self.assertFalse(merchant.spawn(Position(), random).ok)

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

    def test_inventory_rejects_duplicate_identity_and_invalid_fragment_types(self) -> None:
        inventory = Inventory()
        self.assert_ok(inventory.add_orb(WorldOrb(OrbId(1), WorldId(2))))
        for orb in (WorldOrb(OrbId(1), WorldId(3)), WorldOrb(OrbId(2), WorldId(2))):
            self.assertEqual(inventory.add_orb(orb).error, Error.DUPLICATE_ORB)
        for invalid in (None, [], WorldOrb(OrbId(0), WorldId(3)), WorldOrb(OrbId(3), WorldId(0)),
                        WorldOrb(WorldId(3), WorldId(4)), WorldOrb(OrbId(True), WorldId(4))):
            self.assertEqual(inventory.add_orb(invalid).error, Error.INVALID_ITEM)
        self.assertEqual(len(inventory.state.orbs), 1)
        self.assertIsNotNone(inventory.find_orb(OrbId(1)))
        self.assertIsNone(inventory.find_orb(OrbId(9)))
        self.assert_ok(inventory.add_fragments(2**100))
        self.assert_ok(inventory.add_fragments())
        self.assert_ok(inventory.add_fragments(0))
        self.assertEqual(inventory.state.fragment_count, 2**100 + 1)
        before = inventory.state
        for invalid in (-1, 1.5, True, False, None, '1', float('inf'), float('nan')):
            self.assertEqual(inventory.add_fragments(invalid).error, Error.INVALID_ITEM)
            self.assertEqual(inventory.state, before)

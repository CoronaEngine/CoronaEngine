"""验证商人出现、离开、交互范围和一次性赠品。"""

from random import Random

from game import (
    Error, GameSession, ItemGranted, ItemKind, MerchantDeparted,
    MerchantPhase, MerchantSpawned, Position,
)
from game.core.types import NS_PER_SECOND as S, horizontal_distance
from game.systems.merchant import MerchantSystem
from ..support import GameTestCase, count_events, snapshot


class MerchantTests(GameTestCase):
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

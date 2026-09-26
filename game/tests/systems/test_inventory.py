"""验证背包身份去重和碎片数量校验。"""

from game import Error, OrbId, WorldId, WorldOrb
from game.systems.inventory import Inventory
from ..support import GameTestCase


class InventoryTests(GameTestCase):
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

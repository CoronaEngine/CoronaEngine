from __future__ import annotations

import unittest

from editor.plugins.AITool.services.terrain_component_resolver import TerrainComponentResolver


class TerrainComponentResolverTest(unittest.TestCase):
    def test_outdoor_nature_profile_handles_forest_camp_substrate(self) -> None:
        profile = TerrainComponentResolver().derive(
            "\u751f\u6210\u4e00\u4e2a\u7b80\u5355\u68ee\u6797\u8425\u5730\uff0c\u6709\u8349\u5730\u3001\u5929\u7a7a\u3001\u5e10\u7bf7\u3001\u5c0f\u6728\u684c\u3002",
            scene_type="outdoor",
        )

        self.assertEqual(profile.scene_key, "outdoor_nature")
        self.assertEqual(profile.terrain_spec["type"], "outdoor_nature_ground")
        self.assertEqual(profile.terrain_spec["surface"], "grass_with_walkable_clearings")
        self.assertTrue(profile.terrain_spec["walkable"])
        self.assertEqual(profile.terrain_spec["sky"], "open_sky")
        self.assertEqual(profile.boundary_spec["type"], "soft_natural_boundary")
        self.assertNotEqual(profile.boundary_spec.get("material"), "wood")
        self.assertNotEqual(profile.boundary_spec.get("type"), "room_walls")

    def test_specific_profiles_keep_priority_over_generic_outdoor_nature(self) -> None:
        resolver = TerrainComponentResolver()

        yurt = resolver.derive("\u8349\u539f\u8499\u53e4\u5305\u8425\u5730", scene_type="outdoor")
        self.assertEqual(yurt.scene_key, "grassland_yurt")

        market = resolver.derive("\u591c\u665a\u5e7b\u60f3\u96c6\u5e02\uff0c\u6709\u8349\u5730\u8fb9\u7f18\u548c\u706f\u7b3c", scene_type="outdoor")
        self.assertEqual(market.scene_key, "fantasy_night_market")

    def test_indoor_profile_is_not_stolen_by_outdoor_nature_terms(self) -> None:
        profile = TerrainComponentResolver().derive("\u5ba4\u5185\u5367\u5ba4\u5730\u9762", scene_type="indoor")

        self.assertEqual(profile.scene_key, "indoor_room")
        self.assertEqual(profile.boundary_spec["type"], "room_walls")


if __name__ == "__main__":
    unittest.main()

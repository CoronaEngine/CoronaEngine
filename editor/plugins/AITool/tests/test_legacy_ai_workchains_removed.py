import unittest
from pathlib import Path


AITool_ROOT = Path(__file__).resolve().parents[1]
CAI_EXTENSIONS = AITool_ROOT / "cai_extensions"
ENGINE_TOOLS = CAI_EXTENSIONS / "engine_tools.py"


class LegacyAIWorkchainsRemovedTests(unittest.TestCase):
    def test_legacy_scene_planning_and_placement_sources_are_removed(self):
        self.assertFalse((CAI_EXTENSIONS / "scene_plan_tools.py").exists())
        self.assertFalse((CAI_EXTENSIONS / "scene_placement").exists())
        self.assertFalse(
            (CAI_EXTENSIONS / "mcp" / "tools" / "place_object_near.py").exists()
        )

    def test_engine_loader_does_not_register_legacy_scene_workchains(self):
        text = ENGINE_TOOLS.read_text(encoding="utf-8")
        for legacy_reference in (
            "load_scene_plan_tools",
            "scene_placement",
            "load_placement_tools",
            "load_place_object_near_tools",
        ):
            self.assertNotIn(legacy_reference, text)


if __name__ == "__main__":
    unittest.main()

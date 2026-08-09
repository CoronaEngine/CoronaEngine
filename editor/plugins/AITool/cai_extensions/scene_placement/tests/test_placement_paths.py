import sys
import unittest
from pathlib import Path
from unittest.mock import patch


EDITOR_ROOT = Path(__file__).resolve().parents[5]
AI_TOOL_ROOT = EDITOR_ROOT / "plugins" / "AITool"
QUASAR_ROOT = AI_TOOL_ROOT / "Quasar"
for path in (EDITOR_ROOT, AI_TOOL_ROOT, QUASAR_ROOT):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))


class PlacementPathTests(unittest.TestCase):
    def test_placement_tool_does_not_depend_on_engine_project_state(self):
        source = (
            EDITOR_ROOT
            / "plugins"
            / "AITool"
            / "cai_extensions"
            / "scene_placement"
            / "tools"
            / "placement_tools.py"
        ).read_text(encoding="utf-8")
        self.assertNotIn("CoronaEditor.CoronaEngine", source)

    def test_project_root_comes_from_runtime_project_context(self):
        from editor.plugins.AITool.cai_extensions.scene_placement.tools import placement_tools

        with patch(
            "runtime.project_context.get_project_root",
            return_value=Path("D:/Projects/Example"),
        ):
            result = placement_tools._get_project_root()

        self.assertEqual(result, Path("D:/Projects/Example"))


if __name__ == "__main__":
    unittest.main()

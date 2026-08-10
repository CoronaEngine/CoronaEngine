from __future__ import annotations

import unittest

from editor.plugins.AITool.services.agent_runtime.adapters import _runtime_actor_support_type
from editor.plugins.AITool.services.agent_runtime.core import AgentRuntime
from editor.plugins.AITool.services.agent_runtime.support_semantics import classify_support_type
from editor.plugins.AITool.services.agent_runtime.tools import _support_type_for_ground_snap


class AgentRuntimeSupportSemanticsTests(unittest.TestCase):
    def test_child_bedroom_floor_props_share_one_support_classification(self) -> None:
        for name in ("台灯", "玩偶", "书架", "table lamp", "doll", "bookshelf"):
            with self.subTest(name=name):
                self.assertEqual(classify_support_type(name), "floor_supported")
                self.assertEqual(
                    _runtime_actor_support_type({"requested_name": name}),
                    "floor_supported",
                )
                self.assertEqual(
                    AgentRuntime._layout_support_type({"requested_name": name}),
                    "floor_supported",
                )
                self.assertEqual(_support_type_for_ground_snap(name), "floor_supported")

    def test_mounted_light_precedence_prevents_blanket_lamp_grounding(self) -> None:
        self.assertEqual(classify_support_type("儿童壁灯"), "wall_mounted")
        self.assertEqual(classify_support_type("儿童吊灯"), "ceiling_hung")
        self.assertEqual(classify_support_type("wall-mounted bookshelf"), "wall_mounted")
        self.assertEqual(classify_support_type("pendant light"), "ceiling_hung")

    def test_explicit_support_fact_wins_over_name_inference(self) -> None:
        actor = {"requested_name": "书架", "support_type": "wall_mounted"}
        self.assertEqual(_runtime_actor_support_type(actor), "wall_mounted")
        self.assertEqual(AgentRuntime._layout_support_type(actor), "wall_mounted")

    def test_floor_support_domain_still_requires_engine_actual_contact(self) -> None:
        actor = {
            "requested_name": "玩偶",
            "bounds_ready": True,
            "bounds_source": "engine_actual",
            "aabb": {"min": [-0.2, 0.0, -0.2], "max": [0.2, 0.8, 0.2]},
        }
        self.assertEqual(
            AgentRuntime._grounding_status_from_actual_floor_bounds(actor),
            "grounded",
        )

        actor["bounds_source"] = "estimated"
        self.assertEqual(AgentRuntime._grounding_status_from_actual_floor_bounds(actor), "")

        actor["bounds_source"] = "engine_actual"
        actor["aabb"]["min"][1] = 0.4
        self.assertEqual(AgentRuntime._grounding_status_from_actual_floor_bounds(actor), "")

        actor["requested_name"] = "吊灯"
        actor["aabb"]["min"][1] = 0.0
        self.assertEqual(AgentRuntime._grounding_status_from_actual_floor_bounds(actor), "")

    def test_system_and_unknown_entities_remain_non_floor(self) -> None:
        self.assertEqual(classify_support_type("__room_box"), "system")
        self.assertEqual(classify_support_type("room_floor"), "system")
        self.assertEqual(classify_support_type("幻想集市地形边界"), "system")
        self.assertEqual(classify_support_type("抽象装饰元素"), "unknown")


if __name__ == "__main__":
    unittest.main()

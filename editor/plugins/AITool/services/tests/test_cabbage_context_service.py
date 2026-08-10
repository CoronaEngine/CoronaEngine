import json
import pathlib
import sys
import tempfile
import time
import unittest
from unittest import mock

EDITOR_ROOT = pathlib.Path(__file__).resolve().parents[4]
if str(EDITOR_ROOT) not in sys.path:
    sys.path.insert(0, str(EDITOR_ROOT))

from plugins.AITool.services.cabbage_context_service import CabbageContextService
from plugins.AITool.services.node_graph_review_service import NodeGraphReviewService


class CabbageContextServiceTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.world = pathlib.Path(self.temp_dir.name) / "story_world_test"
        self.world.mkdir(parents=True)
        (self.world / "project.ini").write_text("[Project]\nname=story_world_test\n", encoding="utf-8")
        self.service = CabbageContextService()
        self.path_patch = mock.patch.object(
            CabbageContextService,
            "_active_project_path",
            return_value=self.world,
        )
        self.path_patch.start()

    def tearDown(self):
        self.path_patch.stop()
        self.service.shutdown()
        self.temp_dir.cleanup()

    def active_task_keys(self, context):
        return {task.get("taskKey") for task in context.get("activeTasks", [])}

    def history_task(self, context, task_key):
        return next(
            (task for task in context.get("taskHistory", []) if task.get("taskKey") == task_key),
            None,
        )

    def tutorial_tasks(self, context):
        return sorted(
            [task for task in context.get("activeTasks", []) if task.get("type") == "tutorial"],
            key=lambda task: int(task.get("globalOrder") or 0),
        )

    def active_tutorial(self, context):
        active = [
            task for task in self.tutorial_tasks(context)
            if task.get("status") == "active"
        ]
        self.assertEqual(1, len(active), "Exactly one tutorial step must be active")
        return active[0]

    def record(self, event_type, details=None, success=True, timestamp=None):
        payload = {
            "type": event_type,
            "category": "tutorial",
            "success": success,
            "details": details or {},
            "worldId": self.world.name,
        }
        if timestamp is not None:
            payload["timestamp"] = timestamp
        return self.service.record_event(payload)

    @staticmethod
    def tutorial_event_sequence():
        return [
            ("tutorial.basics.viewport_focus", "viewport_focused", {}),
            ("tutorial.basics.camera_forward_back", "camera_moved", {
                "key": "W", "axisGroup": "forward_back", "actualDelta": 1,
            }),
            ("tutorial.basics.camera_left_right", "camera_moved", {
                "key": "A", "axisGroup": "left_right", "actualDelta": 1,
            }),
            ("tutorial.basics.camera_up_down", "camera_moved", {
                "key": "E", "axisGroup": "up_down", "actualDelta": 1,
            }),
            ("tutorial.basics.camera_rotate", "camera_rotated", {
                "interaction": "right_mouse_drag", "actualDelta": 1,
            }),
            ("tutorial.basics.camera_wheel", "camera_moved", {
                "interaction": "wheel", "actualDelta": 1,
            }),
            ("tutorial.basics.open_scene_manager", "panel_opened", {
                "panelId": "SceneTools", "source": "user",
            }),
            ("tutorial.basics.import_model", "model_imported", {
                "sceneName": "Scene", "actorName": "TutorialActor",
                "actorId": "actor-1", "resourcePath": "cache/tutorial.glb",
            }),
            ("tutorial.basics.select_model", "actor_selected", {
                "actorName": "TutorialActor", "actorId": "actor-1", "source": "scene_tree",
            }),
            ("tutorial.basics.set_position_x", "transform_position", {
                "actorName": "TutorialActor", "actorId": "actor-1", "axis": "x", "value": 1,
            }),
            ("tutorial.basics.set_rotation_y", "transform_rotation", {
                "actorName": "TutorialActor", "actorId": "actor-1", "axis": "y", "value": 45,
            }),
            ("tutorial.basics.set_scale_x", "transform_scale", {
                "actorName": "TutorialActor", "actorId": "actor-1", "axis": "x", "value": 1.5,
            }),
            ("tutorial.basics.enable_physics", "physics_changed", {
                "actorName": "TutorialActor", "actorId": "actor-1",
                "operation": "SetPhysicsEnabled", "value": True,
            }),
            ("tutorial.basics.set_mass", "physics_changed", {
                "actorName": "TutorialActor", "actorId": "actor-1",
                "operation": "SetMass", "value": 10,
            }),
            ("tutorial.basics.set_light_x", "lighting_changed", {
                "sceneName": "Scene", "axis": "x", "value": 0.5,
            }),
            ("tutorial.basics.open_nodes", "panel_opened", {
                "panelId": "NodeGraphPanel", "source": "user",
            }),
            ("tutorial.basics.confirm_start_node", "node_selected", {
                "nodeId": "start-1", "nodeType": "start", "uniqueStart": True, "source": "user",
            }),
            ("tutorial.basics.create_custom_node", "node_created", {
                "nodeId": "custom-1", "nodeType": "custom",
            }),
            ("tutorial.basics.select_custom_node", "node_selected", {
                "nodeId": "custom-1", "nodeType": "custom", "mode": "select", "source": "user",
            }),
            ("tutorial.basics.move_custom_node", "node_moved", {
                "nodeId": "custom-1", "actualDelta": 20, "mode": "select", "source": "user",
            }),
            ("tutorial.basics.create_delete_practice_node", "node_created", {
                "nodeId": "delete-practice-1", "nodeType": "custom",
            }),
            ("tutorial.basics.delete_practice_node", "node_deleted", {
                "nodeId": "delete-practice-1", "nodeType": "custom", "mode": "delete", "source": "user",
            }),
            ("tutorial.basics.return_select_tool", "node_tool_mode_changed", {
                "mode": "select", "source": "user",
            }),
            ("tutorial.basics.connect_nodes", "node_connected", {
                "sourceNodeId": "start-1", "targetNodeId": "custom-1", "edgeId": "edge-1",
            }),
            ("tutorial.basics.open_custom_node", "node_selected", {
                "nodeId": "custom-1", "source": "user",
            }),
            ("tutorial.basics.add_when_enter", "block_added", {
                "nodeId": "custom-1", "blockId": "enter-1", "blockType": "node_when_enter",
            }),
            ("tutorial.basics.add_set_position", "block_connected", {
                "nodeId": "custom-1", "blockId": "position-1",
                "blockType": "object_set_position",
                "parentBlockType": "node_when_enter", "connected": True,
            }),
            ("tutorial.basics.set_position_model", "block_parameter_changed", {
                "nodeId": "custom-1", "blockId": "position-1",
                "blockType": "object_set_position", "fieldName": "NAME",
                "parentBlockType": "node_when_enter", "connected": True,
                "newValue": "TutorialActor", "modelName": "TutorialActor",
            }),
            ("tutorial.basics.set_start_x", "block_parameter_changed", {
                "nodeId": "custom-1", "blockId": "position-1",
                "blockType": "object_set_position", "fieldName": "X",
                "parentBlockType": "node_when_enter", "connected": True,
                "newValue": -3, "modelName": "TutorialActor", "x": -3, "y": 0, "z": 0,
            }),
            ("tutorial.basics.add_while_active", "block_added", {
                "nodeId": "custom-1", "blockId": "active-1",
                "blockType": "node_while_active", "parentBlockType": "", "connected": False,
            }),
            ("tutorial.basics.add_move_direction", "block_connected", {
                "nodeId": "custom-1", "blockId": "move-1",
                "blockType": "object_move_direction",
                "parentBlockType": "node_while_active", "connected": True,
            }),
            ("tutorial.basics.set_move_model", "block_parameter_changed", {
                "nodeId": "custom-1", "blockId": "move-1",
                "blockType": "object_move_direction", "fieldName": "NAME",
                "parentBlockType": "node_while_active", "connected": True,
                "newValue": "TutorialActor", "modelName": "TutorialActor",
            }),
            ("tutorial.basics.set_move_direction", "block_parameter_changed", {
                "nodeId": "custom-1", "blockId": "move-1",
                "blockType": "object_move_direction", "fieldName": "DIRECTION",
                "parentBlockType": "node_while_active", "connected": True,
                "newValue": "RIGHT", "direction": "RIGHT", "modelName": "TutorialActor",
            }),
            ("tutorial.basics.set_move_speed", "block_parameter_changed", {
                "nodeId": "custom-1", "blockId": "move-1",
                "blockType": "object_move_direction", "fieldName": "SPEED",
                "parentBlockType": "node_while_active", "connected": True,
                "newValue": 2, "modelName": "TutorialActor",
                "direction": "RIGHT", "speed": 2,
            }),
            ("tutorial.basics.run_graph", "run_clicked", {"source": "user"}),
            ("tutorial.basics.focus_ai_composer", "ai_composer_focused", {
                "source": "user",
            }),
            ("tutorial.basics.ask_ai", "ai_question_answered", {
                "prompt": "Explain why the model starts at X=-3 and moves right; do not modify anything.",
                "mode": "ask", "responseReceived": True,
            }),
            ("tutorial.basics.modify_with_ai", "ai_node_graph_changed", {
                "prompt": "Change the continuous movement speed from 2 to 4 and keep everything else.",
                "mode": "modify", "operation": "edit", "applied": True,
            }),
            ("tutorial.basics.generate_with_ai", "ai_node_graph_changed", {
                "prompt": "Add an End node and connect the Custom node to it.",
                "mode": "generate", "operation": "extend", "applied": True,
                "createdNodeIds": ["ai-end-1"], "createdEdgeIds": ["ai-edge-1"],
            }),
        ]

    def complete_tutorial_steps(self, count):
        response = self.service.load()
        for expected_key, event_type, details in self.tutorial_event_sequence()[:count]:
            self.assertEqual(expected_key, self.active_tutorial(response["context"])["taskKey"])
            response = self.record(event_type, details)
            self.assertTrue(response["success"])
            self.assertEqual([expected_key], response["completedTaskKeys"])
        return response

    def test_new_world_uses_schema_v2_with_four_chapters_and_39_steps(self):
        response = self.service.load()
        self.assertTrue(response["success"])
        context = response["context"]
        tutorials = self.tutorial_tasks(context)

        self.assertEqual(2, context["schemaVersion"])
        self.assertEqual(39, len(tutorials))
        self.assertEqual(
            ["chapter_viewport", "chapter_scene", "chapter_nodes", "chapter_ai"],
            list(dict.fromkeys(task["chapterKey"] for task in tutorials)),
        )
        self.assertEqual(
            {"chapter_viewport": 6, "chapter_scene": 9, "chapter_nodes": 20, "chapter_ai": 4},
            {
                chapter_key: sum(task["chapterKey"] == chapter_key for task in tutorials)
                for chapter_key in {task["chapterKey"] for task in tutorials}
            },
        )
        self.assertEqual(list(range(1, 40)), [task["globalOrder"] for task in tutorials])
        self.assertTrue(all(task["taskKey"].startswith("tutorial.basics.") for task in tutorials))
        self.assertEqual("tutorial.basics.viewport_focus", self.active_tutorial(context)["taskKey"])
        self.assertTrue(all(
            task["status"] == ("active" if task["globalOrder"] == 1 else "queued")
            for task in tutorials
        ))

    def test_tutorial_tasks_are_bilingual_and_do_not_expose_track_or_discipline(self):
        tutorials = self.tutorial_tasks(self.service.load()["context"])
        visible_fields = (
            "chapterTitle", "chapterTitleEn", "chapterSummary", "chapterSummaryEn",
            "title", "titleEn", "message", "messageEn", "suggestion", "suggestionEn",
            "completionCriteria", "completionCriteriaEn",
        )
        forbidden_internal_names = (
            "node_when_enter", "node_while_active", "object_set_position",
            "object_move_direction", "control_wait", "logic_boolean", "run_succeeded",
            "SetPhysicsEnabled", "SECONDS", "DIRECTION", "SPEED",
        )
        self.assertEqual(39, len(CabbageContextService.TUTORIAL_TASKS))
        for task in tutorials:
            with self.subTest(task=task["taskKey"]):
                for field in visible_fields:
                    value = str(task.get(field) or "").strip()
                    self.assertTrue(value, field)
                    self.assertNotIn("???", value)
                    for internal_name in forbidden_internal_names:
                        self.assertNotIn(internal_name, value)
                self.assertNotIn("track", task)
                self.assertNotIn("discipline", task)

    def test_block_add_tasks_name_the_visible_toolbox_category(self):
        tutorials = {
            task["taskKey"]: task
            for task in self.tutorial_tasks(self.service.load()["context"])
        }
        expected_categories = {
            "tutorial.basics.add_when_enter": ("\u4e8b\u4ef6", "Events"),
            "tutorial.basics.add_set_position": ("\u8fd0\u52a8", "Motion"),
            "tutorial.basics.add_while_active": ("\u4e8b\u4ef6", "Events"),
            "tutorial.basics.add_move_direction": ("\u8fd0\u52a8", "Motion"),
        }
        for task_key, (category_zh, category_en) in expected_categories.items():
            with self.subTest(task=task_key):
                task = tutorials[task_key]
                self.assertIn("\u67e5\u627e\u4f4d\u7f6e", task["message"])
                self.assertIn(f"\u201c{category_zh}\u201d\u5206\u7c7b", task["message"])
                self.assertIn("left block toolbox >", task["messageEn"])
                self.assertIn(f"{category_en} category", task["messageEn"])
                self.assertIn(category_zh, task["title"])
                self.assertIn(category_en, task["titleEn"])

    def test_current_tutorial_copy_refreshes_active_and_completed_tasks(self):
        context = self.service._default_context(self.world)
        active = next(
            task for task in context["activeTasks"]
            if task.get("taskKey") == "tutorial.basics.add_when_enter"
        )
        active.update({
            "title": "stale active title",
            "message": "stale active message",
            "track": "programming",
        })

        completed_at = self.service._now_ms() - 5000
        history = next(
            dict(task) for task in context["activeTasks"]
            if task.get("taskKey") == "tutorial.basics.add_set_position"
        )
        history.update({
            "title": "stale history title",
            "message": "stale history message",
            "status": "completed",
            "completedAt": completed_at,
            "updatedAt": completed_at,
            "track": "art",
            "discipline": "art",
            "historyOnlyMarker": "keep-me",
        })
        context["taskHistory"].append(history)
        self.service._write_locked(self.world, context)

        refreshed = self.service.load()["context"]
        refreshed_active = next(
            task for task in refreshed["activeTasks"]
            if task.get("taskKey") == "tutorial.basics.add_when_enter"
        )
        refreshed_history = self.history_task(
            refreshed, "tutorial.basics.add_set_position",
        )

        self.assertIn("\u201c\u4e8b\u4ef6\u201d\u5206\u7c7b", refreshed_active["message"])
        self.assertNotEqual("stale active title", refreshed_active["title"])
        self.assertNotIn("track", refreshed_active)
        self.assertIsNotNone(refreshed_history)
        self.assertIn("\u201c\u8fd0\u52a8\u201d\u5206\u7c7b", refreshed_history["message"])
        self.assertNotEqual("stale history title", refreshed_history["title"])
        self.assertEqual("completed", refreshed_history["status"])
        self.assertEqual(completed_at, refreshed_history["completedAt"])
        self.assertEqual("keep-me", refreshed_history["historyOnlyMarker"])
        self.assertNotIn("track", refreshed_history)
        self.assertNotIn("discipline", refreshed_history)

    def test_only_current_step_can_complete_and_camera_requires_actual_change(self):
        early = self.record("model_imported", {"actorId": "too-early"})
        self.assertEqual([], early["completedTaskKeys"])
        self.assertEqual("tutorial.basics.viewport_focus", self.active_tutorial(early["context"])["taskKey"])

        focused = self.record("viewport_focused")
        self.assertEqual(["tutorial.basics.viewport_focus"], focused["completedTaskKeys"])

        for details in (
            {"key": "W", "axisGroup": "forward_back", "actualDelta": 0},
            {"key": "A", "axisGroup": "forward_back", "actualDelta": 1},
            {"key": "W", "axisGroup": "left_right", "actualDelta": 1},
        ):
            rejected = self.record("camera_moved", details)
            self.assertEqual([], rejected["completedTaskKeys"])
            self.assertEqual(
                "tutorial.basics.camera_forward_back",
                self.active_tutorial(rejected["context"])["taskKey"],
            )

        moved = self.record("camera_moved", {
            "key": "S", "axisGroup": "forward_back", "actualDelta": -0.25,
        })
        self.assertEqual(["tutorial.basics.camera_forward_back"], moved["completedTaskKeys"])

    def test_guidance_open_does_not_complete_panel_tasks(self):
        response = self.complete_tutorial_steps(6)
        self.assertEqual("tutorial.basics.open_scene_manager", self.active_tutorial(response["context"])["taskKey"])
        guided = self.record("panel_opened", {"panelId": "SceneTools", "source": "guidance"})
        self.assertEqual([], guided["completedTaskKeys"])
        opened = self.record("panel_opened", {"panelId": "SceneTools", "source": "user"})
        self.assertEqual(["tutorial.basics.open_scene_manager"], opened["completedTaskKeys"])

    def test_scene_steps_require_bound_actor_axis_operation_and_value(self):
        response = self.complete_tutorial_steps(9)
        self.assertEqual("actor-1", response["context"]["tutorialSession"]["bindings"]["modelActorId"])

        rejected_cases = [
            ("transform_position", {"actorId": "other", "axis": "x", "value": 1}),
            ("transform_position", {"actorId": "actor-1", "axis": "y", "value": 1}),
            ("transform_position", {"actorId": "actor-1", "axis": "x", "value": 1.02}),
        ]
        for event_type, details in rejected_cases:
            rejected = self.record(event_type, details)
            self.assertEqual([], rejected["completedTaskKeys"])
        accepted = self.record("transform_position", {"actorId": "actor-1", "axis": "x", "value": 1.009})
        self.assertEqual(["tutorial.basics.set_position_x"], accepted["completedTaskKeys"])

        rejected = self.record("transform_rotation", {"actorId": "actor-1", "axis": "y", "value": 45.2})
        self.assertEqual([], rejected["completedTaskKeys"])
        accepted = self.record("transform_rotation", {"actorId": "actor-1", "axis": "y", "value": 45.09})
        self.assertEqual(["tutorial.basics.set_rotation_y"], accepted["completedTaskKeys"])

        self.assertEqual([], self.record("transform_scale", {
            "actorId": "actor-1", "axis": "x", "value": 1.52,
        })["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.set_scale_x"], self.record("transform_scale", {
            "actorId": "actor-1", "axis": "x", "value": 1.5,
        })["completedTaskKeys"])

        self.assertEqual([], self.record("physics_changed", {
            "actorId": "actor-1", "operation": "SetMass", "value": True,
        })["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.enable_physics"], self.record("physics_changed", {
            "actorId": "actor-1", "operation": "SetPhysicsEnabled", "value": True,
        })["completedTaskKeys"])

        self.assertEqual([], self.record("physics_changed", {
            "actorId": "actor-1", "operation": "SetMass", "value": 10.02,
        })["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.set_mass"], self.record("physics_changed", {
            "actorId": "actor-1", "operation": "SetMass", "value": 10,
        })["completedTaskKeys"])

        self.assertEqual([], self.record("lighting_changed", {"axis": "y", "value": 0.5})["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.set_light_x"], self.record("lighting_changed", {
            "sceneName": "Scene", "axis": "x", "value": 0.5,
        })["completedTaskKeys"])

    def test_node_and_block_steps_require_bound_entities_and_connections(self):
        response = self.complete_tutorial_steps(17)
        self.assertEqual(
            "tutorial.basics.create_custom_node",
            self.active_tutorial(response["context"])["taskKey"],
        )

        created = self.record("node_created", {"nodeId": "custom-1", "nodeType": "custom"})
        self.assertEqual(["tutorial.basics.create_custom_node"], created["completedTaskKeys"])
        bindings = created["context"]["tutorialSession"]["bindings"]
        self.assertEqual("start-1", bindings["startNodeId"])
        self.assertEqual("custom-1", bindings["customNodeId"])

        for details in (
            {"nodeId": "custom-1", "mode": "select", "source": "creation"},
            {"nodeId": "custom-1", "mode": "delete", "source": "user"},
            {"nodeId": "other", "mode": "select", "source": "user"},
        ):
            self.assertEqual([], self.record("node_selected", details)["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.select_custom_node"], self.record("node_selected", {
            "nodeId": "custom-1", "nodeType": "custom", "mode": "select", "source": "user",
        })["completedTaskKeys"])

        for details in (
            {"nodeId": "other", "actualDelta": 10, "mode": "select", "source": "user"},
            {"nodeId": "custom-1", "actualDelta": 0, "mode": "select", "source": "user"},
            {"nodeId": "custom-1", "actualDelta": 10, "mode": "delete", "source": "user"},
            {"nodeId": "custom-1", "actualDelta": 10, "mode": "select", "source": "guidance"},
        ):
            self.assertEqual([], self.record("node_moved", details)["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.move_custom_node"], self.record("node_moved", {
            "nodeId": "custom-1", "actualDelta": 10, "mode": "select", "source": "user",
        })["completedTaskKeys"])

        self.assertEqual([], self.record("node_created", {
            "nodeId": "custom-1", "nodeType": "custom",
        })["completedTaskKeys"])
        created_delete_node = self.record("node_created", {
            "nodeId": "delete-practice-1", "nodeType": "custom",
        })
        self.assertEqual(
            ["tutorial.basics.create_delete_practice_node"],
            created_delete_node["completedTaskKeys"],
        )
        self.assertEqual(
            "delete-practice-1",
            created_delete_node["context"]["tutorialSession"]["bindings"]["deletePracticeNodeId"],
        )

        for details in (
            {"nodeId": "custom-1", "mode": "delete", "source": "user"},
            {"nodeId": "delete-practice-1", "mode": "select", "source": "user"},
            {"nodeId": "delete-practice-1", "mode": "delete", "source": "guidance"},
        ):
            self.assertEqual([], self.record("node_deleted", details)["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.delete_practice_node"], self.record("node_deleted", {
            "nodeId": "delete-practice-1", "nodeType": "custom", "mode": "delete", "source": "user",
        })["completedTaskKeys"])

        self.assertEqual([], self.record("node_tool_mode_changed", {
            "mode": "select", "source": "guidance",
        })["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.return_select_tool"], self.record(
            "node_tool_mode_changed", {"mode": "select", "source": "user"},
        )["completedTaskKeys"])

        self.assertEqual([], self.record("node_connected", {
            "sourceNodeId": "custom-1", "targetNodeId": "start-1", "edgeId": "wrong",
        })["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.connect_nodes"], self.record("node_connected", {
            "sourceNodeId": "start-1", "targetNodeId": "custom-1", "edgeId": "edge-1",
        })["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.open_custom_node"], self.record("node_selected", {
            "nodeId": "custom-1", "mode": "select", "source": "user",
        })["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.add_when_enter"], self.record("block_added", {
            "nodeId": "custom-1", "blockId": "enter-1", "blockType": "node_when_enter",
        })["completedTaskKeys"])

        for details in (
            {"nodeId": "custom-1", "parentBlockType": "node_when_enter", "connected": False},
            {"nodeId": "custom-1", "parentBlockType": "other_event", "connected": True},
            {"nodeId": "other", "parentBlockType": "node_when_enter", "connected": True},
        ):
            rejected = self.record("block_connected", {
                "blockId": "position-1", "blockType": "object_set_position", **details,
            })
            self.assertEqual([], rejected["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.add_set_position"], self.record("block_connected", {
            "nodeId": "custom-1", "blockId": "position-1",
            "blockType": "object_set_position", "parentBlockType": "node_when_enter",
            "connected": True,
        })["completedTaskKeys"])

        position_base = {
            "nodeId": "custom-1", "blockId": "position-1",
            "blockType": "object_set_position", "parentBlockType": "node_when_enter",
            "connected": True,
        }
        self.assertEqual([], self.record("block_parameter_changed", {
            **position_base, "fieldName": "NAME", "newValue": "OtherActor",
            "modelName": "OtherActor",
        })["completedTaskKeys"])
        self.assertEqual([], self.record("block_parameter_changed", {
            **position_base, "connected": False, "fieldName": "NAME",
            "newValue": "TutorialActor", "modelName": "TutorialActor",
        })["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.set_position_model"], self.record("block_parameter_changed", {
            **position_base, "fieldName": "NAME", "newValue": "TutorialActor",
            "modelName": "TutorialActor",
        })["completedTaskKeys"])

        for details in (
            {"newValue": -2.9, "x": -2.9, "y": 0, "z": 0, "modelName": "TutorialActor"},
            {"newValue": -3, "x": -3, "y": 0, "z": 0, "modelName": "OtherActor"},
            {"newValue": -3, "x": -3, "y": 1, "z": 0, "modelName": "TutorialActor"},
        ):
            rejected = self.record("block_parameter_changed", {
                **position_base, "fieldName": "X", **details,
            })
            self.assertEqual([], rejected["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.set_start_x"], self.record("block_parameter_changed", {
            **position_base, "fieldName": "X", "newValue": -3, "x": -3,
            "y": 0, "z": 0, "modelName": "TutorialActor",
        })["completedTaskKeys"])

        self.assertEqual([], self.record("block_added", {
            "nodeId": "custom-1", "blockId": "active-1", "blockType": "node_while_active",
            "parentBlockType": "node_when_enter", "connected": True,
        })["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.add_while_active"], self.record("block_added", {
            "nodeId": "custom-1", "blockId": "active-1", "blockType": "node_while_active",
            "parentBlockType": "", "connected": False,
        })["completedTaskKeys"])

        for details in (
            {"parentBlockType": "node_while_active", "connected": False},
            {"parentBlockType": "node_when_enter", "connected": True},
        ):
            rejected = self.record("block_connected", {
                "nodeId": "custom-1", "blockId": "move-1",
                "blockType": "object_move_direction", **details,
            })
            self.assertEqual([], rejected["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.add_move_direction"], self.record("block_connected", {
            "nodeId": "custom-1", "blockId": "move-1",
            "blockType": "object_move_direction", "parentBlockType": "node_while_active",
            "connected": True,
        })["completedTaskKeys"])

        move_base = {
            "nodeId": "custom-1", "blockId": "move-1",
            "blockType": "object_move_direction", "parentBlockType": "node_while_active",
            "connected": True,
        }
        self.assertEqual([], self.record("block_parameter_changed", {
            **move_base, "fieldName": "NAME", "newValue": "OtherActor", "modelName": "OtherActor",
        })["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.set_move_model"], self.record("block_parameter_changed", {
            **move_base, "fieldName": "NAME", "newValue": "TutorialActor", "modelName": "TutorialActor",
        })["completedTaskKeys"])

        self.assertEqual([], self.record("block_parameter_changed", {
            **move_base, "fieldName": "DIRECTION", "newValue": "LEFT",
            "direction": "LEFT", "modelName": "TutorialActor",
        })["completedTaskKeys"])
        self.assertEqual([], self.record("block_parameter_changed", {
            **move_base, "fieldName": "DIRECTION", "newValue": "RIGHT",
            "direction": "RIGHT", "modelName": "OtherActor",
        })["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.set_move_direction"], self.record("block_parameter_changed", {
            **move_base, "fieldName": "DIRECTION", "newValue": "RIGHT",
            "direction": "RIGHT", "modelName": "TutorialActor",
        })["completedTaskKeys"])

        for details in (
            {"newValue": 1.9, "speed": 1.9, "direction": "RIGHT", "modelName": "TutorialActor"},
            {"newValue": 2, "speed": 2, "direction": "LEFT", "modelName": "TutorialActor"},
            {"newValue": 2, "speed": 2, "direction": "RIGHT", "modelName": "OtherActor"},
        ):
            rejected = self.record("block_parameter_changed", {
                **move_base, "fieldName": "SPEED", **details,
            })
            self.assertEqual([], rejected["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.set_move_speed"], self.record("block_parameter_changed", {
            **move_base, "fieldName": "SPEED", "newValue": 2, "speed": 2,
            "direction": "RIGHT", "modelName": "TutorialActor",
        })["completedTaskKeys"])
        self.assertEqual(
            "tutorial.basics.run_graph",
            self.active_tutorial(self.service.load()["context"])["taskKey"],
        )

    def test_node_steps_can_reconcile_existing_ui_state_in_order(self):
        response = self.complete_tutorial_steps(16)
        self.assertEqual(
            "tutorial.basics.confirm_start_node",
            self.active_tutorial(response["context"])["taskKey"],
        )

        wrong_task = self.record("tutorial_node_state_observed", {
            "observedTaskKey": "tutorial.basics.create_custom_node",
            "nodeId": "start-1", "nodeType": "start",
            "startNodeCount": 1, "uniqueStart": True,
            "source": "state_observation",
        })
        self.assertEqual([], wrong_task["completedTaskKeys"])

        completed = self.record("tutorial_node_state_observed", {
            "observedTaskKey": "tutorial.basics.confirm_start_node",
            "source": "state_observation",
            "nodeId": "start-1", "nodeType": "start", "startNodeCount": 1,
            "uniqueStart": True, "createdByTutorial": False,
        })
        self.assertEqual(["tutorial.basics.confirm_start_node"], completed["completedTaskKeys"])
        self.assertFalse(completed["context"]["tutorialSession"]["bindings"]["startNodeCreatedByTutorial"])

        completed = self.record("tutorial_node_state_observed", {
            "observedTaskKey": "tutorial.basics.create_custom_node",
            "source": "state_observation", "nodeId": "custom-1", "nodeType": "custom",
        })
        self.assertEqual(["tutorial.basics.create_custom_node"], completed["completedTaskKeys"])

        self.assertEqual([], self.record("tutorial_node_state_observed", {
            "observedTaskKey": "tutorial.basics.select_custom_node",
            "nodeId": "custom-1", "mode": "select", "source": "state_observation",
        })["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.select_custom_node"], self.record("node_selected", {
            "nodeId": "custom-1", "mode": "select", "source": "user",
        })["completedTaskKeys"])

        self.assertEqual([], self.record("tutorial_node_state_observed", {
            "observedTaskKey": "tutorial.basics.move_custom_node",
            "nodeId": "custom-1", "actualDelta": 20, "mode": "select",
            "source": "state_observation",
        })["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.move_custom_node"], self.record("node_moved", {
            "nodeId": "custom-1", "actualDelta": 20, "mode": "select", "source": "user",
        })["completedTaskKeys"])

        self.assertEqual(["tutorial.basics.create_delete_practice_node"], self.record(
            "node_created", {"nodeId": "delete-practice-1", "nodeType": "custom"},
        )["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.delete_practice_node"], self.record("node_deleted", {
            "nodeId": "delete-practice-1", "mode": "delete", "source": "user",
        })["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.return_select_tool"], self.record(
            "node_tool_mode_changed", {"mode": "select", "source": "user"},
        )["completedTaskKeys"])

        remaining_observations = [
            ("tutorial.basics.connect_nodes", {
                "edgeId": "edge-1", "sourceNodeId": "start-1", "targetNodeId": "custom-1",
            }),
            ("tutorial.basics.open_custom_node", {
                "nodeId": "custom-1",
            }),
            ("tutorial.basics.add_when_enter", {
                "nodeId": "custom-1", "blockId": "enter-1", "blockType": "node_when_enter",
                "workspaceRole": "node",
            }),
            ("tutorial.basics.add_set_position", {
                "nodeId": "custom-1", "blockId": "position-1", "blockType": "object_set_position",
                "parentBlockType": "node_when_enter", "connected": True, "workspaceRole": "node",
            }),
            ("tutorial.basics.set_position_model", {
                "nodeId": "custom-1", "blockId": "position-1", "blockType": "object_set_position",
                "parentBlockType": "node_when_enter", "connected": True, "fieldName": "NAME",
                "newValue": "TutorialActor", "value": "TutorialActor", "modelName": "TutorialActor",
            }),
            ("tutorial.basics.set_start_x", {
                "nodeId": "custom-1", "blockId": "position-1", "blockType": "object_set_position",
                "parentBlockType": "node_when_enter", "connected": True, "fieldName": "X",
                "newValue": -3, "value": -3, "x": -3, "y": 0, "z": 0,
                "modelName": "TutorialActor",
            }),
            ("tutorial.basics.add_while_active", {
                "nodeId": "custom-1", "blockId": "active-1", "blockType": "node_while_active",
                "parentBlockType": "", "connected": False, "workspaceRole": "node",
            }),
            ("tutorial.basics.add_move_direction", {
                "nodeId": "custom-1", "blockId": "move-1", "blockType": "object_move_direction",
                "parentBlockType": "node_while_active", "connected": True, "workspaceRole": "node",
            }),
            ("tutorial.basics.set_move_model", {
                "nodeId": "custom-1", "blockId": "move-1", "blockType": "object_move_direction",
                "parentBlockType": "node_while_active", "connected": True, "fieldName": "NAME",
                "newValue": "TutorialActor", "value": "TutorialActor", "modelName": "TutorialActor",
            }),
            ("tutorial.basics.set_move_direction", {
                "nodeId": "custom-1", "blockId": "move-1", "blockType": "object_move_direction",
                "parentBlockType": "node_while_active", "connected": True, "fieldName": "DIRECTION",
                "newValue": "RIGHT", "value": "RIGHT", "direction": "RIGHT",
                "modelName": "TutorialActor",
            }),
            ("tutorial.basics.set_move_speed", {
                "nodeId": "custom-1", "blockId": "move-1", "blockType": "object_move_direction",
                "parentBlockType": "node_while_active", "connected": True, "fieldName": "SPEED",
                "newValue": 2, "value": 2, "speed": 2, "direction": "RIGHT",
                "modelName": "TutorialActor",
            }),
        ]
        for expected_key, details in remaining_observations:
            completed = self.record("tutorial_node_state_observed", {
                "observedTaskKey": expected_key, "source": "state_observation", **details,
            })
            self.assertEqual([expected_key], completed["completedTaskKeys"])

        self.assertEqual(
            "tutorial.basics.run_graph",
            self.active_tutorial(completed["context"])["taskKey"],
        )

    def test_tutorial_baseline_capture_merges_late_sections_without_overwriting(self):
        first = self.record("tutorial_baseline_captured", {
            "baselineJson": json.dumps({
                "cameraState": {"position": [1, 2, 3]},
                "panels": {"NodeGraphPanel": {"open": False}},
            }),
        })
        self.assertEqual([], first["completedTaskKeys"])

        second = self.record("tutorial_baseline_captured", {
            "baselineJson": json.dumps({
                "cameraState": {"position": [9, 9, 9], "forward": [0, 0, 1]},
                "nodeGraph": {
                    "targetKey": "project:node-graph",
                    "nodeIds": ["existing-start"],
                    "edgeIds": [],
                    "selectedKind": "node",
                    "selectedId": "existing-start",
                },
            }),
        })
        baseline = second["context"]["tutorialSession"]["baseline"]
        self.assertEqual([1, 2, 3], baseline["cameraState"]["position"])
        self.assertEqual([0, 0, 1], baseline["cameraState"]["forward"])
        self.assertFalse(baseline["panels"]["NodeGraphPanel"]["open"])
        self.assertEqual(["existing-start"], baseline["nodeGraph"]["nodeIds"])
        self.assertEqual("existing-start", baseline["nodeGraph"]["selectedId"])

    def test_run_and_ai_completion_keeps_tutorial_content_and_never_restores(self):
        response = self.complete_tutorial_steps(34)
        self.assertEqual("tutorial.basics.run_graph", self.active_tutorial(response["context"])["taskKey"])
        self.assertEqual([], self.record("run_started")["completedTaskKeys"])
        self.assertEqual([], self.record("run_succeeded")["completedTaskKeys"])
        self.assertEqual([], self.record("run_clicked", {"source": "guidance"})["completedTaskKeys"])
        self.assertEqual(["tutorial.basics.run_graph"], self.record(
            "run_clicked", {"source": "user"},
        )["completedTaskKeys"])

        after_run = self.service.load()
        self.assertEqual("active", after_run["context"]["tutorialSession"]["status"])
        self.assertEqual(
            "tutorial.basics.focus_ai_composer",
            self.active_tutorial(after_run["context"])["taskKey"],
        )
        self.assertEqual(35, len([
            task for task in after_run["context"]["taskHistory"]
            if task.get("taskKey", "").startswith("tutorial.basics.")
        ]))
        self.assertEqual([], self.record("preview_started", {"status": "running"})["completedTaskKeys"])
        self.assertEqual([], self.record("preview_stopped", {
            "status": "stopped", "restored": True,
        })["completedTaskKeys"])

        self.assertEqual([], self.record("ai_composer_focused", {"source": "guidance"})["completedTaskKeys"])
        self.assertEqual(
            ["tutorial.basics.focus_ai_composer"],
            self.record("ai_composer_focused", {"source": "user"})["completedTaskKeys"],
        )
        self.assertEqual([], self.record("ai_question_answered", {
            "prompt": "", "mode": "ask", "responseReceived": True,
        })["completedTaskKeys"])
        self.assertEqual(
            ["tutorial.basics.ask_ai"],
            self.record("ai_question_answered", {
                "prompt": "Explain why the model moves right. Explain only.",
                "mode": "ask", "responseReceived": True,
            })["completedTaskKeys"],
        )
        self.assertEqual([], self.record("ai_node_graph_changed", {
            "prompt": "Change the movement speed.", "mode": "modify",
            "operation": "extend", "applied": True,
        })["completedTaskKeys"])
        self.assertEqual(
            ["tutorial.basics.modify_with_ai"],
            self.record("ai_node_graph_changed", {
                "prompt": "Change the continuous rightward movement speed from 2 to 4.",
                "mode": "modify", "operation": "edit", "applied": True,
            })["completedTaskKeys"],
        )
        self.assertEqual([], self.record("ai_node_graph_changed", {
            "prompt": "Add an End node.", "mode": "generate", "operation": "create", "applied": True,
        })["completedTaskKeys"])

        completed_at = 1_900_000_000_000
        finished = self.record("ai_node_graph_changed", {
            "prompt": "Add an End node and keep the current graph.",
            "mode": "generate", "operation": "extend", "applied": True,
            "createdNodeIds": ["ai-end-1"], "createdEdgeIds": ["ai-edge-1"],
        }, timestamp=completed_at)
        self.assertEqual(["tutorial.basics.generate_with_ai"], finished["completedTaskKeys"])
        session = finished["context"]["tutorialSession"]
        self.assertEqual("completed", session["status"])
        self.assertEqual(completed_at, session["completedAt"])
        self.assertEqual(completed_at + 15000, session["completionNoticeExpiresAt"])
        self.assertEqual({}, session["baseline"])
        self.assertEqual([], session["modificationLog"])
        self.assertEqual([], self.tutorial_tasks(finished["context"]))
        self.assertEqual(39, len([
            task for task in finished["context"]["taskHistory"]
            if task.get("taskKey", "").startswith("tutorial.basics.")
        ]))
        bindings = session["bindings"]
        self.assertEqual("actor-1", bindings["modelActorId"])
        self.assertEqual("custom-1", bindings["customNodeId"])
        self.assertEqual("delete-practice-1", bindings["deletePracticeNodeId"])

        for legacy_event in (
            "tutorial_restore_failed",
            "tutorial_restore_retry_requested",
            "tutorial_restore_succeeded",
        ):
            legacy = self.record(legacy_event, {"error": "must not restore"})
            legacy_session = legacy["context"]["tutorialSession"]
            self.assertEqual("completed", legacy_session["status"])
            self.assertEqual(completed_at, legacy_session["completedAt"])
            self.assertEqual(completed_at + 15000, legacy_session["completionNoticeExpiresAt"])

        dismissed = self.record("tutorial_completion_notice_dismissed")
        self.assertEqual(0, dismissed["context"]["tutorialSession"]["completionNoticeExpiresAt"])

    def test_full_39_step_sequence_preserves_chapter_history_and_bindings(self):
        response = self.complete_tutorial_steps(39)
        context = response["context"]
        history = [
            task for task in context["taskHistory"]
            if task.get("taskKey", "").startswith("tutorial.basics.")
        ]
        self.assertEqual(
            [key for key, _event_type, _details in self.tutorial_event_sequence()],
            [task["taskKey"] for task in history],
        )
        self.assertTrue(all(int(task.get("completedAt") or 0) > 0 for task in history))
        self.assertEqual([1, 2, 3, 4], list(dict.fromkeys(task["chapterOrder"] for task in history)))
        bindings = context["tutorialSession"]["bindings"]
        self.assertEqual("actor-1", bindings["modelActorId"])
        self.assertEqual("start-1", bindings["startNodeId"])
        self.assertEqual("custom-1", bindings["customNodeId"])
        self.assertEqual("delete-practice-1", bindings["deletePracticeNodeId"])
        self.assertEqual("edge-1", bindings["edgeId"])
        self.assertEqual("enter-1", bindings["whenEnterBlockId"])
        self.assertEqual("position-1", bindings["setPositionBlockId"])
        self.assertEqual("active-1", bindings["whileActiveBlockId"])
        self.assertEqual("move-1", bindings["moveDirectionBlockId"])

    def test_existing_v2_tutorial_receives_template_copy_and_legacy_tasks_retire(self):
        context = self.service._default_context(self.world)
        current = next(
            task for task in context["activeTasks"]
            if task.get("taskKey") == "tutorial.basics.create_custom_node"
        )
        for field in (
            "chapterTitleEn", "chapterSummaryEn", "titleEn", "messageEn",
            "suggestionEn", "completionCriteriaEn",
        ):
            current.pop(field, None)
        context["activeTasks"].append({
            "taskKey": "tutorial.rotate_model",
            "type": "tutorial",
            "track": "scene",
            "status": "active",
            "createdAt": 1,
            "updatedAt": 1,
        })
        context["activeTasks"].append({
            "taskKey": "tutorial.basics.start_preview",
            "type": "tutorial",
            "chapterKey": "chapter_preview",
            "status": "queued",
            "createdAt": 1,
            "updatedAt": 1,
        })
        context["activeTasks"].append({
            "taskKey": "tutorial.basics.choose_select_tool",
            "type": "tutorial",
            "chapterKey": "chapter_nodes",
            "status": "queued",
            "createdAt": 1,
            "updatedAt": 1,
        })
        context["taskHistory"].append({
            "taskKey": "tutorial.create_node",
            "type": "tutorial",
            "status": "completed",
            "completedAt": 10,
        })
        context_path = self.service._context_path(self.world)
        context_path.parent.mkdir(parents=True, exist_ok=True)
        context_path.write_text(json.dumps(context, ensure_ascii=False), encoding="utf-8")

        loaded = self.service.load()["context"]
        self.assertNotIn("tutorial.rotate_model", self.active_task_keys(loaded))
        self.assertNotIn("tutorial.basics.start_preview", self.active_task_keys(loaded))
        self.assertNotIn("tutorial.basics.choose_select_tool", self.active_task_keys(loaded))
        self.assertIsNotNone(self.history_task(loaded, "tutorial.create_node"))
        migrated = next(
            task for task in loaded["activeTasks"]
            if task.get("taskKey") == "tutorial.basics.create_custom_node"
        )
        template = self.service._tutorial_templates()["tutorial.basics.create_custom_node"]
        for field in (
            "chapterTitleEn", "chapterSummaryEn", "titleEn", "messageEn",
            "suggestionEn", "completionCriteriaEn",
        ):
            self.assertEqual(template[field], migrated[field])

    def test_node_issue_task_preserves_english_fields(self):
        response = self.service.update_task({
            "action": "upsert",
            "worldId": self.world.name,
            "task": {
                "taskKey": "bilingual.issue",
                "type": "node-issue",
                "title": "\u4e2d\u6587\u6807\u9898",
                "titleEn": "English Title",
                "message": "\u4e2d\u6587\u539f\u56e0",
                "messageEn": "English Cause",
                "suggestion": "\u4e2d\u6587\u5efa\u8bae",
                "suggestionEn": "English Suggestion",
                "completionCriteria": "\u4e2d\u6587\u6807\u51c6",
                "completionCriteriaEn": "English Criteria",
            },
        })
        task = next(item for item in response["context"]["activeTasks"] if item["taskKey"] == "bilingual.issue")
        self.assertEqual("English Title", task["titleEn"])
        self.assertEqual("English Cause", task["messageEn"])
        self.assertEqual("English Suggestion", task["suggestionEn"])
        self.assertEqual("English Criteria", task["completionCriteriaEn"])

    def test_node_issue_task_without_english_copy_keeps_english_fields_blank(self):
        response = self.service.update_task({
            "action": "upsert",
            "worldId": self.world.name,
            "task": {
                "taskKey": "legacy.chinese.issue",
                "type": "node-issue",
                "title": "\u4e2d\u6587\u6807\u9898",
                "message": "\u4e2d\u6587\u539f\u56e0",
                "messageEn": "\u4e2d\u6587\u539f\u56e0",
                "suggestion": "\u4e2d\u6587\u5efa\u8bae",
                "completionCriteria": "\u4e2d\u6587\u6807\u51c6",
                "completionCriteriaEn": "\u4e2d\u6587\u6807\u51c6",
            },
        })
        task = next(
            item for item in response["context"]["activeTasks"]
            if item["taskKey"] == "legacy.chinese.issue"
        )
        self.assertEqual("", task["titleEn"])
        self.assertEqual("", task["messageEn"])
        self.assertEqual("", task["suggestionEn"])
        self.assertEqual("", task["completionCriteriaEn"])

    def test_first_score_update_clamps_and_persists_model_score(self):
        context = self.service.load()["context"]
        settings = type("Settings", (), {"api_key": "configured"})()
        response_text = json.dumps({
            "score": 126,
            "reasonCodes": ["fast_issue_resolution"],
        })
        with mock.patch.object(NodeGraphReviewService, "_resolve_settings", return_value=settings), \
             mock.patch.object(NodeGraphReviewService, "_call_deepseek", return_value=response_text):
            result = self.service._compute_score(self.world, context)

        self.assertTrue(result["success"])
        self.assertEqual(100, result["profile"]["score"])
        persisted = self.service.load()["context"]["profile"]
        self.assertEqual(100, persisted["score"])
        self.assertNotIn("role", persisted)
        self.assertNotIn("fluencyTier", persisted)

    def test_later_score_can_rise_with_recent_performance(self):
        context = self.service.load()["context"]
        context["profile"].update({"score": 20, "updatedAt": 1000})
        self.service._write_locked(self.world, context)
        settings = type("Settings", (), {"api_key": "configured"})()
        with mock.patch.object(NodeGraphReviewService, "_resolve_settings", return_value=settings), \
             mock.patch.object(NodeGraphReviewService, "_call_deepseek", return_value='{"score":100,"reasonCodes":["recent_success"]}'):
            result = self.service._compute_score(self.world, context)

        self.assertEqual(72, result["profile"]["score"])
        history = self.service.load()["context"]["profileHistory"]
        self.assertEqual(20, history[-1]["score"])

    def test_later_score_can_fall_with_recent_failures(self):
        context = self.service.load()["context"]
        context["profile"].update({"score": 80, "updatedAt": 1000})
        self.service._write_locked(self.world, context)
        settings = type("Settings", (), {"api_key": "configured"})()
        with mock.patch.object(NodeGraphReviewService, "_resolve_settings", return_value=settings), \
             mock.patch.object(NodeGraphReviewService, "_call_deepseek", return_value='{"score":20,"reasonCodes":["recent_failures"]}'):
            result = self.service._compute_score(self.world, context)

        self.assertEqual(41, result["profile"]["score"])
        history = self.service.load()["context"]["profileHistory"]
        self.assertEqual(80, history[-1]["score"])

    def test_legacy_profile_is_migrated_to_score_only(self):
        context = self.service._default_context(self.world)
        context["profile"] = {
            "role": "programmer",
            "confidence": 0.9,
            "fluencyScore": 68,
            "fluencyTier": "intermediate",
            "fluencyReasonCodes": ["legacy_reason"],
            "updatedAt": 123,
        }
        context_path = self.service._context_path(self.world)
        context_path.parent.mkdir(parents=True, exist_ok=True)
        context_path.write_text(json.dumps(context, ensure_ascii=False), encoding="utf-8")

        profile = self.service.load()["context"]["profile"]
        self.assertEqual(68, profile["score"])
        self.assertEqual(["legacy_reason"], profile["reasonCodes"])
        self.assertNotIn("role", profile)
        self.assertNotIn("confidence", profile)
        self.assertNotIn("fluencyTier", profile)

    def test_repeated_issue_and_related_chat_are_remembered(self):
        task = {
            "action": "upsert",
            "worldId": self.world.name,
            "task": {
                "taskKey": "missing_actor_target|start|move",
                "type": "node-issue",
                "code": "missing_actor_target",
                "nodeId": "start",
                "blockId": "move",
                "title": "Missing actor",
            },
        }
        self.service.update_task(task)
        self.service.append_message({
            "worldId": self.world.name,
            "role": "user",
            "content": "How should this actor reference be connected?",
            "taskKey": task["task"]["taskKey"],
        })
        self.service.append_message({
            "worldId": self.world.name,
            "role": "assistant",
            "content": "Connect an object-reference block.",
            "taskKey": task["task"]["taskKey"],
        })
        self.service.update_task({**task, "action": "resolve"})
        self.service.update_task(task)

        memory = self.service.load()["context"]["issueMemory"]["missing_actor_target"]
        self.assertEqual(2, memory["occurrences"])
        self.assertEqual(1, memory["resolvedCount"])
        self.assertEqual(1, memory["chatDiscussionCount"])

    def test_late_event_from_another_world_is_rejected(self):
        response = self.service.record_event({
            "type": "node_moved",
            "success": True,
            "worldId": "another_world",
        })
        self.assertFalse(response["success"])
        self.assertEqual("INVALID_CONTEXT_EVENT", response["error"])
        self.assertIn("其他世界", response["message"])


    def test_node_issue_persists_edge_pattern_without_double_counting_active_upsert(self):
        payload = {
            "action": "upsert",
            "worldId": self.world.name,
            "task": {
                "taskKey": "invalid_edge_endpoint|start||edge_1",
                "type": "node-issue",
                "code": "invalid_edge_endpoint",
                "nodeId": "start",
                "edgeId": "edge_1",
                "pattern": {
                    "relationType": "transition",
                    "edgeId": "edge_1",
                },
                "title": "repair edge",
            },
        }
        first = self.service.update_task(payload)
        second = self.service.update_task(payload)
        task = next(item for item in second["context"]["activeTasks"] if item["taskKey"] == payload["task"]["taskKey"])
        memory = second["context"]["issueMemory"]["invalid_edge_endpoint"]
        self.assertEqual("edge_1", task["edgeId"])
        self.assertEqual("transition", task["pattern"]["relationType"])
        self.assertEqual("edge_1", task["pattern"]["edgeId"])
        self.assertEqual(1, memory["occurrences"])
        self.assertEqual(1, first["context"]["issueMemory"]["invalid_edge_endpoint"]["occurrences"])

    def test_assistant_showcase_metadata_is_persisted_and_invalid_intent_is_rejected(self):
        valid = self.service.append_message({
            "worldId": self.world.name,
            "role": "assistant",
            "content": "Follow these steps.",
            "needsShowcase": True,
            "guidanceIntent": "connect_object_reference",
            "steps": ["Open Node Dock", "repair edge"],
        })
        self.assertTrue(valid["message"]["needsShowcase"])
        self.assertEqual("connect_object_reference", valid["message"]["guidanceIntent"])
        self.assertEqual(["Open Node Dock", "repair edge"], valid["message"]["steps"])

        invalid = self.service.append_message({
            "worldId": self.world.name,
            "role": "assistant",
            "content": "Reject unknown guidance.",
            "needsShowcase": True,
            "guidanceIntent": "querySelector:anything",
            "steps": ["Unknown action"],
        })
        self.assertFalse(invalid["message"]["needsShowcase"])
        self.assertEqual("", invalid["message"]["guidanceIntent"])


    @staticmethod
    def goal_plan_payload():
        effects = [
            {
                "effectId": "player_move_jump",
                "title": "player movement and jump",
                "description": "the player moves with WASD and jumps between platforms",
                "trigger": "keyboard input",
                "outcome": "the player changes position and can leave the ground",
                "recommendedBlockTypes": ["object_third_person_move", "object_arcade_jump"],
                "verification": "the player can move and jump after running the graph",
            },
            {
                "effectId": "rabbit_behavior",
                "title": "rabbit behavior",
                "description": "the rabbit changes its position inside the play area",
                "trigger": "gameplay update",
                "outcome": "the rabbit appears to roam",
                "recommendedBlockTypes": ["object_set_random_position"],
                "verification": "the rabbit changes position in the allowed area",
            },
            {
                "effectId": "mechanism_collision",
                "title": "mechanism collision",
                "description": "the mechanism participates in logical collision",
                "trigger": "player contact",
                "outcome": "the mechanism can be used by collision gameplay",
                "recommendedBlockTypes": ["object_set_logical_collision"],
                "verification": "the node graph runs with collision behavior enabled",
            },
        ]
        task_specs = [
            ("player_move_jump", "node_created", []),
            ("player_move_jump", "block_added", ["object_third_person_move"]),
            ("player_move_jump", "block_added", ["object_arcade_jump"]),
            ("rabbit_behavior", "block_added", ["object_set_random_position"]),
            ("mechanism_collision", "block_added", ["object_set_logical_collision"]),
            ("mechanism_collision", "run_succeeded", []),
        ]
        return {
            "logicBlueprint": {
                "worldSummary": "an ink-style fantasy exploration world",
                "coreLoop": "move and jump across platforms, observe the rabbit, and avoid mechanisms",
                "requiredActors": ["player", "rabbit", "mechanism", "platform"],
                "nodeEffects": effects,
                "flow": ["initialize", "move and jump", "rabbit behavior", "collision", "run verification"],
            },
            "tasks": [
                {
                    "phase": "node-logic",
                    "effectId": effect_id,
                    "title": f"goal step {index}",
                    "titleEn": f"Goal Step {index}",
                    "message": f"build gameplay effect {effect_id}",
                    "messageEn": f"Build gameplay effect {effect_id}",
                    "suggestion": f"finish the {effect_id} node logic",
                    "suggestionEn": f"Finish the {effect_id} node logic",
                    "completionCriteria": f"signal {signal} is observed for the requested effect",
                    "completionCriteriaEn": f"Signal {signal} is observed for the requested effect",
                    "completionSignal": signal,
                    "requiredBlockTypes": block_types,
                }
                for index, (effect_id, signal, block_types) in enumerate(task_specs, start=1)
            ],
        }

    def test_empty_world_prompt_keeps_default_tutorial_tasks(self):
        response = self.service.start_goal_plan({"prompt": "", "mode": "story"})
        self.assertTrue(response["success"])
        self.assertEqual("completed", response["status"])
        context = response["context"]
        self.assertEqual("default", context["worldGoal"]["source"])
        self.assertTrue(any(task.get("type") == "tutorial" for task in context["activeTasks"]))
        self.assertFalse(any(task.get("type") == "goal" for task in context["activeTasks"]))

    def test_project_goal_metadata_uses_native_project_settings(self):
        with mock.patch(
            "plugins.AITool.services.cabbage_context_service.CoronaEditorApi.project_settings.get_active_project_info",
            return_value={
                "project_path": str(self.world),
                "prompt": "a native project goal",
                "mode": "creative",
            },
        ):
            prompt, mode = self.service._project_goal_metadata(self.world)

        self.assertEqual("a native project goal", prompt)
        self.assertEqual("creative", mode)

    def test_active_project_path_accepts_portable_scene_without_project_ini(self):
        portable = pathlib.Path(self.temp_dir.name) / "portable_scene"
        portable.mkdir()
        portable.joinpath("scene.ini").write_text(
            "[format]\ntype=corona_scene_folder\n", encoding="utf-8"
        )

        self.path_patch.stop()
        try:
            with mock.patch(
                "plugins.AITool.services.cabbage_context_service.get_active_project_path",
                return_value=str(portable),
            ):
                self.assertEqual(portable.resolve(), self.service._active_project_path())
        finally:
            self.path_patch.start()

    def test_context_load_rejects_a_different_world_id(self):
        response = self.service.load({"worldId": "another_world"})
        self.assertFalse(response["success"])
        self.assertEqual("CONTEXT_LOAD_FAILED", response["error"])
        self.assertFalse(self.service._context_path(self.world).exists())

    def test_goal_plan_rejects_a_different_world_id_without_writing_context(self):
        response = self.service.start_goal_plan({
            "worldId": "another_world",
            "prompt": "a world that must not leak into this project",
            "mode": "story",
        })
        self.assertFalse(response["success"])
        self.assertEqual("GOAL_PLAN_START_FAILED", response["error"])
        self.assertFalse(self.service._context_path(self.world).exists())

    def test_goal_plan_prompt_only_requests_personalized_guidance_tasks(self):
        description = "一间会随音乐改变颜色的抽象几何空间"
        prompt = self.service._goal_plan_prompt(description, "story")
        self.assertIn("titleEn", prompt)
        self.assertIn("completionCriteriaEn", prompt)
        self.assertIn("个性化搭建任务", prompt)
        self.assertIn("不能直接修改当前节点区", prompt)
        self.assertIn("不是替用户生成一套节点积木", prompt)
        self.assertIn("一步一步引导用户亲自完成世界", prompt)
        self.assertIn("每个任务只能对应一个可由 completionSignal 验证的操作目标", prompt)
        self.assertIn("不要把光照和物理", prompt)
        self.assertIn("任意语言、任意题材、任意详细程度", prompt)
        self.assertIn("不得使用固定题材模板或按关键词套用预制任务", prompt)
        self.assertIn(
            json.dumps(
                {"mode": "story", "description": description},
                ensure_ascii=False,
                separators=(",", ":"),
            ),
            prompt,
        )
        self.assertNotIn('"effectId":"player_move_jump"', prompt)
        self.assertNotIn(
            '"recommendedBlockTypes":["object_third_person_move"]',
            prompt,
        )

    def test_goal_plan_prompt_preserves_arbitrary_world_descriptions(self):
        descriptions = [
            "一个可以控制飞船采集陨石的太空世界",
            "一间可以开关灯和移动家具的房间",
            "暴雨中的赛车计时挑战",
            "抽象几何音乐可视化空间",
            "No characters; only fog, sound, and a slowly changing light.\nKeep it calm.",
            '包含引号“测试”、符号 #[]{} 与完全自定义名词 Zeta-42',
        ]
        for description in descriptions:
            with self.subTest(description=description):
                prompt = self.service._goal_plan_prompt(description, "creative")
                request_data = json.dumps(
                    {"mode": "creative", "description": description},
                    ensure_ascii=False,
                    separators=(",", ":"),
                )
                self.assertIn(request_data, prompt)
                self.assertIn("唯一的世界语义来源", prompt)
                self.assertIn("只代表可选能力，不是必须加入世界的内容", prompt)

    def test_normalized_goal_tasks_preserve_complete_english_copy(self):
        context = self.service._default_context(self.world)
        tasks = self.service._normalize_goal_plan_tasks(
            self.goal_plan_payload(), context, self.service._now_ms(),
        )
        self.assertEqual(6, len(tasks))
        for task in tasks:
            with self.subTest(task=task.get("taskKey")):
                for field in ("titleEn", "messageEn", "suggestionEn", "completionCriteriaEn"):
                    self.assertTrue(str(task.get(field) or "").strip())

    def test_goal_plan_rejects_unknown_completion_signal(self):
        context = self.service._default_context(self.world)
        payload = self.goal_plan_payload()
        payload["tasks"][0]["completionSignal"] = "write_python_script"
        with self.assertRaises(ValueError):
            self.service._normalize_goal_plan_tasks(payload, context, self.service._now_ms())


    def test_goal_plan_rejects_unknown_block_type(self):
        context = self.service._default_context(self.world)
        payload = self.goal_plan_payload()
        payload["tasks"][1]["requiredBlockTypes"] = ["invented_collision_block"]
        with self.assertRaises(ValueError):
            self.service._normalize_goal_plan_tasks(payload, context, self.service._now_ms())

    def test_goal_plan_requires_node_tasks_before_scene_polish(self):
        context = self.service._default_context(self.world)
        payload = self.goal_plan_payload()
        payload["tasks"][1].update({
            "phase": "scene-polish",
            "effectId": "",
            "completionSignal": "object_transformed",
            "requiredBlockTypes": [],
        })
        with self.assertRaises(ValueError):
            self.service._normalize_goal_plan_tasks(payload, context, self.service._now_ms())

    def test_goal_block_task_ignores_unrelated_block_type(self):
        context = self.service._default_context(self.world)
        context["worldGoal"] = {
            "prompt": "ink fantasy world with a rabbit and mechanisms",
            "mode": "story",
            "source": "ai",
            "status": "ready",
            "generatedAt": self.service._now_ms(),
            "generationError": "",
        }
        context["activeTasks"] = self.service._normalize_goal_plan_tasks(
            self.goal_plan_payload(), context, self.service._now_ms(),
        )
        self.service._ensure_task_slots_locked(context, self.service._now_ms())
        self.service._write_locked(self.world, context)

        unrelated = self.service.record_event({
            "type": "block_added",
            "category": "node",
            "success": True,
            "details": {"blockType": "logic_boolean", "interaction": "drag"},
            "worldId": self.world.name,
        })
        self.assertEqual([], unrelated["completedTaskKeys"])
        move_task = next(
            task for task in unrelated["context"]["activeTasks"]
            if task.get("taskKey") == "goal.ai.02"
        )
        self.assertEqual([], move_task["observedBlockTypes"])

        matched = self.service.record_event({
            "type": "block_added",
            "category": "node",
            "success": True,
            "details": {"blockType": "object_third_person_move", "interaction": "drag"},
            "worldId": self.world.name,
        })
        self.assertEqual(["goal.ai.02"], matched["completedTaskKeys"])
        archived = self.history_task(matched["context"], "goal.ai.02")
        self.assertEqual(["object_third_person_move"], archived["observedBlockTypes"])

    def test_load_recovers_personalized_plan_from_native_project_info(self):
        with mock.patch.object(
            CabbageContextService,
            "_call_deepseek_for_goal_plan",
            return_value=self.goal_plan_payload(),
        ), mock.patch(
            "plugins.AITool.services.cabbage_context_service.CoronaEditorApi.project_settings.get_active_project_info",
            return_value={
                "project_path": str(self.world),
                "mode": "creative",
                "prompt": "an ink fantasy world with rabbits and mechanisms",
            },
        ):
            response = self.service.load()
            task_id = response["goalPlan"]["taskId"]
            deadline = time.monotonic() + 3
            result = None
            while time.monotonic() < deadline:
                status = self.service.goal_plan_status(task_id)
                if status.get("status") == "completed":
                    result = status.get("result")
                    break
                time.sleep(0.02)

        self.assertTrue(response["success"])
        self.assertIsNotNone(result)
        self.assertTrue(result["success"])
        context = result["context"]
        self.assertEqual("creative", context["worldGoal"]["mode"])
        self.assertEqual(
            "an ink fantasy world with rabbits and mechanisms",
            context["worldGoal"]["prompt"],
        )
        self.assertEqual(2, len([
            task for task in context["activeTasks"]
            if task.get("type") == "goal" and task.get("status") == "active"
        ]))
        self.assertFalse(any(task.get("type") == "tutorial" for task in context["activeTasks"]))

    def test_load_does_not_restart_matching_personalized_plan(self):
        prompt = "a completed personalized world"
        self.world.joinpath("project.ini").write_text(
            f"[Project]\nname=story_world_test\nmode=story\nworld_prompt={prompt}\n",
            encoding="utf-8",
        )
        context = self.service._default_context(self.world)
        now = self.service._now_ms()
        tasks = self.service._normalize_goal_plan_tasks(self.goal_plan_payload(), context, now)
        context["worldGoal"] = {
            "prompt": prompt,
            "mode": "story",
            "source": "ai",
            "status": "ready",
            "generatedAt": now,
            "generationError": "",
            "generationId": "",
        }
        context["activeTasks"] = tasks
        self.service._ensure_task_slots_locked(context, now)
        self.service._write_locked(self.world, context)

        with mock.patch.object(self.service, "start_goal_plan") as start_goal_plan:
            response = self.service.load()

        self.assertTrue(response["success"])
        start_goal_plan.assert_not_called()
        self.assertEqual(prompt, response["context"]["worldGoal"]["prompt"])

    def test_ai_goal_plan_replaces_tutorials_and_shows_two_tasks(self):
        with mock.patch.object(
            CabbageContextService,
            "_call_deepseek_for_goal_plan",
            return_value=self.goal_plan_payload(),
        ):
            started = self.service.start_goal_plan({
                "prompt": "a puzzle world across floating islands",
                "mode": "story",
            })
            self.assertTrue(started["success"])
            self.assertEqual("pending", started["status"])
            deadline = time.monotonic() + 3
            result = None
            while time.monotonic() < deadline:
                status = self.service.goal_plan_status(started["taskId"])
                if status.get("status") == "completed":
                    result = status.get("result")
                    break
                time.sleep(0.02)

        self.assertIsNotNone(result)
        self.assertTrue(result["success"])
        context = result["context"]
        self.assertEqual("ai", context["worldGoal"]["source"])
        self.assertEqual(2, context["goalTaskPlan"]["schemaVersion"])
        self.assertEqual(6, context["goalTaskPlan"]["taskCount"])
        self.assertEqual(6, context["goalTaskPlan"]["nodeTaskCount"])
        self.assertEqual(0, context["goalTaskPlan"]["sceneTaskCount"])
        self.assertEqual(
            "an ink-style fantasy exploration world",
            context["goalTaskPlan"]["logicBlueprint"]["worldSummary"],
        )
        self.assertFalse(any(task.get("type") == "tutorial" for task in context["activeTasks"]))
        visible = [
            task for task in context["activeTasks"]
            if task.get("type") == "goal" and task.get("status") == "active"
        ]
        queued = [
            task for task in context["activeTasks"]
            if task.get("type") == "goal" and task.get("status") == "queued"
        ]
        self.assertEqual(2, len(visible))
        self.assertEqual(4, len(queued))

        self.assertFalse(
            (self.world / "Scripts" / "blockly").exists(),
            "World-description planning must not create or modify Blockly graph files",
        )

    def test_completing_goal_task_reveals_next_ai_task(self):
        context = self.service._default_context(self.world)
        context["worldGoal"] = {
            "prompt": "a puzzle world across floating islands",
            "mode": "story",
            "source": "ai",
            "status": "ready",
            "generatedAt": self.service._now_ms(),
            "generationError": "",
        }
        context["activeTasks"] = self.service._normalize_goal_plan_tasks(
            self.goal_plan_payload(), context, self.service._now_ms(),
        )
        self.service._ensure_task_slots_locked(context, self.service._now_ms())
        self.service._write_locked(self.world, context)

        response = self.service.record_event({
            "type": "node_created",
            "category": "node",
            "success": True,
            "details": {"nodeId": "start"},
            "worldId": self.world.name,
        })
        self.assertEqual(["goal.ai.01"], response["completedTaskKeys"])
        self.assertIsNotNone(self.history_task(response["context"], "goal.ai.01"))
        visible_keys = {
            task.get("taskKey")
            for task in response["context"]["activeTasks"]
            if task.get("type") == "goal" and task.get("status") == "active"
        }
        self.assertEqual({"goal.ai.02", "goal.ai.03"}, visible_keys)


    def test_generating_goal_without_tasks_keeps_default_tutorials_visible(self):
        context = self.service._default_context(self.world)
        context["worldGoal"] = {
            "prompt": "a world that still needs planning",
            "mode": "story",
            "source": "ai",
            "status": "generating",
            "generatedAt": 0,
            "generationError": "",
        }
        context["goalTaskPlan"] = {
            "schemaVersion": 2,
            "generatedAt": 0,
            "taskCount": 0,
            "nodeTaskCount": 0,
            "sceneTaskCount": 0,
            "logicBlueprint": {},
        }
        context["activeTasks"] = []

        self.service._ensure_task_slots_locked(context, self.service._now_ms())

        self.assertEqual(
            {"tutorial.basics.viewport_focus"},
            {self.active_tutorial(context)["taskKey"]},
        )

    def test_failed_goal_generation_restores_default_tutorial_tasks(self):
        with mock.patch.object(
            CabbageContextService,
            "_call_deepseek_for_goal_plan",
            side_effect=ValueError("invalid generated plan"),
        ):
            started = self.service.start_goal_plan({
                "prompt": "a personalized world whose plan fails",
                "mode": "story",
            })
            deadline = time.monotonic() + 3
            result = None
            while time.monotonic() < deadline:
                status = self.service.goal_plan_status(started["taskId"])
                if status.get("status") == "completed":
                    result = status.get("result")
                    break
                time.sleep(0.02)

        self.assertIsNotNone(result)
        self.assertFalse(result["success"])
        self.assertIn("context", result)
        context = result["context"]
        self.assertEqual("error", context["worldGoal"]["status"])
        self.assertEqual("invalid generated plan", context["worldGoal"]["generationError"])
        self.assertEqual(
            {"tutorial.basics.viewport_focus"},
            {self.active_tutorial(context)["taskKey"]},
        )
        self.assertFalse(any(task.get("type") == "goal" for task in context["activeTasks"]))

    def test_completed_ai_goal_plan_does_not_restart_default_tutorials(self):
        context = self.service._default_context(self.world)
        now = self.service._now_ms()
        tasks = self.service._normalize_goal_plan_tasks(self.goal_plan_payload(), context, now)
        context["worldGoal"] = {
            "prompt": "a completed personalized world",
            "mode": "story",
            "source": "ai",
            "status": "ready",
            "generatedAt": now,
            "generationError": "",
        }
        context["goalTaskPlan"] = {
            "schemaVersion": 2,
            "generatedAt": now,
            "taskCount": len(tasks),
            "nodeTaskCount": len(tasks),
            "sceneTaskCount": 0,
            "logicBlueprint": self.goal_plan_payload()["logicBlueprint"],
        }
        context["activeTasks"] = []
        context["taskHistory"].extend([
            {**task, "status": "completed", "completedAt": now, "updatedAt": now}
            for task in tasks
        ])

        self.service._ensure_task_slots_locked(context, now)

        self.assertFalse(any(task.get("type") == "tutorial" for task in context["activeTasks"]))


    def test_completed_goal_plan_is_reused_after_all_tasks_finish(self):
        context = self.service._default_context(self.world)
        now = self.service._now_ms()
        tasks = self.service._normalize_goal_plan_tasks(self.goal_plan_payload(), context, now)
        context["worldGoal"] = {
            "prompt": "a completed personalized world",
            "mode": "story",
            "source": "ai",
            "status": "ready",
            "generatedAt": now,
            "generationError": "",
            "generationId": "finished-plan",
        }
        context["activeTasks"] = []
        context["taskHistory"].extend([
            {**task, "status": "completed", "completedAt": now, "updatedAt": now}
            for task in tasks
        ])
        self.service._write_locked(self.world, context)

        with mock.patch.object(
            CabbageContextService, "_call_deepseek_for_goal_plan",
        ) as generate:
            response = self.service.start_goal_plan({
                "prompt": "a completed personalized world",
                "mode": "story",
            })

        self.assertTrue(response["success"])
        self.assertEqual("completed", response["status"])
        generate.assert_not_called()
        self.assertFalse(any(
            task.get("type") == "goal" for task in response["context"]["activeTasks"]
        ))

    def test_old_running_request_with_same_prompt_is_not_reused_after_goal_changed(self):
        context = self.service._default_context(self.world)
        context["worldGoal"] = {
            "prompt": "a different current goal",
            "mode": "story",
            "source": "ai",
            "status": "generating",
            "generatedAt": 0,
            "generationError": "",
            "generationId": "different-request",
        }
        self.service._write_locked(self.world, context)
        self.service._goal_plan_tasks["old-request"] = {
            "taskId": "old-request",
            "status": "running",
            "projectPath": str(self.world),
            "prompt": "the repeated prompt",
            "mode": "story",
            "createdAt": time.time(),
            "result": None,
        }

        class PendingFuture:
            def add_done_callback(self, callback):
                self.callback = callback

        with mock.patch.object(
            self.service._executor, "submit", return_value=PendingFuture(),
        ) as submit:
            response = self.service.start_goal_plan({
                "prompt": "the repeated prompt",
                "mode": "story",
            })

        self.assertTrue(response["success"])
        self.assertEqual("pending", response["status"])
        self.assertNotEqual("old-request", response["taskId"])
        submit.assert_called_once()
        loaded = self.service.load()["context"]
        self.assertEqual(response["taskId"], loaded["worldGoal"]["generationId"])
        self.assertEqual("the repeated prompt", loaded["worldGoal"]["prompt"])


    def test_stale_generation_id_cannot_replace_a_newer_goal_request(self):
        context = self.service._default_context(self.world)
        context["worldGoal"] = {
            "prompt": "the same visible prompt",
            "mode": "story",
            "source": "ai",
            "status": "generating",
            "generatedAt": 0,
            "generationError": "",
            "generationId": "new-request",
        }
        self.service._write_locked(self.world, context)

        with mock.patch.object(
            CabbageContextService,
            "_call_deepseek_for_goal_plan",
            return_value=self.goal_plan_payload(),
        ):
            result = self.service._generate_goal_plan(
                self.world, "the same visible prompt", "story", "old-request",
            )

        self.assertFalse(result["success"])
        self.assertEqual("GOAL_PLAN_STALE", result["error"])
        loaded = self.service.load()["context"]
        self.assertEqual("generating", loaded["worldGoal"]["status"])
        self.assertEqual("new-request", loaded["worldGoal"]["generationId"])
        self.assertFalse(any(task.get("type") == "goal" for task in loaded["activeTasks"]))

    def test_stale_generation_failure_cannot_mark_newer_request_as_error(self):
        context = self.service._default_context(self.world)
        context["worldGoal"] = {
            "prompt": "the same visible prompt",
            "mode": "story",
            "source": "ai",
            "status": "generating",
            "generatedAt": 0,
            "generationError": "",
            "generationId": "new-request",
        }
        self.service._write_locked(self.world, context)

        with mock.patch.object(
            CabbageContextService,
            "_call_deepseek_for_goal_plan",
            side_effect=ValueError("old request failed"),
        ):
            result = self.service._generate_goal_plan(
                self.world, "the same visible prompt", "story", "old-request",
            )

        self.assertFalse(result["success"])
        self.assertNotIn("context", result)
        loaded = self.service.load()["context"]
        self.assertEqual("generating", loaded["worldGoal"]["status"])
        self.assertEqual("", loaded["worldGoal"]["generationError"])
        self.assertEqual("new-request", loaded["worldGoal"]["generationId"])


if __name__ == "__main__":
    unittest.main()

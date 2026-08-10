from __future__ import annotations

import json
import time
import unittest
from types import SimpleNamespace
from unittest import mock

from editor.plugins.AITool.services.node_graph_generation_service import NodeGraphGenerationService
from editor.script_runtime.blockly.ai_node_graph_contract import load_contract_catalog


def minimal_workspace():
    return {
        "version": 1,
        "nodes": [
            {
                "id": "start",
                "macroType": "state",
                "nodeType": "start",
                "name": "Start",
                "customName": "Initialize",
                "x": 80,
                "y": 120,
                "workspace": {},
            }
        ],
        "edges": [],
        "globalVariablesWorkspace": {},
    }


def control_workspace(
    actor="ball",
    *,
    movement_type="object_third_person_move",
    jump_actor=None,
    include_jump=True,
    include_score=False,
    custom_name="\u7403\u4f53\u63a7\u5236",
    edge_name="\u8fdb\u5165\u63a7\u5236",
):
    movement_fields = {
        "NAME": actor,
        "SPEED": 0.18,
        "OBSTACLE_TAG": "obstacle",
        "MIN_X": -12,
        "MAX_X": 12,
        "MIN_Z": -12,
        "MAX_Z": 12,
    }
    movement = {
        "type": movement_type,
        "id": "move-ball",
        "fields": movement_fields,
    }
    tail = movement
    if include_jump:
        jump = {
            "type": "object_arcade_jump",
            "id": "jump-ball",
            "fields": {
                "NAME": actor if jump_actor is None else jump_actor,
                "POWER": 0.28,
                "GRAVITY": 0.025,
                "GROUND_Y": 0.8,
            },
        }
        tail["next"] = {"block": jump}
        tail = jump
    if include_score:
        tail["next"] = {
            "block": {
                "type": "ui_set_score",
                "id": "unrequested-score",
                "fields": {"VALUE": 0},
            }
        }
    control = {
        "id": "control",
        "macroType": "state",
        "nodeType": "custom",
        "name": custom_name,
        "customName": custom_name,
        "x": 480,
        "y": 120,
        "workspace": {
            "blocks": {
                "languageVersion": 0,
                "blocks": [
                    {
                        "type": "node_while_active",
                        "id": "control-active",
                        "x": 24,
                        "y": 24,
                        "inputs": {"DO": {"block": movement}},
                    }
                ],
            }
        },
    }
    edge = {
        "id": "start-control",
        "source": {"nodeId": "start", "side": "right", "index": 0},
        "target": {"nodeId": "control", "side": "left", "index": 0},
        "name": edge_name,
        "conditionWorkspace": {
            "blocks": {
                "languageVersion": 0,
                "blocks": [
                    {
                        "type": "logic_boolean",
                        "id": "always-control",
                        "fields": {"BOOL": "TRUE"},
                        "x": 24,
                        "y": 24,
                    }
                ],
            }
        },
    }
    return {
        "version": 1,
        "nodes": [minimal_workspace()["nodes"][0], control],
        "edges": [edge],
        "globalVariablesWorkspace": {},
    }


def tag_velocity_workspace():
    workspace = control_workspace(include_jump=False)
    root = workspace["nodes"][1]["workspace"]["blocks"]["blocks"][0]
    root["inputs"]["DO"]["block"] = {
        "type": "object_set_tag_velocity_axis",
        "id": "wrong-tag-velocity",
        "fields": {"TAG_TEXT": "ball", "AXIS": "X", "VALUE_NUMBER": 0.1},
    }
    return workspace


def coordinate_getter(axis, actor, block_id):
    return {
        "type": f"object_get_{axis.lower()}",
        "id": block_id,
        "fields": {"NAME": actor},
    }


def dynamic_tag_move_block(tag, source, block_id="move-tag"):
    return {
        "type": "object_move_tag",
        "id": block_id,
        "fields": {"TAG_TEXT": tag},
        "inputs": {
            "DX": {"block": coordinate_getter("X", source, block_id + "-source-x")},
            "DY": {"block": coordinate_getter("Y", source, block_id + "-source-y")},
            "DZ": {"block": coordinate_getter("Z", source, block_id + "-source-z")},
        },
    }


def dynamic_relation_workspace(source="LeaderActor", target="FollowerActor", *, live=True, distance_live=True):
    workspace = minimal_workspace()
    inputs = {}
    if live:
        inputs = {
            "X": {"block": coordinate_getter("X", source, "source-x")},
            "Y": {"block": coordinate_getter("Y", source, "source-y")},
            "Z": {"block": coordinate_getter("Z", source, "source-z")},
        }
    move = {
        "type": "object_set_position",
        "id": "follow-target",
        "fields": {"NAME": target, "X": 0, "Y": 0, "Z": 0},
    }
    if inputs:
        move["inputs"] = inputs
    near_inputs = {
        "TOLERANCE": {
            "shadow": {"type": "math_number", "id": "distance-threshold", "fields": {"NUM": 2}}
        }
    }
    if distance_live:
        near_inputs.update({
            "X": {"block": coordinate_getter("X", source, "distance-source-x")},
            "Y": {"block": coordinate_getter("Y", source, "distance-source-y")},
            "Z": {"block": coordinate_getter("Z", source, "distance-source-z")},
        })
    condition = {
        "type": "detect_position_near",
        "id": "target-near-source",
        "fields": {
            "NAME_TEXT": target,
            "X_NUMBER": 0,
            "Y_NUMBER": 0,
            "Z_NUMBER": 0,
            "TOLERANCE_NUMBER": 2,
        },
        "inputs": near_inputs,
    }
    move["next"] = {
        "block": {
            "type": "control_if",
            "id": "hide-when-near",
            "inputs": {
                "CONDITION": {"block": condition},
                "DO": {
                    "block": {
                        "type": "object_hide",
                        "id": "hide-target",
                        "fields": {"NAME": target},
                    }
                },
            },
        }
    }
    workspace["nodes"][0]["workspace"] = {
        "blocks": {
            "languageVersion": 0,
            "blocks": [{
                "type": "node_while_active",
                "id": "active",
                "x": 24,
                "y": 24,
                "inputs": {"DO": {"block": move}},
            }],
        }
    }
    return workspace


def camera_follow_workspace(actor):
    workspace = minimal_workspace()
    workspace["nodes"][0]["workspace"] = {
        "blocks": {
            "languageVersion": 0,
            "blocks": [{
                "type": "node_while_active",
                "id": "camera-active",
                "x": 24,
                "y": 24,
                "inputs": {
                    "DO": {
                        "block": {
                            "type": "camera_follow_object",
                            "id": "camera-follow",
                            "fields": {"NAME": actor},
                        }
                    }
                },
            }],
        }
    }
    return workspace


def legacy_field_input_workspace(actor="bunny2"):
    start = minimal_workspace()["nodes"][0]
    start["workspace"] = {
        "blocks": {
            "languageVersion": 0,
            "blocks": [
                {
                    "type": "node_while_active",
                    "id": "active-root",
                    "x": 24,
                    "y": 24,
                    "inputs": {
                        "DO": {
                            "block": {
                                "type": "control_if",
                                "id": "legacy-if",
                                "fields": {"BOOL": "TRUE"},
                                "inputs": {
                                    "DO": {
                                        "block": {
                                            "type": "engine_rotateZ",
                                            "id": "legacy-rotate",
                                            "fields": {"ANGLE": 15, "OBJECT": actor},
                                        }
                                    }
                                },
                            }
                        }
                    },
                }
            ],
        }
    }
    return {
        "version": 1,
        "nodes": [start],
        "edges": [],
        "globalVariablesWorkspace": {},
    }


def request_payload(**overrides):
    value = {
        "schemaVersion": 1,
        "requestId": "request-1",
        "targetId": "node_graph:project:global",
        "projectScopeId": "world-scope-1",
        "baseGraphRevision": "revision-1",
        "operation": "create",
        "instruction": "Please create a dodgeball demo",
        "workspace": minimal_workspace(),
        "projectContext": {
            "sceneName": "default",
            "actors": [{"name": "Player", "type": "model", "tags": ["player"]}],
        },
    }
    value.update(overrides)
    return value


def generated_result(request=None, **overrides):
    request = request or request_payload()
    value = {
        "schemaVersion": 1,
        "requestId": request["requestId"],
        "targetId": request["targetId"],
        "projectScopeId": request["projectScopeId"],
        "baseGraphRevision": request["baseGraphRevision"],
        "operation": request["operation"],
        "summary": "Generated the dodgeball node graph.",
        "workspace": minimal_workspace(),
    }
    value.update(overrides)
    return value


class NodeGraphGenerationServiceTests(unittest.TestCase):
    def setUp(self):
        self.service = NodeGraphGenerationService()

    def tearDown(self):
        self.service.shutdown()

    def test_prompt_contains_complete_contract_workspace_and_actor_context(self):
        request = self.service._normalize_payload(request_payload())
        _path, contract = self.service._load_contract()
        prompt = self.service._build_prompt(request, contract)
        self.assertIn(contract, prompt)
        self.assertIn('"baseGraphRevision":"revision-1"', prompt)
        self.assertIn('"nodes":[{"id":"start"', prompt)
        self.assertIn('"name":"Player"', prompt)
        self.assertIn("FULL_CORONA_BLOCKS_CONTRACT_XML", prompt)
        self.assertIn("control_if has no BOOL field", prompt)
        self.assertIn("inputs.OBJECT.block using object_reference", prompt)
        self.assertIn("choose an exact actor name from PROJECT_CONTEXT", prompt)
        self.assertNotIn("{...}", prompt)


    def test_chinese_instruction_sets_chinese_response_language(self):
        request = self.service._normalize_payload(
            request_payload(instruction="\u5e2e\u6211\u7ed9\u7403\u52a0\u4e0a WASD \u548c\u7a7a\u683c\u8df3\u8dc3", responseLanguage="")
        )
        self.assertEqual("zh-CN", request["responseLanguage"])

    def test_prompt_scopes_wasd_jump_to_minimal_real_object_control(self):
        request = self.service._normalize_payload(
            request_payload(
                operation="extend",
                instruction="\u5e2e\u6211\u7ed9\u5f53\u524d\u573a\u666f\u4e2d\u7684\u7403\u52a0\u4e00\u4e2a WASD \u548c\u7a7a\u683c\u8df3\u8dc3",
                responseLanguage="zh-CN",
            )
        )
        _path, contract = self.service._load_contract()
        prompt = self.service._build_prompt(request, contract)
        self.assertIn("object_third_person_move", prompt)
        self.assertIn("object_arcade_jump", prompt)
        self.assertIn("object_set_tag_velocity_axis", prompt)
        self.assertIn("Do not expand a small feature into a full game", prompt)
        self.assertIn('"responseLanguage":"zh-CN"', prompt)

    def test_chinese_request_rejects_english_summary_and_labels(self):
        request = self.service._normalize_payload(
            request_payload(
                operation="extend",
                instruction="\u7ed9\u7403\u52a0 WASD \u548c\u7a7a\u683c\u8df3\u8dc3",
                responseLanguage="zh-CN",
            )
        )
        contract_path, _contract = self.service._load_contract()
        result = generated_result(
            request,
            summary="Added ball controls.",
            workspace=control_workspace(custom_name="Play", edge_name="Start Play"),
        )
        with self.assertRaisesRegex(ValueError, "summary"):
            self.service._validate_result(result, request, contract_path)

    def test_tag_velocity_template_cannot_satisfy_wasd_and_jump(self):
        request = self.service._normalize_payload(
            request_payload(
                operation="extend",
                instruction="\u7ed9\u7403\u52a0 WASD \u548c\u7a7a\u683c\u8df3\u8dc3",
                responseLanguage="zh-CN",
            )
        )
        contract_path, _contract = self.service._load_contract()
        result = generated_result(
            request,
            summary="\u5df2\u6dfb\u52a0\u7403\u4f53\u63a7\u5236\u3002",
            workspace=tag_velocity_workspace(),
        )
        with self.assertRaisesRegex(ValueError, "object_third_person_move"):
            self.service._validate_result(result, request, contract_path)

    def test_wasd_and_jump_chain_for_same_real_actor_is_accepted(self):
        request = self.service._normalize_payload(
            request_payload(
                operation="extend",
                instruction="\u7ed9\u7403\u52a0 WASD \u548c\u7a7a\u683c\u8df3\u8dc3",
                responseLanguage="zh-CN",
                projectContext={"actors": [{"name": "ball", "type": "model", "tags": []}]},
            )
        )
        contract_path, _contract = self.service._load_contract()
        normalized = self.service._validate_result(
            generated_result(
                request,
                summary="\u5df2\u4e3a\u7403\u6dfb\u52a0 WASD \u79fb\u52a8\u548c\u7a7a\u683c\u8df3\u8dc3\u3002",
                workspace=control_workspace(),
            ),
            request,
            contract_path,
        )
        self.assertEqual(2, len(normalized["workspace"]["nodes"]))

    def test_movement_and_jump_must_target_the_same_actor(self):
        request = self.service._normalize_payload(
            request_payload(
                operation="extend",
                instruction="\u7ed9\u7403\u52a0 WASD \u548c\u7a7a\u683c\u8df3\u8dc3",
                responseLanguage="zh-CN",
                projectContext={
                    "actors": [
                        {"name": "ball", "type": "model", "tags": []},
                        {"name": "other", "type": "model", "tags": []},
                    ]
                },
            )
        )
        contract_path, _contract = self.service._load_contract()
        result = generated_result(
            request,
            summary="\u5df2\u6dfb\u52a0\u79fb\u52a8\u548c\u8df3\u8dc3\u3002",
            workspace=control_workspace(jump_actor="other"),
        )
        with self.assertRaisesRegex(ValueError, "\u540c\u4e00\u4e2a\u5bf9\u8c61"):
            self.service._validate_result(result, request, contract_path)

    def test_control_target_must_exist_in_scene_context(self):
        request = self.service._normalize_payload(
            request_payload(
                operation="extend",
                instruction="\u7ed9\u7403\u52a0 WASD \u548c\u7a7a\u683c\u8df3\u8dc3",
                responseLanguage="zh-CN",
                projectContext={"actors": [{"name": "Player", "type": "model", "tags": []}]},
            )
        )
        contract_path, _contract = self.service._load_contract()
        result = generated_result(
            request,
            summary="\u5df2\u6dfb\u52a0\u7403\u4f53\u63a7\u5236\u3002",
            workspace=control_workspace(actor="ball"),
        )
        with self.assertRaisesRegex(ValueError, "\u4e0d\u5b58\u5728\u4e8e\u5f53\u524d\u573a\u666f"):
            self.service._validate_result(result, request, contract_path)

    def test_narrow_control_request_rejects_unrequested_score_logic(self):
        request = self.service._normalize_payload(
            request_payload(
                operation="extend",
                instruction="\u7ed9\u7403\u52a0 WASD \u548c\u7a7a\u683c\u8df3\u8dc3",
                responseLanguage="zh-CN",
                projectContext={"actors": [{"name": "ball", "type": "model", "tags": []}]},
            )
        )
        contract_path, _contract = self.service._load_contract()
        result = generated_result(
            request,
            summary="\u5df2\u6dfb\u52a0\u7403\u4f53\u63a7\u5236\u3002",
            workspace=control_workspace(include_score=True),
        )
        with self.assertRaisesRegex(ValueError, "ui_set_score"):
            self.service._validate_result(result, request, contract_path)

    def test_legacy_bool_and_object_fields_are_safely_normalized(self):
        request = self.service._normalize_payload(
            request_payload(
                operation="extend",
                instruction="\u7ed9\u5154\u5b50\u6dfb\u52a0\u65cb\u8f6c\u529f\u80fd",
                responseLanguage="zh-CN",
                projectContext={"actors": [{"name": "bunny2", "type": "model", "tags": []}]},
            )
        )
        contract_path, _contract = self.service._load_contract()
        normalized = self.service._validate_result(
            generated_result(
                request,
                summary="\u5df2\u4e3a\u5154\u5b50\u6dfb\u52a0\u65cb\u8f6c\u529f\u80fd\u3002",
                workspace=legacy_field_input_workspace(),
            ),
            request,
            contract_path,
        )
        root = normalized["workspace"]["nodes"][0]["workspace"]["blocks"]["blocks"][0]
        control_if = root["inputs"]["DO"]["block"]
        rotate = control_if["inputs"]["DO"]["block"]
        self.assertNotIn("BOOL", control_if.get("fields", {}))
        self.assertEqual(
            "logic_boolean", control_if["inputs"]["CONDITION"]["block"]["type"]
        )
        self.assertNotIn("OBJECT", rotate.get("fields", {}))
        self.assertEqual(
            "bunny2", rotate["inputs"]["OBJECT"]["block"]["fields"]["OBJECT"]
        )
        self.assertTrue(any("moved BOOL" in item for item in normalized["warnings"]))
        self.assertTrue(any("moved OBJECT" in item for item in normalized["warnings"]))

    def test_object_reference_must_exist_in_current_scene(self):
        request = self.service._normalize_payload(
            request_payload(
                operation="extend",
                instruction="\u7ed9\u5154\u5b50\u6dfb\u52a0\u65cb\u8f6c\u529f\u80fd",
                responseLanguage="zh-CN",
                projectContext={"actors": [{"name": "bunny2", "type": "model", "tags": []}]},
            )
        )
        contract_path, _contract = self.service._load_contract()
        result = generated_result(
            request,
            summary="\u5df2\u6dfb\u52a0\u65cb\u8f6c\u529f\u80fd\u3002",
            workspace=legacy_field_input_workspace(actor="missing-rabbit"),
        )
        with self.assertRaisesRegex(ValueError, "\u4e0d\u5b58\u5728\u4e8e\u5f53\u524d\u573a\u666f"):
            self.service._validate_result(result, request, contract_path)

    def test_unknown_fields_are_still_rejected_after_safe_normalization(self):
        request = self.service._normalize_payload(
            request_payload(
                operation="extend",
                instruction="\u7ed9\u5154\u5b50\u6dfb\u52a0\u65cb\u8f6c\u529f\u80fd",
                responseLanguage="zh-CN",
                projectContext={"actors": [{"name": "bunny2", "type": "model", "tags": []}]},
            )
        )
        contract_path, _contract = self.service._load_contract()
        workspace = legacy_field_input_workspace()
        rotate = workspace["nodes"][0]["workspace"]["blocks"]["blocks"][0]["inputs"]["DO"]["block"]["inputs"]["DO"]["block"]
        rotate["fields"]["BROKEN"] = 1
        result = generated_result(
            request,
            summary="\u5df2\u6dfb\u52a0\u65cb\u8f6c\u529f\u80fd\u3002",
            workspace=workspace,
        )
        with self.assertRaisesRegex(ValueError, "unknown field BROKEN"):
            self.service._validate_result(result, request, contract_path)

    def test_replacement_instruction_extracts_source_and_target(self):
        requirements = self.service._instruction_requirements(
            "\u8bf7\u5c06\u79fb\u52a8\u79ef\u6728\u7684\u5bf9\u8c61\u6539\u4e3a bunny2\uff0c"
        )
        self.assertEqual(
            {"source": "\u79fb\u52a8\u79ef\u6728\u7684\u5bf9\u8c61", "target": "bunny2"},
            requirements["replacementDirective"],
        )

    def test_edit_prompt_requires_in_place_change(self):
        request = self.service._normalize_payload(
            request_payload(
                operation="edit",
                instruction="\u5c06\u79fb\u52a8\u79ef\u6728\u7684\u5bf9\u8c61\u6539\u4e3a bunny2",
                responseLanguage="zh-CN",
            )
        )
        _path, contract = self.service._load_contract()
        prompt = self.service._build_prompt(request, contract)
        self.assertIn("Edit the current workspace in place", prompt)
        self.assertIn("Do not clear or rebuild the graph", prompt)
        self.assertIn('"target":"bunny2"', prompt)

    def test_edit_cannot_remove_existing_node_or_edge(self):
        before = control_workspace(actor="bunny1", include_jump=False)
        request = self.service._normalize_payload(
            request_payload(
                operation="edit",
                instruction="\u5c06\u79fb\u52a8\u79ef\u6728\u7684\u5bf9\u8c61\u6539\u4e3a bunny2",
                responseLanguage="zh-CN",
                workspace=before,
            )
        )
        without_node = {**before, "nodes": before["nodes"][:1]}
        with self.assertRaisesRegex(ValueError, "removed existing structures"):
            self.service._validate_operation_scope({"workspace": without_node}, request)
        without_edge = {**before, "edges": []}
        with self.assertRaisesRegex(ValueError, "removed existing structures"):
            self.service._validate_operation_scope({"workspace": without_edge}, request)

    def test_edit_must_change_existing_logic(self):
        before = control_workspace(actor="bunny1", include_jump=False)
        request = self.service._normalize_payload(
            request_payload(
                operation="edit",
                instruction="\u5c06\u79fb\u52a8\u79ef\u6728\u7684\u5bf9\u8c61\u6539\u4e3a bunny2",
                responseLanguage="zh-CN",
                workspace=before,
            )
        )
        with self.assertRaisesRegex(ValueError, "did not change any node logic"):
            self.service._validate_operation_scope({"workspace": before}, request)

    def test_edit_can_replace_only_an_existing_object_reference(self):
        before = control_workspace(actor="bunny1", include_jump=False)
        after = control_workspace(actor="bunny2", include_jump=False)
        request = self.service._normalize_payload(
            request_payload(
                operation="edit",
                instruction="\u5c06\u79fb\u52a8\u79ef\u6728\u7684\u5bf9\u8c61\u6539\u4e3a bunny2",
                responseLanguage="zh-CN",
                workspace=before,
                projectContext={
                    "actors": [
                        {"name": "bunny1", "type": "model", "tags": []},
                        {"name": "bunny2", "type": "model", "tags": []},
                    ]
                },
            )
        )
        contract_path, _contract = self.service._load_contract()
        normalized = self.service._validate_result(
            generated_result(
                request,
                summary="\u5df2\u5c06\u79fb\u52a8\u79ef\u6728\u7684\u5bf9\u8c61\u6539\u4e3a bunny2\u3002",
                workspace=after,
            ),
            request,
            contract_path,
        )
        self.assertEqual(
            "bunny2",
            normalized["workspace"]["nodes"][1]["workspace"]["blocks"]["blocks"][0]
            ["inputs"]["DO"]["block"]["fields"]["NAME"],
        )
        self.assertEqual(
            [node["id"] for node in before["nodes"]],
            [node["id"] for node in normalized["workspace"]["nodes"]],
        )
        self.assertEqual(
            [edge["id"] for edge in before["edges"]],
            [edge["id"] for edge in normalized["workspace"]["edges"]],
        )

    def test_wrong_request_identity_is_rejected(self):
        request = self.service._normalize_payload(request_payload())
        contract_path, _contract = self.service._load_contract()
        result = generated_result(request, baseGraphRevision="stale-revision")
        with self.assertRaisesRegex(ValueError, "baseGraphRevision"):
            self.service._validate_result(result, request, contract_path)

    def test_valid_complete_workspace_passes_contract_validation(self):
        request = self.service._normalize_payload(request_payload())
        contract_path, _contract = self.service._load_contract()
        normalized = self.service._validate_result(generated_result(request), request, contract_path)
        self.assertEqual("node_graph:project:global", normalized["targetId"])
        self.assertEqual("revision-1", normalized["baseGraphRevision"])
        self.assertEqual(1, len(normalized["workspace"]["nodes"]))

    def test_forbidden_python_or_xml_fields_are_rejected(self):
        request = self.service._normalize_payload(request_payload())
        contract_path, _contract = self.service._load_contract()
        for forbidden in ("python", "generatedCode", "xml"):
            with self.subTest(forbidden=forbidden):
                result = generated_result(request)
                result[forbidden] = "not allowed"
                with self.assertRaisesRegex(ValueError, forbidden):
                    self.service._validate_result(result, request, contract_path)


    def test_rejected_semantic_result_is_retried_once(self):
        payload = request_payload(
            operation="extend",
            instruction="\u7ed9\u7403\u52a0 WASD \u548c\u7a7a\u683c\u8df3\u8dc3",
            responseLanguage="zh-CN",
            projectContext={"actors": [{"name": "ball", "type": "model", "tags": []}]},
        )
        invalid = generated_result(
            payload,
            summary="\u5df2\u6dfb\u52a0\u7403\u4f53\u63a7\u5236\u3002",
            workspace=tag_velocity_workspace(),
        )
        valid = generated_result(
            payload,
            summary="\u5df2\u4e3a\u7403\u6dfb\u52a0 WASD \u79fb\u52a8\u548c\u7a7a\u683c\u8df3\u8dc3\u3002",
            workspace=control_workspace(),
        )
        settings = SimpleNamespace(
            api_key="test-key",
            base_url="https://api.deepseek.com",
            model="deepseek-test",
            source="unit-test",
        )
        responses = [
            json.dumps(invalid, ensure_ascii=False),
            json.dumps(valid, ensure_ascii=False),
        ]
        with mock.patch.object(
            NodeGraphGenerationService, "_call_deepseek", side_effect=responses
        ) as provider, mock.patch(
            "editor.plugins.AITool.services.node_graph_generation_service.NodeGraphReviewService._resolve_settings",
            return_value=settings,
        ):
            result = self.service.generate(payload)
        self.assertTrue(result["success"])
        self.assertEqual(2, provider.call_count)

    def test_async_task_reaches_completed_without_real_provider_call(self):
        request = request_payload()
        settings = SimpleNamespace(
            api_key="test-key",
            base_url="https://api.deepseek.com",
            model="deepseek-test",
            source="unit-test",
        )
        response = json.dumps(generated_result(request), ensure_ascii=False)
        with mock.patch.object(NodeGraphGenerationService, "_call_deepseek", return_value=response), mock.patch(
            "editor.plugins.AITool.services.node_graph_generation_service.NodeGraphReviewService._resolve_settings",
            return_value=settings,
        ):
            started = self.service.start(request)
            self.assertTrue(started["success"])
            deadline = time.time() + 3
            status = self.service.status(started["taskId"])
            while status.get("status") == "pending" and time.time() < deadline:
                time.sleep(0.01)
                status = self.service.status(started["taskId"])
        self.assertEqual("completed", status["status"])
        self.assertTrue(status["result"]["success"])
        self.assertEqual("ok", status["result"]["status"])

    def test_cancelled_task_stays_cancelled(self):
        def wait_for_cancel(_payload, cancel_event):
            cancel_event.wait(1)
            return {"success": False, "status": "error", "error": "GENERATION_CANCELLED"}

        with mock.patch.object(self.service, "generate", side_effect=wait_for_cancel):
            started = self.service.start(request_payload())
            cancelled = self.service.cancel(started["taskId"])
            self.assertTrue(cancelled["success"])
            deadline = time.time() + 2
            status = self.service.status(started["taskId"])
            while status.get("status") == "pending" and time.time() < deadline:
                time.sleep(0.01)
                status = self.service.status(started["taskId"])
        self.assertEqual("cancelled", status["status"])


    def test_generation_prompt_uses_current_scene_actors_without_binding_workflow(self):
        payload = request_payload(
            projectContext={
                "sceneName": "Scene/default.scene",
                "actorContextAvailable": True,
                "actors": [{"name": "modern chair 11 obj", "type": "model"}],
            },
        )
        request = self.service._normalize_payload(payload)
        _contract_path, contract = self.service._load_contract()
        prompt = self.service._build_prompt(request, contract)
        self.assertIn("already scoped to the current Native Editor scene", prompt)
        self.assertIn("must never trigger a scene-binding workflow", prompt)
        self.assertIn("Never ask the user to bind a scene or actor", prompt)
        self.assertIn("modern chair 11 obj", prompt)


    def test_contract_selector_filters_common_capabilities_and_falls_back_for_unknown_requests(self):
        contract_path, contract = self.service._load_contract()
        cases = (
            ("\u79fb\u52a8 Player", "object_third_person_move"),
            ("\u8ba9\u76f8\u673a\u8ddf\u968f Player", "camera_follow_object"),
            ("\u7ed9 Player \u5f00\u542f\u7269\u7406\u548c\u901f\u5ea6", "object_set_native_physics"),
            ("\u68c0\u6d4b Player \u8ddd\u79bb\u662f\u5426\u5c0f\u4e8e 5", "detect_position_near"),
            ("\u8ba9 Player \u6301\u7eed\u65cb\u8f6c", "engine_rotateY"),
            ("\u6279\u91cf\u79fb\u52a8\u6240\u6709\u5bf9\u8c61", "object_move_tag"),
        )
        for instruction, expected_type in cases:
            with self.subTest(instruction=instruction):
                request = self.service._normalize_payload(request_payload(instruction=instruction))
                requirements = self.service._instruction_requirements(
                    instruction, request["operation"], request["projectContext"]
                )
                selection = self.service._select_contract(
                    request, contract_path, contract, requirements
                )
                self.assertEqual("filtered", selection["mode"])
                self.assertGreaterEqual(len(selection["selectedTypes"]), 20)
                self.assertLessEqual(len(selection["selectedTypes"]), 60)
                self.assertIn(expected_type, selection["selectedTypes"])

        unknown = self.service._normalize_payload(
            request_payload(instruction="\u521b\u5efa\u4e00\u4e2a\u795e\u79d8\u673a\u5236")
        )
        requirements = self.service._instruction_requirements(
            unknown["instruction"], unknown["operation"], unknown["projectContext"]
        )
        selection = self.service._select_contract(
            unknown, contract_path, contract, requirements
        )
        self.assertEqual("full", selection["mode"])
        expected_contract_size = len(load_contract_catalog(contract_path)["blocks"])
        self.assertEqual(expected_contract_size, len(selection["selectedTypes"]))

        unknown_with_existing_graph = self.service._normalize_payload(
            request_payload(
                instruction="\u521b\u5efa\u4e00\u4e2a\u795e\u79d8\u673a\u5236",
                workspace=control_workspace(include_jump=False),
            )
        )
        requirements = self.service._instruction_requirements(
            unknown_with_existing_graph["instruction"],
            unknown_with_existing_graph["operation"],
            unknown_with_existing_graph["projectContext"],
        )
        selection = self.service._select_contract(
            unknown_with_existing_graph, contract_path, contract, requirements
        )
        self.assertEqual("full", selection["mode"])
        self.assertEqual(expected_contract_size, len(selection["selectedTypes"]))

    def test_contract_selector_uses_xml_capability_metadata(self):
        contract_path, contract = self.service._load_contract()
        request = self.service._normalize_payload(request_payload(
            instruction="make Player rotate continuously"
        ))
        requirements = self.service._instruction_requirements(
            request["instruction"], request["operation"], request["projectContext"]
        )
        capability_map = dict(self.service.CAPABILITY_BLOCK_TYPES)
        capability_map["rotation"] = set()
        with mock.patch.object(NodeGraphGenerationService, "CAPABILITY_BLOCK_TYPES", capability_map):
            selection = self.service._select_contract(
                request, contract_path, contract, requirements
            )
        self.assertEqual("filtered", selection["mode"])
        self.assertIn("engine_rotateY", selection["selectedTypes"])

    def test_contract_signatures_include_xml_capabilities_and_usage_hints(self):
        _contract_path, contract = self.service._load_contract()
        signatures = self.service._contract_signature_index(contract)
        follow = signatures["camera_follow_object"]
        self.assertIn("camera-follow", follow["capabilities"])
        self.assertIn("exact real actor", follow["aiUse"])

    def test_representative_requirements_resolve_real_multi_targets_and_alternatives(self):
        context = {
            "actorContextAvailable": True,
            "actors": [
                {"name": "\u9ed1\u6d1e", "semanticRole": "attractor", "tags": ["source"]},
                {"name": "\u5efa\u7b51A", "tags": ["building"]},
                {"name": "\u5efa\u7b51B", "tags": ["building"]},
            ],
        }
        requirements = self.service._instruction_requirements(
            "\u8ba9\u573a\u666f\u4e2d\u7684\u9ed1\u6d1e\u6301\u7eed\u5438\u9644\u6240\u6709\u5efa\u7b51\uff0c"
            "\u8ddd\u79bb\u5c0f\u4e8e5\u540e\u9690\u85cf\u6216\u9500\u6bc1\u76ee\u6807\uff0c\u540c\u65f6\u76f8\u673a\u8ddf\u968f\u9ed1\u6d1e\u3002",
            "create",
            context,
        )
        self.assertEqual(["\u9ed1\u6d1e"], requirements["actors"]["source"])
        self.assertEqual(["\u5efa\u7b51A", "\u5efa\u7b51B"], requirements["actors"]["targets"])
        self.assertEqual(5, requirements["parameters"]["distance"])
        self.assertIn("force", requirements["parameters"]["defaultsRequired"])
        for capability in (
            "attraction", "distance-check", "threshold-action", "object-removal",
            "visibility", "camera-follow", "multi-object",
        ):
            self.assertIn(capability, requirements["capabilities"])
        self.assertEqual(
            [["visibility", "object-removal"]],
            requirements["capabilityAlternatives"],
        )
        self.assertNotIn("black-hole", json.dumps(requirements))
        contract_path, contract = self.service._load_contract()
        request = self.service._normalize_payload(request_payload(
            instruction=(
                "\u8ba9\u573a\u666f\u4e2d\u7684\u9ed1\u6d1e\u6301\u7eed\u5438\u9644\u6240\u6709\u5efa\u7b51\uff0c"
                "\u8ddd\u79bb\u5c0f\u4e8e5\u540e\u9690\u85cf\u6216\u9500\u6bc1\u76ee\u6807\uff0c\u540c\u65f6\u76f8\u673a\u8ddf\u968f\u9ed1\u6d1e\u3002"
            ),
            responseLanguage="zh-CN",
            projectContext=context,
        ))
        selection = self.service._select_contract(
            request, contract_path, contract, requirements
        )
        self.assertEqual("filtered", selection["mode"])
        self.assertGreaterEqual(len(selection["selectedTypes"]), 20)
        self.assertLessEqual(len(selection["selectedTypes"]), 60)

    def test_actor_matching_uses_word_boundaries_and_generic_semantic_roles(self):
        boundary_context = {
            "actorContextAvailable": True,
            "actors": [
                {"name": "A"},
                {"name": "Player"},
            ],
        }
        requirements = self.service._instruction_requirements(
            "camera follows Player",
            "create",
            boundary_context,
        )
        self.assertEqual(["Player"], requirements["actors"]["mentioned"])
        self.assertEqual(["Player"], requirements["actors"]["source"])

        role_context = {
            "actorContextAvailable": True,
            "actors": [
                {"name": "SourceCore", "semanticRole": "source"},
                {"name": "TowerA", "semanticRole": "building"},
                {"name": "TowerB", "tags": ["building"]},
            ],
        }
        requirements = self.service._instruction_requirements(
            "\u8ba9\u6240\u6709\u5efa\u7b51\u88ab\u6765\u6e90\u5bf9\u8c61\u6301\u7eed\u5438\u9644",
            "create",
            role_context,
        )
        self.assertEqual(["SourceCore"], requirements["actors"]["source"])
        self.assertEqual(["TowerA", "TowerB"], requirements["actors"]["targets"])
        self.assertTrue(requirements["actors"]["multiTarget"])

    def test_unreachable_control_and_tag_blocks_do_not_satisfy_semantics(self):
        control_request = self.service._normalize_payload(request_payload(
            instruction="Give ball WASD control",
            projectContext={
                "actorContextAvailable": True,
                "actors": [{"name": "ball"}],
            },
        ))
        disconnected_control = control_workspace(actor="ball", include_jump=False)
        disconnected_control["edges"] = []
        with self.assertRaisesRegex(ValueError, "\u63a7\u5236\u80fd\u529b"):
            self.service._validate_requested_semantics(
                {"workspace": disconnected_control}, control_request
            )

        context = {
            "actorContextAvailable": True,
            "actors": [
                {"name": "\u9ed1\u6d1e", "semanticRole": "source"},
                {"name": "\u5efa\u7b51A", "tags": ["building"]},
                {"name": "\u5efa\u7b51B", "tags": ["building"]},
            ],
        }
        tag_request = self.service._normalize_payload(request_payload(
            instruction="\u8ba9\u9ed1\u6d1e\u6301\u7eed\u5438\u9644\u6240\u6709\u5efa\u7b51",
            responseLanguage="zh-CN",
            projectContext=context,
        ))
        workspace = control_workspace(include_jump=False)
        reachable_root = workspace["nodes"][1]["workspace"]["blocks"]["blocks"][0]
        reachable_root["inputs"]["DO"]["block"] = {
            "type": "object_move_tag",
            "id": "reachable-wrong-tag",
            "fields": {"TAG_TEXT": "unrelated", "DX_NUMBER": 0, "DY_NUMBER": 0, "DZ_NUMBER": 0},
        }
        workspace["nodes"].append({
            "id": "detached",
            "macroType": "state",
            "nodeType": "custom",
            "name": "Detached",
            "customName": "Detached",
            "x": 900,
            "y": 500,
            "workspace": {
                "blocks": {
                    "languageVersion": 0,
                    "blocks": [{
                        "type": "node_while_active",
                        "id": "detached-active",
                        "inputs": {"DO": {"block": {
                            "type": "object_move_tag",
                            "id": "detached-correct-tag",
                            "fields": {
                                "TAG_TEXT": "building",
                                "DX_NUMBER": 0,
                                "DY_NUMBER": 0,
                                "DZ_NUMBER": 0,
                            },
                        }}},
                    }],
                },
            },
        })
        with self.assertRaisesRegex(ValueError, "\u591a\u76ee\u6807"):
            self.service._validate_requested_semantics({"workspace": workspace}, tag_request)

    def test_compact_project_context_keeps_semantics_and_drops_render_payloads(self):
        compact = self.service._compact_project_context({
            "sceneName": "default",
            "actorContextAvailable": False,
            "actors": [{
                "name": "Building A",
                "type": "model",
                "tags": ["building"],
                "aliases": ["Tower"],
                "semanticRole": "target",
                "transform": {"position": [1, 2, 3]},
                "size": [4, 5, 6],
                "collision": "box",
                "physicsEnabled": True,
                "material": {"huge": True},
                "optics": {"metallic": 1},
                "renderCache": [1, 2, 3],
            }],
        })
        self.assertFalse(compact["actorContextAvailable"])
        actor = compact["actors"][0]
        self.assertEqual("target", actor["semanticRole"])
        self.assertEqual({"position": [1, 2, 3]}, actor["transform"])
        self.assertTrue(actor["physicsEnabled"])
        self.assertNotIn("material", actor)
        self.assertNotIn("optics", actor)
        self.assertNotIn("renderCache", actor)

    def test_actor_reference_validation_degrades_when_context_is_unavailable(self):
        request = self.service._normalize_payload(request_payload(
            operation="extend",
            instruction="\u7ed9\u5bf9\u8c61\u6dfb\u52a0\u65cb\u8f6c",
            responseLanguage="zh-CN",
            projectContext={"actorContextAvailable": False, "actors": []},
        ))
        self.service._validate_actor_references(
            {"workspace": legacy_field_input_workspace(actor="unavailable-actor")},
            request,
        )

    def test_multi_target_semantics_accept_shared_real_tag_and_reject_wrong_tag(self):
        context = {
            "actorContextAvailable": True,
            "actors": [
                {"name": "\u9ed1\u6d1e", "tags": ["source"]},
                {"name": "\u5efa\u7b51A", "tags": ["building"]},
                {"name": "\u5efa\u7b51B", "tags": ["building"]},
            ],
        }
        request = self.service._normalize_payload(request_payload(
            instruction="\u8ba9\u9ed1\u6d1e\u6301\u7eed\u5438\u9644\u6240\u6709\u5efa\u7b51",
            responseLanguage="zh-CN",
            projectContext=context,
        ))
        workspace = control_workspace(include_jump=False)
        root = workspace["nodes"][1]["workspace"]["blocks"]["blocks"][0]
        root["inputs"]["DO"]["block"] = dynamic_tag_move_block(
            "building", "\u9ed1\u6d1e", "move-buildings"
        )
        self.service._validate_requested_semantics({"workspace": workspace}, request)

        root["inputs"]["DO"]["block"]["fields"]["TAG_TEXT"] = "unrelated"
        with self.assertRaisesRegex(ValueError, "\u591a\u76ee\u6807"):
            self.service._validate_requested_semantics({"workspace": workspace}, request)

    def test_follow_requirements_capture_dynamic_source_and_single_target(self):
        requirements = self.service._instruction_requirements(
            "make FollowerActor continuously follow LeaderActor",
            "create",
            {
                "actors": [
                    {"name": "LeaderActor", "type": "model"},
                    {"name": "FollowerActor", "type": "model"},
                ]
            },
        )
        self.assertEqual(["LeaderActor"], requirements["actors"]["source"])
        self.assertEqual(["FollowerActor"], requirements["actors"]["targets"])
        self.assertFalse(requirements["actors"]["multiTarget"])
        self.assertEqual([
            {
                "kind": "follow",
                "source": "LeaderActor",
                "targets": ["FollowerActor"],
                "axes": ["X", "Y", "Z"],
            }
        ], requirements["dynamicActorRelations"])

    def test_camera_target_is_parsed_independently_from_other_actor_roles(self):
        requirements = self.service._instruction_requirements(
            "make LeaderActor attract FollowerActor and have the camera follow FollowerActor",
            "create",
            {
                "actors": [
                    {"name": "LeaderActor", "type": "model", "semanticRole": "source"},
                    {"name": "FollowerActor", "type": "model"},
                ]
            },
        )
        self.assertEqual(["LeaderActor"], requirements["actors"]["source"])
        self.assertEqual(["FollowerActor"], requirements["actors"]["targets"])
        self.assertEqual(["FollowerActor"], requirements["cameraTargets"])

    def test_camera_follow_must_bind_the_requested_actor(self):
        request = self.service._normalize_payload(request_payload(
            instruction="make the camera continuously follow FollowerActor",
            responseLanguage="en-US",
            projectContext={
                "actorContextAvailable": True,
                "actors": [
                    {"name": "LeaderActor", "type": "model", "semanticRole": "source"},
                    {"name": "FollowerActor", "type": "model"},
                ],
            },
        ))
        self.service._validate_requested_semantics(
            {"workspace": camera_follow_workspace("FollowerActor")}, request
        )
        with self.assertRaisesRegex(ValueError, "Camera follow block"):
            self.service._validate_requested_semantics(
                {"workspace": camera_follow_workspace("LeaderActor")}, request
            )

    def test_dynamic_follow_rejects_fixed_world_coordinates(self):
        request = self.service._normalize_payload(request_payload(
            instruction=(
                "make FollowerActor continuously follow LeaderActor and hide FollowerActor "
                "when distance is less than 2"
            ),
            responseLanguage="en-US",
            projectContext={
                "actorContextAvailable": True,
                "actors": [
                    {"name": "LeaderActor", "type": "model"},
                    {"name": "FollowerActor", "type": "model"},
                ],
            },
        ))
        with self.assertRaisesRegex(ValueError, "Dynamic actor dependency"):
            self.service._validate_requested_semantics(
                {"workspace": dynamic_relation_workspace(live=False)}, request
            )

    def test_dynamic_follow_and_distance_accept_live_actor_dependencies(self):
        request = self.service._normalize_payload(request_payload(
            instruction=(
                "make FollowerActor continuously follow LeaderActor and hide FollowerActor "
                "when distance is less than 2"
            ),
            responseLanguage="en-US",
            projectContext={
                "actorContextAvailable": True,
                "actors": [
                    {"name": "LeaderActor", "type": "model"},
                    {"name": "FollowerActor", "type": "model"},
                ],
            },
        ))
        self.service._validate_requested_semantics(
            {"workspace": dynamic_relation_workspace()}, request
        )
        with self.assertRaisesRegex(ValueError, "Distance condition"):
            self.service._validate_requested_semantics(
                {"workspace": dynamic_relation_workspace(distance_live=False)}, request
            )

    def test_reachable_edge_condition_contributes_capability_types(self):
        workspace = control_workspace(include_jump=False)
        workspace["edges"][0]["conditionWorkspace"]["blocks"]["blocks"][0] = {
            "type": "detect_position_near",
            "id": "edge-distance",
            "fields": {
                "NAME_TEXT": "ball",
                "X_NUMBER": 0,
                "Y_NUMBER": 0,
                "Z_NUMBER": 0,
                "TOLERANCE_NUMBER": 1,
            },
        }
        self.assertIn("detect_position_near", self.service._reachable_block_types(workspace))

    def test_prompt_explains_macro_edges_and_exact_block_signatures(self):
        request = self.service._normalize_payload(request_payload(
            operation="edit",
            instruction="edit the movement logic",
            responseLanguage="en-US",
        ))
        _path, contract = self.service._load_contract()
        prompt = self.service._build_prompt(request, contract)
        self.assertIn("source:{nodeId,side,index}", prompt)
        self.assertIn("Use edges:[] when no macro transition is needed", prompt)
        self.assertIn("CONTRACT_BLOCK_SIGNATURES_JSON", prompt)
        self.assertIn('"control_if"', prompt)
        self.assertIn("Never add an input name", prompt)

    def test_unknown_input_repair_keeps_filtered_contract(self):
        self.assertFalse(self.service._needs_contract_expansion("uses unknown input TARGET_X"))
        self.assertFalse(self.service._needs_contract_expansion("uses unknown field TARGET_X"))
        self.assertTrue(self.service._needs_contract_expansion("unknown block type object_magic_follow"))

    def test_retry_expands_contract_only_for_contract_or_capability_failures(self):
        payload = request_payload(
            instruction="\u8ba9\u76f8\u673a\u8ddf\u968f Player",
            responseLanguage="zh-CN",
        )
        normalized = generated_result(payload, summary="\u5df2\u5b8c\u6210\u3002")
        settings = SimpleNamespace(
            api_key="test-key", base_url="https://api.deepseek.com", model="test", source="unit-test"
        )

        with mock.patch.object(NodeGraphGenerationService, "_call_deepseek", return_value="{}") as provider, mock.patch.object(
            NodeGraphGenerationService,
            "_validate_result",
            side_effect=[ValueError("\u4e2d\u6587\u8bf7\u6c42\u7684 summary \u5fc5\u987b\u4f7f\u7528\u4e2d\u6587"), normalized],
        ), mock.patch(
            "editor.plugins.AITool.services.node_graph_generation_service.NodeGraphReviewService._resolve_settings",
            return_value=settings,
        ):
            result = self.service.generate(payload)
        self.assertTrue(result["success"])
        self.assertEqual(2, provider.call_count)
        self.assertIn('"mode":"filtered"', provider.call_args_list[0].args[1])
        self.assertIn('"mode":"filtered"', provider.call_args_list[1].args[1])

        with mock.patch.object(NodeGraphGenerationService, "_call_deepseek", return_value="{}") as provider, mock.patch.object(
            NodeGraphGenerationService,
            "_validate_result",
            side_effect=[ValueError("\u751f\u6210\u8282\u70b9\u56fe\u7f3a\u5c11\u8bf7\u6c42\u7684\u5173\u952e\u80fd\u529b\uff1acamera-follow"), normalized],
        ), mock.patch(
            "editor.plugins.AITool.services.node_graph_generation_service.NodeGraphReviewService._resolve_settings",
            return_value=settings,
        ):
            result = self.service.generate(payload)
        self.assertTrue(result["success"])
        self.assertEqual(2, provider.call_count)
        self.assertNotIn('"mode":"filtered"', provider.call_args_list[1].args[1])

    def test_node_generation_call_uses_independent_settings(self):
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.__exit__.return_value = False
        response.read.return_value = json.dumps({
            "choices": [{"message": {"content": "{}"}}]
        }).encode("utf-8")
        settings = SimpleNamespace(
            api_key="test-secret",
            base_url="https://api.deepseek.com",
            model="node-model",
            temperature=0.03,
            max_tokens=9000,
            thinking_enabled=True,
        )
        with mock.patch(
            "editor.plugins.AITool.services.node_graph_generation_service.urllib.request.urlopen",
            return_value=response,
        ) as urlopen:
            self.assertEqual("{}", self.service._call_deepseek(settings, "prompt"))
        body = json.loads(urlopen.call_args.args[0].data.decode("utf-8"))
        self.assertEqual("node-model", body["model"])
        self.assertEqual(0.03, body["temperature"])
        self.assertEqual(9000, body["max_tokens"])
        self.assertEqual({"type": "enabled"}, body["thinking"])

    def test_missing_key_skips_provider_and_generation_errors_redact_key(self):
        empty_settings = SimpleNamespace(
            api_key="", base_url="https://api.deepseek.com", model="test", source="unit-test"
        )
        with mock.patch(
            "editor.plugins.AITool.services.node_graph_generation_service.NodeGraphReviewService._resolve_settings",
            return_value=empty_settings,
        ), mock.patch.object(NodeGraphGenerationService, "_call_deepseek") as provider:
            result = self.service.generate(request_payload())
        self.assertEqual("AI_NOT_CONFIGURED", result["error"])
        provider.assert_not_called()

        secret = "unit-test-secret"
        settings = SimpleNamespace(
            api_key=secret, base_url="https://api.deepseek.com", model="test", source="unit-test"
        )
        with mock.patch(
            "editor.plugins.AITool.services.node_graph_generation_service.NodeGraphReviewService._resolve_settings",
            return_value=settings,
        ), mock.patch.object(
            NodeGraphGenerationService, "_call_deepseek", side_effect=ValueError("provider leaked " + secret)
        ), self.assertLogs(
            "editor.plugins.AITool.services.node_graph_generation_service", level="WARNING"
        ) as logs:
            result = self.service.generate(request_payload())
        combined = "\n".join(logs.output) + json.dumps(result)
        self.assertNotIn(secret, combined)


if __name__ == "__main__":
    unittest.main()

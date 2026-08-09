"""DeepSeek-powered project node-graph generation for Cabbage Q&A.

The service consumes the trusted CoronaBlocks XML contract and returns a complete JSON
workspace.  It never writes project files and never generates Python; the mounted
NodeGraphWorkspace remains responsible for validation, code generation, atomic apply,
and persistence.
"""

from __future__ import annotations

from collections import Counter, deque
from concurrent.futures import ThreadPoolExecutor
import copy
import json
import logging
from pathlib import Path
import re
import socket
import threading
import time
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
import uuid
from typing import Any

from .node_graph_review_service import NodeGraphReviewService

try:
    from script_runtime.blockly.ai_node_graph_contract import (
        load_contract_catalog,
        validate_generated_node_graph,
    )
except ModuleNotFoundError as exc:
    # Tests and package consumers may import the editor as ``editor.*`` while
    # the embedded host exposes ``editor`` itself as the import root. Both
    # paths point to the same canonical script runtime package.
    if exc.name != "script_runtime":
        raise
    from editor.script_runtime.blockly.ai_node_graph_contract import (
        load_contract_catalog,
        validate_generated_node_graph,
    )

logger = logging.getLogger(__name__)


class NodeGraphGenerationService:
    """Asynchronous, single-worker node graph CRUD generation service."""

    TARGET_ID = "node_graph:project:global"
    VALID_OPERATIONS = {"create", "extend", "edit", "delete"}
    TIMEOUT_SECONDS = 90
    MAX_TASKS = 16
    MAX_INSTRUCTION_CHARS = 4000
    VALID_RESPONSE_LANGUAGES = {"zh-CN", "en-US"}
    FORBIDDEN_KEYS = {
        "python",
        "sourcecode",
        "generatedcode",
        "xml",
        "filepath",
        "actortarget",
        "scenetarget",
    }
    CORE_BLOCK_TYPES = {
        "node_when_enter", "node_while_active", "node_when_exit",
        "control_if", "control_else", "control_wait", "control_wait2",
        "control_for", "control_until",
        "logic_boolean", "logic_compare", "logic_operation", "logic_negate",
        "math_number", "math_arithmetic", "math_single", "math_constrain",
        "text", "object_reference",
        "variable_define", "variable_set", "variable_get", "variable_exists",
        "engine_X", "engine_Y", "engine_Z",
        "engine_Xset", "engine_Yset", "engine_Zset",
        "engine_Xadd", "engine_Yadd", "engine_Zadd",
        "object_set_position", "object_move_direction", "object_get_x", "object_get_y", "object_get_z",
    }
    CAPABILITY_BLOCK_TYPES = {
        "input-control": {
            "event_keyboard", "event_keyboard_combo", "detect_keyboard1", "detect_keyboard0",
            "object_third_person_move", "object_first_person_move", "object_arcade_jump",
        },
        "wasd-object-movement": {
            "object_third_person_move", "object_first_person_move",
        },
        "space-object-jump": {"object_arcade_jump"},
        "movement": {
            "engine_move", "engine_moveto", "engine_movetoXYZ", "engine_movetoXYZtime",
            "engine_Xset", "engine_Yset", "engine_Zset",
            "engine_Xadd", "engine_Yadd", "engine_Zadd",
            "object_set_position", "object_move_direction", "object_move_to_lane", "object_move_to_lane_smooth",
            "object_third_person_move", "object_first_person_move",
        },
        "physics": {
            "object_set_native_physics", "object_set_tag_velocity_axis",
        },
        "attraction": {
            "engine_X", "engine_Y", "engine_Z",
            "engine_Xadd", "engine_Yadd", "engine_Zadd",
            "object_get_x", "object_get_y", "object_get_z",
            "object_move_tag", "object_set_tag_velocity_axis",
            "math_arithmetic", "math_single", "math_constrain",
        },
        "distance-check": {
            "detect_position_near", "engine_X", "engine_Y", "engine_Z",
            "object_get_x", "object_get_y", "object_get_z",
            "math_arithmetic", "math_single", "logic_compare",
        },
        "threshold-action": {"logic_compare", "control_if", "math_number"},
        "object-removal": {
            "object_delete", "object_hide", "object_show",
            "object_delete_raycast_hit", "object_delete_mouse_pick",
        },
        "visibility": {"object_hide", "object_show", "appearance_hide", "appearance_show"},
        "rotation": {
            "engine_rotateX", "engine_rotateY", "engine_rotateZ",
            "engine_rotationX", "engine_rotationY", "engine_rotationZ",
        },
        "camera-follow": {
            "camera_follow_object", "camera_third_person_orbit", "camera_first_person_follow",
            "camera_lock_mouse", "camera_unlock_mouse",
        },
        "timer": {
            "control_wait", "control_wait2", "control_cooldown_ready",
            "control_start_cooldown", "control_reset_cooldown",
        },
        "state": {
            "variable_define", "variable_set", "variable_get", "variable_exists", "variable_add",
        },
        "multi-object": {
            "object_move_tag", "object_set_tag_velocity_axis", "object_set_tag",
            "object_count_tag", "object_count_active_tag", "object_tag_numbered_range",
            "object_reset_tag", "object_scatter_tag", "object_recycle_tag_axis",
        },
        "collision": {
            "detect_touch", "detect_touch_tag", "detect_touch_started",
            "detect_touch_tag_started", "object_set_logical_collision",
            "object_logical_collision_enabled",
        },
    }
    CAPABILITY_PATTERNS = (
        ("input-control", r"(?:wasd|keyboard|key\s*press|mouse|input|控制|输入|按键|键盘|鼠标)"),
        ("movement", r"(?:move|movement|motion|translate|移动|运动|位移|行走|朝.*(?:靠近|前进))"),
        ("physics", r"(?:physics|velocity|impulse|rigidbody|物理|速度|冲量|刚体)"),
        ("attraction", r"(?:attract|attraction|pull\s+towards?|suction|gravity\s*well|吸附|吸引|吸力|拉向|引力)"),
        ("distance-check", r"(?:distance|radius|near|proximity|距离|半径|靠近|附近|接近)"),
        ("threshold-action", r"(?:threshold|less\s+than|greater\s+than|within|阈值|小于|大于|以内|超过|达到|低于)"),
        ("object-removal", r"(?:destroy|remove\s+(?:the\s+)?object|consume|swallow|销毁|删除对象|吞噬|消失|移除)"),
        ("visibility", r"(?:hide|show|visible|invisible|隐藏|显示|可见|不可见)"),
        ("rotation", r"(?:rotate|rotation|spin|orbit|旋转|转动|自转)"),
        ("camera-follow", r"(?:camera.*follow|follow.*camera|相机.*跟随|跟随.*相机|镜头.*跟随)"),
        ("timer", r"(?:timer|cooldown|delay|every\s+\d|计时|冷却|延迟|每隔)"),
        ("state", r"(?:state|variable|counter|flag|状态|变量|计数|标记)"),
        ("multi-object", r"(?:all\s+(?:objects?|actors?|targets?)|every\s+(?:object|actor|target)|所有对象|全部对象|每个对象|所有建筑|全部建筑|多个|批量)"),
        ("collision", r"(?:collision|collide|touch|碰撞|接触|触碰)"),
    )
    CAPABILITY_VALIDATION_TYPES = {
        "input-control": {
            "event_keyboard", "event_keyboard_combo", "detect_keyboard1", "detect_keyboard0",
            "object_third_person_move", "object_first_person_move", "object_arcade_jump",
        },
        "movement": {
            "engine_move", "engine_moveto", "engine_movetoXYZ", "engine_movetoXYZtime",
            "engine_Xset", "engine_Yset", "engine_Zset",
            "engine_Xadd", "engine_Yadd", "engine_Zadd",
            "object_set_position", "object_move_direction", "object_move_to_lane", "object_move_to_lane_smooth",
            "object_third_person_move", "object_first_person_move",
        },
        "physics": {"object_set_native_physics", "object_set_tag_velocity_axis"},
        "attraction": {
            "engine_Xadd", "engine_Yadd", "engine_Zadd", "object_move_tag",
            "object_set_tag_velocity_axis", "object_set_position",
        },
        "distance-check": {
            "detect_position_near", "engine_X", "engine_Y", "engine_Z",
            "object_get_x", "object_get_y", "object_get_z",
        },
        "threshold-action": {"logic_compare", "control_if"},
        "object-removal": {"object_delete", "object_hide", "appearance_hide"},
        "visibility": {"object_hide", "object_show", "appearance_hide", "appearance_show"},
        "rotation": {"engine_rotateX", "engine_rotateY", "engine_rotateZ"},
        "camera-follow": {
            "camera_follow_object", "camera_third_person_orbit", "camera_first_person_follow",
        },
        "timer": {
            "control_wait", "control_wait2", "control_cooldown_ready",
            "control_start_cooldown", "control_reset_cooldown",
        },
        "state": {
            "variable_define", "variable_set", "variable_get", "variable_exists", "variable_add",
        },
        "multi-object": {
            "object_move_tag", "object_set_tag_velocity_axis", "object_set_tag",
            "object_count_tag", "object_count_active_tag", "object_tag_numbered_range",
            "object_reset_tag", "object_scatter_tag", "object_recycle_tag_axis",
        },
        "collision": {
            "detect_touch", "detect_touch_tag", "detect_touch_started",
            "detect_touch_tag_started", "object_set_logical_collision",
            "object_logical_collision_enabled",
        },
    }
    COORDINATE_GETTER_TYPES = {
        "engine_X": ("OBJECT", "X"),
        "engine_Y": ("OBJECT", "Y"),
        "engine_Z": ("OBJECT", "Z"),
        "object_get_x": ("NAME", "X"),
        "object_get_y": ("NAME", "Y"),
        "object_get_z": ("NAME", "Z"),
    }
    DYNAMIC_MOVEMENT_INPUTS = {
        "object_set_position": {"X": "X", "Y": "Y", "Z": "Z"},
        "engine_Xset": {"VALUE": "X"},
        "engine_Yset": {"VALUE": "Y"},
        "engine_Zset": {"VALUE": "Z"},
        "engine_Xadd": {"VALUE": "X"},
        "engine_Yadd": {"VALUE": "Y"},
        "engine_Zadd": {"VALUE": "Z"},
        "object_move_tag": {"DX": "X", "DY": "Y", "DZ": "Z"},
        "object_set_tag_velocity_axis": {"VALUE": "FIELD_AXIS"},
    }
    CAMERA_FOLLOW_TYPES = {
        "camera_follow_object", "camera_third_person_orbit", "camera_first_person_follow",
    }

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="NodeGraphGenerate")
        self._tasks: dict[str, dict[str, Any]] = {}
        self._closed = False
        self._contract_cache: tuple[int, str] | None = None

    @staticmethod
    def _error(code: str, message: str) -> dict[str, Any]:
        return {"success": False, "status": "error", "error": code, "message": message}

    @classmethod
    def _normalize_payload(cls, payload: Any) -> dict[str, Any]:
        if isinstance(payload, str):
            payload = json.loads(payload)
        if not isinstance(payload, dict):
            raise ValueError("节点生成请求必须是对象")

        request_id = str(payload.get("requestId") or "").strip()
        project_scope_id = str(payload.get("projectScopeId") or "").strip()
        base_revision = str(payload.get("baseGraphRevision") or "").strip()
        operation = str(payload.get("operation") or "create").strip().lower()
        instruction = str(payload.get("instruction") or "").strip()
        target_id = str(payload.get("targetId") or cls.TARGET_ID).strip()
        workspace = payload.get("workspace")
        project_context = payload.get("projectContext")
        response_language = str(payload.get("responseLanguage") or "").strip()

        if not request_id:
            raise ValueError("缺少 requestId")
        if not project_scope_id:
            raise ValueError("缺少 projectScopeId")
        if not base_revision:
            raise ValueError("缺少 baseGraphRevision")
        if target_id != cls.TARGET_ID:
            raise ValueError(f"targetId 必须为 {cls.TARGET_ID}")
        if operation not in cls.VALID_OPERATIONS:
            raise ValueError("operation 必须为 create、extend、edit 或 delete")
        if not instruction:
            raise ValueError("请输入要生成、制作或编辑的游戏逻辑")
        if not isinstance(workspace, dict):
            raise ValueError("缺少当前节点 workspace")
        if not isinstance(workspace.get("nodes"), list) or not isinstance(workspace.get("edges"), list):
            raise ValueError("workspace.nodes 和 workspace.edges 必须是数组")
        if not isinstance(workspace.get("globalVariablesWorkspace", {}), dict):
            raise ValueError("workspace.globalVariablesWorkspace 必须是对象")
        if not isinstance(project_context, dict):
            project_context = {}
        if response_language not in cls.VALID_RESPONSE_LANGUAGES:
            response_language = "zh-CN" if re.search(r"[\u3400-\u9fff]", instruction) else "en-US"

        return {
            "schemaVersion": 1,
            "requestId": request_id[:160],
            "targetId": cls.TARGET_ID,
            "projectScopeId": project_scope_id[:160],
            "baseGraphRevision": base_revision[:160],
            "operation": operation,
            "instruction": instruction[: cls.MAX_INSTRUCTION_CHARS],
            "responseLanguage": response_language,
            "workspace": json.loads(json.dumps(workspace, ensure_ascii=False)),
            "projectContext": cls._compact_project_context(project_context),
        }

    def _load_contract(self) -> tuple[Path, str]:
        path = NodeGraphReviewService._find_contract_path(__file__)
        if not path.is_file():
            raise ValueError(f"找不到节点积木 AI 合同：{path.name}")
        modified = path.stat().st_mtime_ns
        with self._lock:
            if self._contract_cache and self._contract_cache[0] == modified:
                return path, self._contract_cache[1]
        text = path.read_text(encoding="utf-8")
        if "<CoronaBlocksDocument" not in text or "<Catalog" not in text:
            raise ValueError("节点积木 AI 合同格式不正确")
        with self._lock:
            self._contract_cache = (modified, text)
        return path, text

    @staticmethod
    def _compact_project_context(project_context: dict[str, Any]) -> dict[str, Any]:
        if not isinstance(project_context, dict):
            return {}

        compact = {
            key: copy.deepcopy(project_context[key])
            for key in (
                "sceneName", "actorContextAvailable", "actorContextRevision",
                "assistanceProfile", "optimizationHintsEnabled",
            )
            if key in project_context
        }
        actors = project_context.get("actors")
        compact_actors: list[dict[str, Any]] = []
        if isinstance(actors, list):
            for actor in actors:
                if not isinstance(actor, dict):
                    continue
                name = str(actor.get("name") or "").strip()
                if not name:
                    continue
                item: dict[str, Any] = {
                    "name": name,
                    "type": str(actor.get("type") or "actor").strip() or "actor",
                    "tags": [str(value).strip() for value in (actor.get("tags") or []) if str(value).strip()]
                    if isinstance(actor.get("tags"), list)
                    else [],
                    "aliases": [str(value).strip() for value in (actor.get("aliases") or []) if str(value).strip()]
                    if isinstance(actor.get("aliases"), list)
                    else [],
                }
                for key in ("semanticRole", "transform", "size", "collision", "physicsEnabled"):
                    if key in actor and actor[key] is not None:
                        item[key] = copy.deepcopy(actor[key])
                compact_actors.append(item)
        compact["actors"] = compact_actors
        return compact

    @staticmethod
    def _actor_match_terms(actor: dict[str, Any]) -> list[str]:
        values: list[str] = []
        for field in ("name", "semanticRole"):
            value = str(actor.get(field) or "").strip()
            if value:
                values.append(value)
        for field in ("aliases", "tags"):
            raw_values = actor.get(field)
            if isinstance(raw_values, list):
                values.extend(str(value).strip() for value in raw_values if str(value).strip())

        terms: list[str] = []
        for value in values:
            terms.append(value)
            terms.extend(re.findall(r"[\u3400-\u9fff]{2,}", value))
            terms.extend(re.findall(r"[A-Za-z][A-Za-z0-9_-]{2,}", value))

        role_text = " ".join(values).casefold()
        role_aliases = (
            (
                r"(?:^|[\s_-])(?:attractor|controller|source|origin)(?:$|[\s_-])|"
                r"\u5438\u5f15\u6e90|\u63a7\u5236\u6e90|\u6765\u6e90",
                ("source object", "source actor", "\u6765\u6e90\u5bf9\u8c61", "\u5438\u5f15\u6e90", "\u63a7\u5236\u5bf9\u8c61"),
            ),
            (
                r"(?:^|[\s_-])(?:building|structure)s?(?:$|[\s_-])|\u5efa\u7b51",
                ("building", "buildings", "\u5efa\u7b51"),
            ),
            (
                r"(?:^|[\s_-])targets?(?:$|[\s_-])|\u76ee\u6807",
                ("target object", "target actor", "targets", "\u76ee\u6807\u5bf9\u8c61"),
            ),
        )
        for pattern, aliases in role_aliases:
            if re.search(pattern, role_text, re.IGNORECASE):
                terms.extend(aliases)
        return list(dict.fromkeys(term for term in terms if term))

    @staticmethod
    def _actor_term_position(lowered_instruction: str, term: str) -> int:
        lowered_term = str(term or "").casefold().strip()
        if not lowered_term:
            return -1
        if re.fullmatch(r"[a-z0-9_-]+", lowered_term):
            match = re.search(
                rf"(?<![a-z0-9_-]){re.escape(lowered_term)}(?![a-z0-9_-])",
                lowered_instruction,
            )
            return match.start() if match else -1
        return lowered_instruction.find(lowered_term)

    @staticmethod
    def _actor_has_source_role(actor: dict[str, Any]) -> bool:
        values = [str(actor.get("semanticRole") or "")]
        if isinstance(actor.get("tags"), list):
            values.extend(str(value) for value in actor["tags"])
        role_text = " ".join(values).casefold()
        return bool(re.search(
            r"(?:attractor|controller|source|origin|\u5438\u5f15\u6e90|\u63a7\u5236\u6e90|\u6765\u6e90)",
            role_text,
        ))

    @staticmethod
    def _actor_is_camera(actor: dict[str, Any]) -> bool:
        values = [
            str(actor.get("name") or ""),
            str(actor.get("type") or ""),
            str(actor.get("semanticRole") or ""),
        ]
        if isinstance(actor.get("tags"), list):
            values.extend(str(value) for value in actor["tags"])
        return bool(re.search(r"(?:camera|\u6444\u50cf\u673a|\u76f8\u673a|\u955c\u5934)", " ".join(values), re.IGNORECASE))

    @classmethod
    def _explicit_follow_relation(
        cls,
        text: str,
        actor_matches: list[tuple[int, int, str]],
        actors_by_name: dict[str, dict[str, Any]],
    ) -> dict[str, Any] | None:
        follow_pattern = r"(?:\bfollow(?:s|ing)?\b|\btrack(?:s|ing)?\b|\bchase(?:s|ing)?\b|\u8ddf\u968f|\u8ffd\u968f|\u8ffd\u8e2a)"
        camera_pattern = r"(?:camera|\u6444\u50cf\u673a|\u76f8\u673a|\u955c\u5934)"
        follow = None
        for candidate in re.finditer(follow_pattern, text, re.IGNORECASE):
            prefix = text[max(0, candidate.start() - 80):candidate.start()]
            suffix = text[candidate.end():min(len(text), candidate.end() + 40)]
            if re.search(camera_pattern, prefix, re.IGNORECASE) or re.search(camera_pattern, suffix, re.IGNORECASE):
                continue
            follow = candidate
            break
        if follow is None:
            return None
        before = [name for position, _index, name in actor_matches if position < follow.start()]
        after = [name for position, _index, name in actor_matches if position > follow.end()]
        before = list(dict.fromkeys(before))
        after = list(dict.fromkeys(after))
        if not before or not after:
            return None
        leader = after[0]
        followers = [name for name in before if name != leader]
        if not followers:
            return None
        if all(cls._actor_is_camera(actors_by_name.get(name, {})) for name in followers):
            return None
        return {"source": leader, "targets": followers}

    @classmethod
    def _explicit_camera_target(
        cls,
        text: str,
        actor_matches: list[tuple[int, int, str]],
        actors_by_name: dict[str, dict[str, Any]],
    ) -> str | None:
        camera_pattern = r"(?:camera|\u6444\u50cf\u673a|\u76f8\u673a|\u955c\u5934)"
        follow_pattern = r"(?:\bfollow(?:s|ing)?\b|\btrack(?:s|ing)?\b|\bchase(?:s|ing)?\b|\u8ddf\u968f|\u8ffd\u968f|\u8ffd\u8e2a)"
        relation = None
        for match in re.finditer(follow_pattern, text, re.IGNORECASE):
            prefix = text[max(0, match.start() - 80):match.start()]
            suffix = text[match.end():min(len(text), match.end() + 40)]
            if re.search(camera_pattern, prefix, re.IGNORECASE) or re.search(camera_pattern, suffix, re.IGNORECASE):
                relation = match
        if relation is None:
            return None

        after = [
            name
            for position, _index, name in actor_matches
            if position > relation.end() and not cls._actor_is_camera(actors_by_name.get(name, {}))
        ]
        if after:
            return list(dict.fromkeys(after))[0]

        before = [
            name
            for position, _index, name in actor_matches
            if position < relation.start() and not cls._actor_is_camera(actors_by_name.get(name, {}))
        ]
        return list(dict.fromkeys(before))[-1] if before else None

    @staticmethod
    def _dynamic_axes(instruction: str) -> list[str]:
        if re.search(r"(?:xz|x-z|\u6c34\u5e73|\u5730\u9762|\u5e73\u9762|\u4fef\u89c6|top[- ]?down)", instruction, re.IGNORECASE):
            return ["X", "Z"]
        return ["X", "Y", "Z"]

    @classmethod
    def _instruction_requirements(
        cls,
        instruction: str,
        operation: str | None = None,
        project_context: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        text = str(instruction or "")
        lowered = text.casefold()
        requires_wasd = "wasd" in lowered or bool(
            re.search(r"(?:前后左右|四方向|方向键)", text)
        )
        requires_space_jump = bool(
            ("space" in lowered or "空格" in text)
            and ("jump" in lowered or "跳" in text)
        )
        first_person_requested = bool(
            "first person" in lowered
            or "first-person" in lowered
            or "第一人称" in text
        )
        broad_demo_terms = re.compile(
            r"(?:完整游戏|整个游戏|游戏演示|demo|deno|"
            r"计分|生命|胜利|失败|敌人|战斗|关卡|"
            r"score|lives?|victory|defeat|enemy|combat|level)",
            re.IGNORECASE,
        )
        narrow_object_control = bool(
            (requires_wasd or requires_space_jump) and not broad_demo_terms.search(text)
        )

        capabilities: list[str] = []
        if requires_wasd:
            capabilities.extend(("input-control", "wasd-object-movement"))
        if requires_space_jump:
            capabilities.append("space-object-jump")
        for capability, pattern in cls.CAPABILITY_PATTERNS:
            if re.search(pattern, text, re.IGNORECASE):
                capabilities.append(capability)
        capabilities = list(dict.fromkeys(capabilities))

        replacement_match = re.search(
            r"(?:(?:将|把)\s*)?(.{1,80}?)\s*"
            r"(?:修改(?:成|为)|改(?:成|为)|"
            r"替换(?:成|为)|换(?:成|为)|"
            r"调整(?:成|为)|设置(?:成|为)|"
            r"设(?:成|为)|变(?:成|为))\s*"
            r"([^。！？!?\n]{1,80})",
            text,
            re.IGNORECASE,
        )
        replacement_directive = None
        if replacement_match:
            source = replacement_match.group(1).strip(" \t，,：:")
            source = re.sub(
                r"^(?:请(?:你)?|麻烦|帮我|帮忙|给我|替我|为我)\s*",
                "",
                source,
            )
            source = re.sub(r"^(?:将|把)\s*", "", source)
            replacement_directive = {
                "source": source,
                "target": replacement_match.group(2).strip(" \t，,：:"),
            }

        known_actors: list[dict[str, Any]] = []
        actors = (project_context or {}).get("actors") if isinstance(project_context, dict) else []
        if isinstance(actors, list):
            known_actors = [actor for actor in actors if isinstance(actor, dict)]
        actors_by_name = {
            str(actor.get("name") or "").strip(): actor
            for actor in known_actors
            if str(actor.get("name") or "").strip()
        }

        actor_matches: list[tuple[int, int, str]] = []
        for actor_index, actor in enumerate(known_actors):
            name = str(actor.get("name") or "").strip()
            if not name:
                continue
            positions = [
                position
                for term in cls._actor_match_terms(actor)
                if (position := cls._actor_term_position(lowered, term)) >= 0
            ]
            if positions:
                actor_matches.append((min(positions), actor_index, name))
        actor_matches.sort(key=lambda item: (item[0], item[1]))
        mentioned = list(dict.fromkeys(name for _position, _index, name in actor_matches))
        follow_relation = cls._explicit_follow_relation(text, actor_matches, actors_by_name)
        camera_target = cls._explicit_camera_target(text, actor_matches, actors_by_name)
        if follow_relation and "camera-follow" not in capabilities and "movement" not in capabilities:
            capabilities.append("movement")

        source_capability = any(
            item in capabilities for item in ("attraction", "rotation")
        )
        role_sources = [
            str(actor.get("name") or "").strip()
            for actor in known_actors
            if str(actor.get("name") or "").strip() and cls._actor_has_source_role(actor)
        ]
        mentioned_role_sources = [name for name in mentioned if name in role_sources]
        generic_source_requested = bool(re.search(
            r"(?:source\s+(?:object|actor)|controller|attractor|"
            r"\u6765\u6e90\u5bf9\u8c61|\u5438\u5f15\u6e90|\u63a7\u5236\u6e90)",
            text,
            re.IGNORECASE,
        ))
        source: list[str] = []
        if follow_relation:
            source = [str(follow_relation["source"])]
        elif source_capability:
            if mentioned_role_sources:
                source = mentioned_role_sources[:1]
            elif generic_source_requested and len(role_sources) == 1:
                source = role_sources[:1]
            elif mentioned:
                source = mentioned[:1]
        elif "camera-follow" in capabilities:
            if camera_target:
                source = [camera_target]
            elif mentioned_role_sources:
                source = mentioned_role_sources[:1]
            elif generic_source_requested and len(role_sources) == 1:
                source = role_sources[:1]
            elif mentioned:
                source = mentioned[:1]
        explicit_actor_list = len(mentioned) >= 2 and bool(re.search(
            r"(?:,|\uFF0C|\u3001|\band\b|\u548c|\u4e0e|\u53ca)",
            text,
            re.IGNORECASE,
        ))
        target_candidates = [name for name in mentioned if name not in source]
        multi_target = (
            "multi-object" in capabilities
            or len(target_candidates) >= 2
            or (explicit_actor_list and not source_capability)
        )
        all_objects_requested = bool(re.search(
            r"(?:all\s+(?:objects?|actors?|targets?)|every\s+(?:object|actor|target)|\u6240\u6709\u5bf9\u8c61|\u5168\u90e8\u5bf9\u8c61|\u6bcf\u4e2a\u5bf9\u8c61)",
            text,
            re.IGNORECASE,
        ))
        targets = target_candidates
        if follow_relation:
            targets = [str(name) for name in follow_relation.get("targets") or [] if str(name)]
            multi_target = len(targets) >= 2
        if multi_target and all_objects_requested:
            for actor in known_actors:
                name = str(actor.get("name") or "").strip()
                if name and name not in source and name not in targets:
                    targets.append(name)
        actor_requirements = {
            "mentioned": mentioned,
            "control": mentioned[:1] if any(
                item in capabilities for item in ("input-control", "wasd-object-movement", "movement")
            ) else [],
            "source": source,
            "targets": targets,
            "multiTarget": multi_target,
        }

        parameter_patterns = {
            "speed": r"(?:速度|speed)\s*(?:为|=|:)?\s*(-?\d+(?:\.\d+)?)",
            "radius": r"(?:半径|radius)\s*(?:为|=|:)?\s*(-?\d+(?:\.\d+)?)",
            "force": r"(?:力度|力量|吸力|force|strength)\s*(?:为|=|:)?\s*(-?\d+(?:\.\d+)?)",
            "distance": r"(?:距离|distance)\s*(?:小于|大于|为|=|:)?\s*(-?\d+(?:\.\d+)?)",
            "duration": r"(?:时间|时长|秒|duration|seconds?)\s*(?:为|=|:)?\s*(-?\d+(?:\.\d+)?)",
        }
        parameters: dict[str, Any] = {}
        for name, pattern in parameter_patterns.items():
            match = re.search(pattern, text, re.IGNORECASE)
            if match:
                value = float(match.group(1))
                parameters[name] = int(value) if value.is_integer() else value
        numeric_mentions = []
        for match in re.finditer(r"-?\d+(?:\.\d+)?", text):
            value = float(match.group(0))
            numeric_mentions.append({
                "value": int(value) if value.is_integer() else value,
                "context": text[max(0, match.start() - 12): min(len(text), match.end() + 12)],
            })
        if numeric_mentions:
            parameters["numericMentions"] = numeric_mentions[:12]
        defaults_required = []
        if "movement" in capabilities and "speed" not in parameters:
            defaults_required.append("speed")
        if "attraction" in capabilities and "force" not in parameters:
            defaults_required.append("force")
        if "distance-check" in capabilities and not ({"distance", "radius"} & parameters.keys()):
            defaults_required.append("distanceThreshold")
        parameters["defaultsRequired"] = defaults_required

        lifecycle = []
        if re.search(r"(?:开始时|进入时|初始化|on\s+start|initialize)", text, re.IGNORECASE):
            lifecycle.append("on-enter")
        if re.search(r"(?:持续|每帧|实时|不断|一直|while|continuously|constantly)", text, re.IGNORECASE) or any(
            item in capabilities for item in ("movement", "attraction", "rotation", "camera-follow")
        ):
            lifecycle.append("while-active")
        if "input-control" in capabilities:
            lifecycle.append("input-triggered")
        if not lifecycle:
            lifecycle.append("on-enter")

        capability_alternatives: list[list[str]] = []
        hide_or_destroy = bool(
            re.search(
                r"(?:\u9690\u85cf|hide)\s*(?:\u6216|\u6216\u8005|/|or)\s*(?:\u9500\u6bc1|\u5220\u9664|destroy|remove)"
                r"|(?:\u9500\u6bc1|\u5220\u9664|destroy|remove)\s*(?:\u6216|\u6216\u8005|/|or)\s*(?:\u9690\u85cf|hide)",
                text,
                re.IGNORECASE,
            )
        )
        if hide_or_destroy:
            capability_alternatives.append(["visibility", "object-removal"])

        dynamic_relations: list[dict[str, Any]] = []
        relation_axes = cls._dynamic_axes(text)
        if follow_relation:
            dynamic_relations.append({
                "kind": "follow",
                "source": str(follow_relation["source"]),
                "targets": [str(name) for name in follow_relation.get("targets") or [] if str(name)],
                "axes": relation_axes,
            })
        elif "attraction" in capabilities and source and targets:
            dynamic_relations.append({
                "kind": "attraction",
                "source": source[0],
                "targets": targets,
                "axes": relation_axes,
            })

        distance_relations: list[dict[str, Any]] = []
        if "distance-check" in capabilities:
            if source and targets:
                distance_relations.append({
                    "source": source[0],
                    "targets": targets,
                    "axes": relation_axes,
                })
            elif len(mentioned) >= 2:
                distance_relations.append({
                    "source": mentioned[0],
                    "targets": mentioned[1:],
                    "axes": relation_axes,
                })

        return {
            "operation": operation or "create",
            "actors": actor_requirements,
            "capabilities": capabilities,
            "requiredCapabilities": capabilities,
            "capabilityAlternatives": capability_alternatives,
            "parameters": parameters,
            "lifecycle": list(dict.fromkeys(lifecycle)),
            "constraints": [
                "use-only-existing-scene-actors",
                "use-only-contracted-blocks",
                "return-complete-workspace",
                "preserve-unrelated-logic",
            ],
            "narrowObjectControl": narrow_object_control,
            "firstPersonRequested": first_person_requested,
            "replacementDirective": replacement_directive,
            "dynamicActorRelations": dynamic_relations,
            "distanceActorRelations": distance_relations,
            "cameraTargets": ([camera_target] if camera_target else source[:1]) if "camera-follow" in capabilities else [],
        }

    @staticmethod
    def _contract_block_text(block: ET.Element) -> str:
        values = [str(value or "") for value in block.attrib.values()]
        for child in block:
            values.extend(str(value or "") for value in child.attrib.values())
        return " ".join(values).casefold()

    @staticmethod
    def _contract_signature_index(contract_text: str) -> dict[str, Any]:
        root = ET.fromstring(contract_text)
        signatures: dict[str, Any] = {}
        for block in root.findall("./Catalog/Blocks/Block"):
            block_type = str(block.get("type") or "").strip()
            if not block_type:
                continue
            signatures[block_type] = {
                "shape": str(block.get("shape") or ""),
                "output": str(block.get("outputCheck") or ""),
                "capabilities": str(block.get("capabilities") or "").split(),
                "aiUse": str(block.get("aiUse") or "").strip(),
                "fields": [
                    str(field.get("name") or "").strip()
                    for field in block.findall("./Field")
                    if str(field.get("name") or "").strip()
                ],
                "inputs": {
                    str(input_node.get("name") or "").strip(): {
                        "kind": str(input_node.get("kind") or ""),
                        "check": str(input_node.get("check") or ""),
                    }
                    for input_node in block.findall("./Input")
                    if str(input_node.get("name") or "").strip()
                },
            }
        return signatures

    @classmethod
    def _select_contract(
        cls,
        request: dict[str, Any],
        contract_path: Path,
        contract_text: str,
        requirements: dict[str, Any],
        *,
        expanded: bool = False,
    ) -> dict[str, Any]:
        catalog = load_contract_catalog(contract_path)
        specs = catalog.get("blocks") if isinstance(catalog, dict) else {}
        root = ET.fromstring(contract_text)
        block_elements = root.findall("./Catalog/Blocks/Block")
        block_by_type = {
            str(block.get("type") or "").strip(): block
            for block in block_elements
            if str(block.get("type") or "").strip()
        }
        project_safe = {
            block_type
            for block_type, spec in specs.items()
            if getattr(spec, "project_usage", "") == "project-safe"
        }
        selected = set(cls.CORE_BLOCK_TYPES) & project_safe
        capabilities = set(requirements.get("capabilities") or [])
        for capability in capabilities:
            selected.update(cls.CAPABILITY_BLOCK_TYPES.get(capability, set()) & project_safe)
            selected.update(
                block_type
                for block_type, spec in specs.items()
                if block_type in project_safe
                and capability in set(getattr(spec, "capabilities", ()) or ())
            )

        existing_types = set(cls._graph_block_types(request.get("workspace") or {}))
        selected.update(existing_types & project_safe)

        tokens = {
            token
            for token in re.findall(r"[a-zA-Z][a-zA-Z0-9_-]{2,}", request.get("instruction") or "")
            if token.casefold() not in {
                "the", "and", "for", "with", "this", "that", "from", "into", "make", "create",
            }
        }
        direct_matches: list[tuple[int, str]] = []
        lowered_instruction = str(request.get("instruction") or "").casefold()
        for block_type, block in block_by_type.items():
            if block_type not in project_safe:
                continue
            metadata = cls._contract_block_text(block)
            score = sum(1 for token in tokens if token.casefold() in metadata)
            if block_type.casefold() in lowered_instruction:
                score += 4
            if score:
                direct_matches.append((score, block_type))
        direct_matches.sort(key=lambda item: (-item[0], item[1]))
        selected.update(block_type for _score, block_type in direct_matches[:16])

        # Existing blocks must be preserved when the task is understood, but they do
        # not prove that the selector understood an otherwise unknown instruction.
        recognized = bool(capabilities or direct_matches)
        if not recognized:
            return {
                "text": contract_text,
                "mode": "full",
                "selectedTypes": sorted(block_by_type),
            }

        if expanded:
            selected_categories = {
                str(block_by_type[block_type].get("category") or "")
                for block_type in selected
                if block_type in block_by_type
            }
            for block_type, block in block_by_type.items():
                if block_type not in project_safe:
                    continue
                if (
                    str(block.get("category") or "") in selected_categories
                    or str(block.get("recommended") or "").casefold() == "true"
                ):
                    selected.add(block_type)

        selected &= set(block_by_type)
        if len(selected) < 8:
            return {
                "text": contract_text,
                "mode": "full",
                "selectedTypes": sorted(block_by_type),
            }

        filtered_root = copy.deepcopy(root)
        blocks_parent = filtered_root.find("./Catalog/Blocks")
        if blocks_parent is None:
            raise ValueError("节点 AI 合同缺少 Catalog/Blocks")
        for block in list(blocks_parent):
            if str(block.get("type") or "").strip() not in selected:
                blocks_parent.remove(block)
        filtered_root.set("selectionMode", "expanded" if expanded else "filtered")
        filtered_root.set("selectedBlockCount", str(len(selected)))
        try:
            ET.indent(filtered_root, space="  ")
        except AttributeError:
            pass
        return {
            "text": ET.tostring(filtered_root, encoding="unicode"),
            "mode": "expanded" if expanded else "filtered",
            "selectedTypes": sorted(selected),
        }

    @staticmethod
    def _build_prompt(
        request: dict[str, Any],
        contract_text: str,
        requirements: dict[str, Any] | None = None,
        *,
        contract_mode: str = "full",
        selected_types: list[str] | None = None,
    ) -> str:
        requirements = requirements or NodeGraphGenerationService._instruction_requirements(
            request["instruction"], request.get("operation"), request.get("projectContext")
        )
        request_envelope = {
            "schemaVersion": request["schemaVersion"],
            "requestId": request["requestId"],
            "targetId": request["targetId"],
            "projectScopeId": request["projectScopeId"],
            "baseGraphRevision": request["baseGraphRevision"],
            "operation": request["operation"],
            "instruction": request["instruction"],
            "responseLanguage": request["responseLanguage"],
        }
        scoped_rules = []
        if requirements["narrowObjectControl"]:
            scoped_rules.append(
                "This is a narrow object-control request. Modify only the minimum relevant node logic. "
                "Do not add score, lives, victory, defeat, combat, enemy, or complete-demo state templates."
            )
        if "wasd-object-movement" in requirements["requiredCapabilities"]:
            scoped_rules.append(
                "The final reachable node_while_active DO chain must contain object_third_person_move "
                "(or object_first_person_move only when first-person control is explicitly requested). "
                "Never use object_set_tag_velocity_axis as a substitute for single-object WASD control."
            )
        if "space-object-jump" in requirements["requiredCapabilities"]:
            scoped_rules.append(
                "The same reachable node_while_active DO chain must also contain object_arcade_jump. "
                "Its NAME must exactly equal the movement block NAME."
            )
        for relation in requirements.get("dynamicActorRelations") or []:
            if not isinstance(relation, dict):
                continue
            scoped_rules.append(
                "Dynamic actor relation: each movement target "
                + ", ".join(str(value) for value in relation.get("targets") or [])
                + " must read the live "
                + ", ".join(str(value) for value in relation.get("axes") or [])
                + " coordinates of "
                + str(relation.get("source") or "")
                + ". Fixed world coordinates are not a valid substitute."
            )
        for relation in requirements.get("distanceActorRelations") or []:
            if not isinstance(relation, dict):
                continue
            scoped_rules.append(
                "Every requested distance/proximity condition must contain both the source actor "
                + str(relation.get("source") or "")
                + " and each target actor in the same Boolean condition, using live actor coordinates."
            )
        if requirements.get("cameraTargets"):
            scoped_rules.append(
                "Every requested camera-follow block must bind exactly to "
                + str((requirements.get("cameraTargets") or [""])[0])
                + "."
            )
        scoped_text = "\n".join(f"- {item}" for item in scoped_rules) or "- Follow only the explicit user request."
        operation_rules = {
            "create": (
                "Create the requested logic as a complete valid final workspace. Reuse useful existing logic "
                "when it already satisfies part of the request."
            ),
            "extend": (
                "Extend the current workspace in place. Keep every existing unrelated node, edge, block, "
                "condition, global variable, ID, and canvas position; add or connect only the minimum requested logic."
            ),
            "edit": (
                "Edit the current workspace in place as a targeted transformation. Locate the existing nodes, "
                "blocks, fields, object references, edges, or conditions described by the instruction and change "
                "only those matches. Preserve unrelated logic and preserve existing IDs, node positions, edge "
                "connections, and block order whenever they are not the requested target. Do not clear or rebuild "
                "the graph. For phrases such as modify-to/change-to/replace-with, treat the right-hand value as "
                "the replacement and update the matching existing value rather than generating a second parallel feature."
            ),
            "delete": (
                "Delete only the explicitly requested nodes, blocks, edges, or conditions. Preserve all unrelated "
                "logic and repair only connections made invalid by that deletion."
            ),
        }
        language = request["responseLanguage"]
        contract_label = "FULL_CORONA_BLOCKS_CONTRACT_XML" if contract_mode == "full" else "SELECTED_CORONA_BLOCKS_CONTRACT_XML"
        selection_summary = {
            "mode": contract_mode,
            "selectedBlockCount": len(selected_types or []),
            "selectedBlockTypes": selected_types or [],
        }
        signature_index = NodeGraphGenerationService._contract_signature_index(contract_text)
        return (
            "You are editing CoronaEngine's visible project node graph. Follow the trusted XML contract supplied below.\n"
            "Return exactly one JSON object containing the complete final workspace. Never return a patch, Python, XML, "
            "Markdown, file path, or prose outside the JSON. Use only catalog blocks. Preserve unrelated existing logic "
            "for extend/edit/delete. Never invent an actor: object names must exactly match PROJECT_CONTEXT_JSON.actors.\n"
            "The project node graph is already scoped to the current Native Editor scene. An empty graph-level actor "
            "binding is expected and must never trigger a scene-binding workflow. Never ask the user to bind a scene or actor. "
            "For movement, jump, rotation, collision, physics, and other object operations, choose the intended concrete target "
            "from PROJECT_CONTEXT_JSON.actors and serialize its exact name into the supported object field or object input.\n\n"
            "OUTPUT_AND_OPERATION_RULES:\n"
            + f"- {operation_rules[request['operation']]}\n"
            + "- Implement only capabilities explicitly requested by the user. Do not expand a small feature into a full game.\n"
            + "- XML examples are structural illustrations only and must never be copied as gameplay templates.\n"
            + "- Prefer the smallest valid graph change and reuse an existing reachable gameplay node when practical.\n"
            + scoped_text
            + "\n\nBLOCKLY_SERIALIZATION_GUARDRAILS:\n"
            "- fields contains only real Catalog <Field> names. inputs contains only real Catalog <Input> names. "
            "Never serialize an input socket as a field.\n"
            "- control_if has no BOOL field. Put a Boolean output block under inputs.CONDITION.block.\n"
            "- engine_rotateX/engine_rotateY/engine_rotateZ have only the ANGLE field. Bind the target through "
            "inputs.OBJECT.block using object_reference, and choose an exact actor name from PROJECT_CONTEXT_JSON.\n"
            "- inputs.DO is a statement connection. Put the first action in inputs.DO.block and continue that branch "
            "with next.block. Never put branch actions beside DO or inside fields.\n"
            "- When editing an existing block, preserve its id and type. Change only documented field values or "
            "documented input connections. Never add an input name that is absent from CONTRACT_BLOCK_SIGNATURES_JSON.\n"
            "- workspace.edges entries use this exact macro schema: {id,name,source:{nodeId,side,index},"
            "target:{nodeId,side,index},conditionWorkspace}. source and target are objects, side is left/right/bottom, "
            "and index is a non-negative integer. Use edges:[] when no macro transition is needed.\n"
            "- conditionWorkspace is either {} or a Blockly workspace with exactly one top-level Boolean output block. "
            "Do not place gameplay statement blocks in an edge condition.\n"
            "- Before returning JSON, check every block against its Catalog entry: every field name, input name, "
            "connection kind, output type, and dropdown value must match exactly.\n\n"
            "CONTRACT_BLOCK_SIGNATURES_JSON:\n"
            + json.dumps(signature_index, ensure_ascii=False, separators=(",", ":"))
            + "\n\nLANGUAGE_RULES:\n"
            + f"- responseLanguage is {language}. The summary and every newly added or renamed custom node/edge label must use that language.\n"
            + "- Do not mix Chinese and English UI labels. Technical identifiers and real actor names must not be translated.\n\n"
            "REQUEST_ENVELOPE_JSON:\n"
            + json.dumps(request_envelope, ensure_ascii=False, separators=(",", ":"))
            + "\n\nDERIVED_REQUIREMENTS_JSON:\n"
            + json.dumps(requirements, ensure_ascii=False, separators=(",", ":"))
            + "\n\nCURRENT_WORKSPACE_JSON:\n"
            + json.dumps(request["workspace"], ensure_ascii=False, separators=(",", ":"))
            + "\n\nPROJECT_CONTEXT_JSON:\n"
            + json.dumps(request["projectContext"], ensure_ascii=False, separators=(",", ":"))
            + "\n\nCONTRACT_SELECTION_JSON:\n"
            + json.dumps(selection_summary, ensure_ascii=False, separators=(",", ":"))
            + f"\n\n{contract_label}:\n"
            + contract_text
        )

    @classmethod
    def _call_deepseek(cls, settings: Any, prompt: str) -> str:
        base = str(settings.base_url or "").rstrip("/")
        endpoint = base if base.endswith("/chat/completions") else base + "/chat/completions"
        body = {
            "model": settings.model,
            "temperature": float(getattr(settings, "temperature", 0.05)),
            "max_tokens": int(getattr(settings, "max_tokens", 12000)),
            "response_format": {"type": "json_object"},
            "thinking": {
                "type": "enabled" if getattr(settings, "thinking_enabled", False) else "disabled"
            },
            "messages": [
                {
                    "role": "system",
                    "content": (
                        "你是 CoronaEngine 内嵌节点图编辑器。你只输出一个 JSON 对象。"
                        "必须根据本次提供的 XML 合同对 node_graph:project:global 做增删改查，返回完整最终节点图。"
                        "禁止输出 Python、XML、文件路径、JSON Patch、Markdown 或合同外积木。"
                    ),
                },
                {"role": "user", "content": prompt},
            ],
        }
        http_request = urllib.request.Request(
            endpoint,
            data=json.dumps(body, ensure_ascii=False).encode("utf-8"),
            headers={
                "Authorization": "Bearer " + settings.api_key,
                "Content-Type": "application/json",
            },
            method="POST",
        )
        with urllib.request.urlopen(http_request, timeout=cls.TIMEOUT_SECONDS) as response:
            data = json.loads(response.read().decode("utf-8"))
        choices = data.get("choices") if isinstance(data, dict) else None
        first = choices[0] if isinstance(choices, list) and choices and isinstance(choices[0], dict) else {}
        message = first.get("message") if isinstance(first.get("message"), dict) else {}
        content = message.get("content")
        if not isinstance(content, str) or not content.strip():
            raise ValueError("DeepSeek 返回了空内容")
        return content.strip()

    @classmethod
    def _contains_forbidden_key(cls, value: Any) -> str:
        if isinstance(value, dict):
            for key, child in value.items():
                normalized = str(key).replace("_", "").lower()
                if normalized in cls.FORBIDDEN_KEYS:
                    return str(key)
                found = cls._contains_forbidden_key(child)
                if found:
                    return found
        elif isinstance(value, list):
            for child in value:
                found = cls._contains_forbidden_key(child)
                if found:
                    return found
        return ""

    @staticmethod
    def _workspace_roots(workspace: Any) -> list[dict[str, Any]]:
        if not isinstance(workspace, dict):
            return []
        container = workspace.get("blocks")
        if not isinstance(container, dict):
            return []
        roots = container.get("blocks")
        return [item for item in roots if isinstance(item, dict)] if isinstance(roots, list) else []

    @classmethod
    def _walk_block(cls, block: Any):
        if not isinstance(block, dict):
            return
        yield block
        inputs = block.get("inputs")
        if isinstance(inputs, dict):
            for connection in inputs.values():
                if not isinstance(connection, dict):
                    continue
                for key in ("block", "shadow"):
                    child = connection.get(key)
                    if isinstance(child, dict):
                        yield from cls._walk_block(child)
        connection = block.get("next")
        if isinstance(connection, dict):
            for key in ("block", "shadow"):
                child = connection.get(key)
                if isinstance(child, dict):
                    yield from cls._walk_block(child)

    @classmethod
    def _workspace_blocks(cls, workspace: Any):
        for root in cls._workspace_roots(workspace):
            yield from cls._walk_block(root)

    @staticmethod
    def _all_graph_workspaces(workspace: Any):
        if not isinstance(workspace, dict):
            return
        nodes = workspace.get("nodes")
        if isinstance(nodes, list):
            for node in nodes:
                if isinstance(node, dict):
                    yield node.get("workspace")
        edges = workspace.get("edges")
        if isinstance(edges, list):
            for edge in edges:
                if isinstance(edge, dict):
                    yield edge.get("conditionWorkspace")
        yield workspace.get("globalVariablesWorkspace")

    @classmethod
    def _normalize_model_block_serialization(
        cls, result: dict[str, Any], contract_path: Path
    ) -> tuple[dict[str, Any], list[str]]:
        """Repair deterministic field/input confusions, then require strict validation."""
        normalized = json.loads(json.dumps(result, ensure_ascii=False))
        workspace = normalized.get("workspace")
        if not isinstance(workspace, dict):
            return normalized, []

        catalog = load_contract_catalog(contract_path)
        block_specs = catalog.get("blocks", {})
        used_ids: set[str] = set()
        for graph_workspace in cls._reachable_node_workspaces(workspace):
            for block in cls._workspace_blocks(graph_workspace):
                block_id = str(block.get("id") or "").strip()
                if block_id:
                    used_ids.add(block_id)

        repair_sequence = 0

        def repair_id(prefix: str) -> str:
            nonlocal repair_sequence
            while True:
                repair_sequence += 1
                candidate = f"ai_repair_{prefix}_{repair_sequence}"
                if candidate not in used_ids:
                    used_ids.add(candidate)
                    return candidate

        repairs: list[str] = []

        def normalize_block(block: Any, trail: str) -> None:
            if not isinstance(block, dict):
                return
            block_type = str(block.get("type") or "").strip()
            spec = block_specs.get(block_type)
            fields = block.get("fields")
            inputs = block.get("inputs")
            if not isinstance(fields, dict):
                fields = None
            if inputs is None:
                inputs = {}
                block["inputs"] = inputs
            elif not isinstance(inputs, dict):
                inputs = None

            # BOOL belongs to logic_boolean, never directly to control_if/control_else.
            if (
                block_type in {"control_if", "control_else"}
                and fields is not None
                and "BOOL" in fields
                and inputs is not None
            ):
                bool_value = fields.pop("BOOL")
                if "CONDITION" not in inputs:
                    inputs["CONDITION"] = {
                        "block": {
                            "type": "logic_boolean",
                            "id": repair_id("condition"),
                            "fields": {"BOOL": bool_value},
                        }
                    }
                    repairs.append(f"{trail}: moved BOOL into inputs.CONDITION.logic_boolean")
                else:
                    repairs.append(f"{trail}: removed redundant BOOL field from {block_type}")

            # Generic transforms expose OBJECT as a String value input, not a field.
            if fields is not None and inputs is not None and spec is not None and "OBJECT" in fields:
                object_input = spec.inputs.get("OBJECT")
                object_is_not_field = "OBJECT" not in spec.fields
                object_is_string_input = bool(
                    object_input
                    and object_input.get("kind") == "value"
                    and "String" in tuple(object_input.get("check") or ())
                )
                object_name = fields.get("OBJECT")
                if object_is_not_field and object_is_string_input and isinstance(object_name, str):
                    fields.pop("OBJECT")
                    if "OBJECT" not in inputs and object_name.strip():
                        inputs["OBJECT"] = {
                            "block": {
                                "type": "object_reference",
                                "id": repair_id("object"),
                                "fields": {"OBJECT": object_name.strip(), "MANUAL": ""},
                            }
                        }
                        repairs.append(f"{trail}: moved OBJECT into inputs.OBJECT.object_reference")
                    else:
                        repairs.append(f"{trail}: removed redundant OBJECT field from {block_type}")

            if fields == {}:
                block.pop("fields", None)
            if inputs == {}:
                block.pop("inputs", None)
                inputs = None

            if isinstance(inputs, dict):
                for input_name, connection in inputs.items():
                    if not isinstance(connection, dict):
                        continue
                    for connection_key in ("block", "shadow"):
                        child = connection.get(connection_key)
                        if isinstance(child, dict):
                            normalize_block(child, f"{trail}.inputs.{input_name}.{connection_key}")
            next_connection = block.get("next")
            if isinstance(next_connection, dict):
                child = next_connection.get("block")
                if isinstance(child, dict):
                    normalize_block(child, f"{trail}.next.block")

        for graph_index, graph_workspace in enumerate(cls._all_graph_workspaces(workspace)):
            for root_index, root in enumerate(cls._workspace_roots(graph_workspace)):
                normalize_block(root, f"workspace[{graph_index}].blocks[{root_index}]")
        return normalized, repairs

    @classmethod
    def _graph_block_types(cls, workspace: dict[str, Any]) -> Counter:
        block_types: Counter = Counter()
        nodes = workspace.get("nodes") if isinstance(workspace, dict) else []
        if isinstance(nodes, list):
            for node in nodes:
                if isinstance(node, dict):
                    for block in cls._workspace_blocks(node.get("workspace")):
                        block_type = str(block.get("type") or "").strip()
                        if block_type:
                            block_types[block_type] += 1
        edges = workspace.get("edges") if isinstance(workspace, dict) else []
        if isinstance(edges, list):
            for edge in edges:
                if isinstance(edge, dict):
                    for block in cls._workspace_blocks(edge.get("conditionWorkspace")):
                        block_type = str(block.get("type") or "").strip()
                        if block_type:
                            block_types[block_type] += 1
        for block in cls._workspace_blocks(
            workspace.get("globalVariablesWorkspace") if isinstance(workspace, dict) else None
        ):
            block_type = str(block.get("type") or "").strip()
            if block_type:
                block_types[block_type] += 1
        return block_types

    @classmethod
    def _reachable_node_workspaces(cls, workspace: dict[str, Any]) -> list[dict[str, Any]]:
        nodes = [item for item in (workspace.get("nodes") or []) if isinstance(item, dict)]
        edges = [item for item in (workspace.get("edges") or []) if isinstance(item, dict)]
        node_by_id = {
            str(node.get("id") or "").strip(): node
            for node in nodes
            if str(node.get("id") or "").strip()
        }
        starts = [
            node_id
            for node_id, node in node_by_id.items()
            if str(node.get("nodeType") or "").strip() == "start"
        ]
        adjacency: dict[str, set[str]] = {}
        for edge in edges:
            source = edge.get("source") if isinstance(edge.get("source"), dict) else {}
            target = edge.get("target") if isinstance(edge.get("target"), dict) else {}
            source_id = str(source.get("nodeId") or "").strip()
            target_id = str(target.get("nodeId") or "").strip()
            if source_id and target_id:
                adjacency.setdefault(source_id, set()).add(target_id)
        reachable: set[str] = set()
        queue = deque(starts)
        while queue:
            node_id = queue.popleft()
            if node_id in reachable:
                continue
            reachable.add(node_id)
            queue.extend(sorted(adjacency.get(node_id, set()) - reachable))
        return [
            node_by_id[node_id].get("workspace")
            for node_id in sorted(reachable)
            if isinstance(node_by_id.get(node_id, {}).get("workspace"), dict)
        ]

    @classmethod
    def _reachable_condition_workspaces(cls, workspace: dict[str, Any]) -> list[dict[str, Any]]:
        nodes = [item for item in (workspace.get("nodes") or []) if isinstance(item, dict)]
        edges = [item for item in (workspace.get("edges") or []) if isinstance(item, dict)]
        node_ids = {str(node.get("id") or "").strip() for node in nodes}
        starts = {
            str(node.get("id") or "").strip()
            for node in nodes
            if str(node.get("nodeType") or "").strip() == "start"
        }
        outgoing: dict[str, list[dict[str, Any]]] = {}
        for edge in edges:
            source = edge.get("source") if isinstance(edge.get("source"), dict) else {}
            source_id = str(source.get("nodeId") or "").strip()
            if source_id:
                outgoing.setdefault(source_id, []).append(edge)
        reachable = set(starts)
        queue = deque(starts)
        condition_workspaces: list[dict[str, Any]] = []
        while queue:
            source_id = queue.popleft()
            for edge in outgoing.get(source_id, []):
                condition = edge.get("conditionWorkspace")
                if isinstance(condition, dict):
                    condition_workspaces.append(condition)
                target = edge.get("target") if isinstance(edge.get("target"), dict) else {}
                target_id = str(target.get("nodeId") or "").strip()
                if target_id in node_ids and target_id not in reachable:
                    reachable.add(target_id)
                    queue.append(target_id)
        return condition_workspaces

    @classmethod
    def _reachable_block_types(cls, workspace: dict[str, Any]) -> Counter:
        block_types: Counter = Counter()
        graph_workspaces = (
            cls._reachable_node_workspaces(workspace)
            + cls._reachable_condition_workspaces(workspace)
        )
        for graph_workspace in graph_workspaces:
            for block in cls._workspace_blocks(graph_workspace):
                block_type = str(block.get("type") or "").strip()
                if block_type:
                    block_types[block_type] += 1
        return block_types

    @classmethod
    def _referenced_actor_names(cls, workspace: dict[str, Any]) -> set[str]:
        names: set[str] = set()
        for graph_workspace in cls._reachable_node_workspaces(workspace):
            for block in cls._workspace_blocks(graph_workspace):
                block_type = str(block.get("type") or "").strip()
                for input_name in NodeGraphReviewService.ACTOR_REFERENCE_FIELDS.get(block_type, ()):
                    state, actor_name = NodeGraphReviewService._actor_reference(block, input_name)
                    if state == "resolved" and actor_name:
                        names.add(actor_name)
        return names

    @staticmethod
    def _literal_text_input(block: dict[str, Any], input_name: str) -> str:
        inputs = block.get("inputs") if isinstance(block.get("inputs"), dict) else {}
        connection = inputs.get(input_name) if isinstance(inputs, dict) else None
        if not isinstance(connection, dict):
            return ""
        for key in ("block", "shadow"):
            value_block = connection.get(key)
            if not isinstance(value_block, dict):
                continue
            fields = value_block.get("fields") if isinstance(value_block.get("fields"), dict) else {}
            for field_name in ("TEXT", "VALUE", "TAG", "TAG_TEXT"):
                value = fields.get(field_name)
                if isinstance(value, str) and value.strip():
                    return value.strip()
        return ""

    @classmethod
    def _referenced_tags(cls, workspace: dict[str, Any]) -> set[str]:
        batch_tag_blocks = {
            "object_move_tag",
            "object_set_tag_velocity_axis",
            "object_count_tag",
            "object_count_active_tag",
            "object_reset_tag",
            "object_scatter_tag",
            "object_recycle_tag_axis",
        }
        tags: set[str] = set()
        for graph_workspace in cls._reachable_node_workspaces(workspace):
            for block in cls._workspace_blocks(graph_workspace):
                if str(block.get("type") or "").strip() not in batch_tag_blocks:
                    continue
                fields = block.get("fields") if isinstance(block.get("fields"), dict) else {}
                for field_name in ("TAG_TEXT", "TAG"):
                    value = fields.get(field_name)
                    if isinstance(value, str) and value.strip():
                        tags.add(value.strip())
                input_value = cls._literal_text_input(block, "TAG")
                if input_value:
                    tags.add(input_value)
        return tags

    @staticmethod
    def _actor_tags(project_context: dict[str, Any]) -> dict[str, set[str]]:
        actors = project_context.get("actors") if isinstance(project_context, dict) else []
        if not isinstance(actors, list):
            return {}
        result: dict[str, set[str]] = {}
        for actor in actors:
            if not isinstance(actor, dict):
                continue
            name = str(actor.get("name") or "").strip()
            if not name:
                continue
            raw_tags = actor.get("tags") if isinstance(actor.get("tags"), list) else []
            result[name] = {str(tag).strip() for tag in raw_tags if str(tag).strip()}
        return result

    @staticmethod
    def _actor_names(project_context: dict[str, Any]) -> set[str]:
        actors = project_context.get("actors") if isinstance(project_context, dict) else []
        if not isinstance(actors, list):
            return set()
        return {
            str(actor.get("name") or "").strip()
            for actor in actors
            if isinstance(actor, dict) and str(actor.get("name") or "").strip()
        }

    @staticmethod
    def _input_child_blocks(block: dict[str, Any], input_name: str) -> list[dict[str, Any]]:
        inputs = block.get("inputs") if isinstance(block.get("inputs"), dict) else {}
        connection = inputs.get(input_name) if isinstance(inputs, dict) else None
        if not isinstance(connection, dict):
            return []
        return [
            child
            for key in ("block", "shadow")
            if isinstance((child := connection.get(key)), dict)
        ]

    @classmethod
    def _walk_input_subtree(cls, block: dict[str, Any]):
        if not isinstance(block, dict):
            return
        yield block
        inputs = block.get("inputs") if isinstance(block.get("inputs"), dict) else {}
        for connection in inputs.values():
            if not isinstance(connection, dict):
                continue
            for key in ("block", "shadow"):
                child = connection.get(key)
                if isinstance(child, dict):
                    yield from cls._walk_input_subtree(child)

    @classmethod
    def _coordinate_reads(cls, blocks: list[dict[str, Any]]) -> dict[str, set[str]]:
        reads: dict[str, set[str]] = {}
        for root in blocks:
            for block in cls._walk_input_subtree(root):
                getter = cls.COORDINATE_GETTER_TYPES.get(str(block.get("type") or "").strip())
                if not getter:
                    continue
                field_name, axis = getter
                state, actor_name = NodeGraphReviewService._actor_reference(block, field_name)
                if state == "resolved" and actor_name:
                    reads.setdefault(actor_name, set()).add(axis)
        return reads

    @classmethod
    def _subtree_actor_references(cls, root: dict[str, Any]) -> set[str]:
        names: set[str] = set()
        for block in cls._walk_input_subtree(root):
            block_type = str(block.get("type") or "").strip()
            for field_name in NodeGraphReviewService.ACTOR_REFERENCE_FIELDS.get(block_type, ()):
                state, actor_name = NodeGraphReviewService._actor_reference(block, field_name)
                if state == "resolved" and actor_name:
                    names.add(actor_name)
        return names

    @classmethod
    def _movement_targets(
        cls, block: dict[str, Any], actor_tags: dict[str, set[str]]
    ) -> set[str]:
        block_type = str(block.get("type") or "").strip()
        targets: set[str] = set()
        for field_name in NodeGraphReviewService.ACTOR_REFERENCE_FIELDS.get(block_type, ()):
            state, actor_name = NodeGraphReviewService._actor_reference(block, field_name)
            if state == "resolved" and actor_name:
                targets.add(actor_name)
        if block_type in {"object_move_tag", "object_set_tag_velocity_axis"}:
            fields = block.get("fields") if isinstance(block.get("fields"), dict) else {}
            tag = str(fields.get("TAG_TEXT") or fields.get("TAG") or cls._literal_text_input(block, "TAG")).strip()
            if tag:
                targets.update(name for name, tags in actor_tags.items() if tag in tags)
        return targets

    @classmethod
    def _dynamic_relation_coverage(
        cls,
        workspace: dict[str, Any],
        source: str,
        targets: list[str],
        actor_tags: dict[str, set[str]],
    ) -> dict[str, set[str]]:
        coverage = {target: set() for target in targets}
        for graph_workspace in cls._reachable_node_workspaces(workspace):
            for block in cls._workspace_blocks(graph_workspace):
                block_type = str(block.get("type") or "").strip()
                input_axes = cls.DYNAMIC_MOVEMENT_INPUTS.get(block_type)
                if not input_axes:
                    continue
                matching_targets = cls._movement_targets(block, actor_tags).intersection(coverage)
                if not matching_targets:
                    continue
                fields = block.get("fields") if isinstance(block.get("fields"), dict) else {}
                for input_name, configured_axis in input_axes.items():
                    axis = str(fields.get("AXIS") or "").upper() if configured_axis == "FIELD_AXIS" else configured_axis
                    if axis not in {"X", "Y", "Z"}:
                        continue
                    reads = cls._coordinate_reads(cls._input_child_blocks(block, input_name))
                    if axis not in reads.get(source, set()):
                        continue
                    for target in matching_targets:
                        coverage[target].add(axis)
        return coverage

    @classmethod
    def _condition_relation_satisfied(
        cls,
        workspace: dict[str, Any],
        source: str,
        target: str,
        axes: set[str],
    ) -> bool:
        condition_roots: list[dict[str, Any]] = []
        for graph_workspace in cls._reachable_node_workspaces(workspace):
            for block in cls._workspace_blocks(graph_workspace):
                if str(block.get("type") or "") not in {"control_if", "control_else"}:
                    continue
                condition_roots.extend(cls._input_child_blocks(block, "CONDITION"))
        for graph_workspace in cls._reachable_condition_workspaces(workspace):
            condition_roots.extend(cls._workspace_roots(graph_workspace))

        for root in condition_roots:
            references = cls._subtree_actor_references(root)
            if source not in references or target not in references:
                continue
            reads = cls._coordinate_reads([root])
            source_axes = reads.get(source, set())
            target_axes = reads.get(target, set())
            contains_position_near = any(
                str(block.get("type") or "") == "detect_position_near"
                for block in cls._walk_input_subtree(root)
            )
            if axes.issubset(source_axes) and (contains_position_near or axes.issubset(target_axes)):
                return True
        return False

    @staticmethod
    def _contains_chinese(value: Any) -> bool:
        return bool(re.search(r"[\u3400-\u9fff]", str(value or "")))

    @classmethod
    def _validate_actor_references(
        cls, result: dict[str, Any], request: dict[str, Any]
    ) -> None:
        workspace = result.get("workspace") if isinstance(result.get("workspace"), dict) else {}
        project_context = request.get("projectContext")
        actors = project_context.get("actors") if isinstance(project_context, dict) else None
        if isinstance(project_context, dict) and "actorContextAvailable" in project_context:
            actor_context_available = project_context.get("actorContextAvailable") is True
        else:
            actor_context_available = isinstance(actors, list)
        known_actors = cls._actor_names(project_context if isinstance(project_context, dict) else {})
        errors: list[str] = []

        for graph_workspace in cls._all_graph_workspaces(workspace):
            for block in cls._workspace_blocks(graph_workspace):
                block_type = str(block.get("type") or "").strip()
                block_id = str(block.get("id") or "").strip() or "<missing-id>"
                for input_name in NodeGraphReviewService.ACTOR_REFERENCE_FIELDS.get(block_type, ()):
                    state, actor_name = NodeGraphReviewService._actor_reference(block, input_name)
                    if state == "missing":
                        errors.append(
                            f"积木 {block_id} ({block_type}) 没有指定对象输入 {input_name}"
                        )
                    elif state == "resolved" and actor_context_available and actor_name not in known_actors:
                        errors.append(
                            f"积木 {block_id} ({block_type}) 引用的对象 {actor_name!r} 不存在于当前场景"
                        )
                    if len(errors) >= 6:
                        break
                if len(errors) >= 6:
                    break
            if len(errors) >= 6:
                break

        if errors:
            raise ValueError("对象引用校验失败：" + "；".join(errors))

    @classmethod
    def _validate_response_language(
        cls, result: dict[str, Any], request: dict[str, Any]
    ) -> None:
        language = request["responseLanguage"]
        summary = str(result.get("summary") or "").strip()
        if language == "zh-CN" and not cls._contains_chinese(summary):
            raise ValueError("中文请求的 summary 必须使用中文")
        if language == "en-US" and cls._contains_chinese(summary):
            raise ValueError("English request summary must use English")

        old_workspace = request.get("workspace") if isinstance(request.get("workspace"), dict) else {}
        new_workspace = result.get("workspace") if isinstance(result.get("workspace"), dict) else {}
        old_nodes = {
            str(node.get("id") or ""): node
            for node in old_workspace.get("nodes", [])
            if isinstance(node, dict) and str(node.get("id") or "")
        }
        for node in new_workspace.get("nodes", []):
            if not isinstance(node, dict) or node.get("nodeType") != "custom":
                continue
            node_id = str(node.get("id") or "")
            label = str(node.get("customName") or node.get("name") or "").strip()
            old = old_nodes.get(node_id)
            old_label = str(old.get("customName") or old.get("name") or "").strip() if old else None
            if old is not None and old_label == label:
                continue
            if language == "zh-CN" and not cls._contains_chinese(label):
                raise ValueError(f"新增或改名的自定义节点必须使用中文：{label or node_id}")
            if language == "en-US" and cls._contains_chinese(label):
                raise ValueError(f"New or renamed custom node must use English: {label or node_id}")

        old_edges = {
            str(edge.get("id") or ""): edge
            for edge in old_workspace.get("edges", [])
            if isinstance(edge, dict) and str(edge.get("id") or "")
        }
        for edge in new_workspace.get("edges", []):
            if not isinstance(edge, dict):
                continue
            edge_id = str(edge.get("id") or "")
            label = str(edge.get("name") or "").strip()
            old = old_edges.get(edge_id)
            old_label = str(old.get("name") or "").strip() if old else None
            if not label or (old is not None and old_label == label):
                continue
            if language == "zh-CN" and not cls._contains_chinese(label):
                raise ValueError(f"新增或改名的连线名称必须使用中文：{label}")
            if language == "en-US" and cls._contains_chinese(label):
                raise ValueError(f"New or renamed edge name must use English: {label}")

    @classmethod
    def _validate_requested_semantics(
        cls, result: dict[str, Any], request: dict[str, Any]
    ) -> None:
        requirements = cls._instruction_requirements(
            request["instruction"], request.get("operation"), request.get("projectContext")
        )
        required = set(requirements["requiredCapabilities"])
        if not required:
            return

        workspace = result["workspace"]
        actor_names = cls._actor_names(request.get("projectContext") or {})
        control_required = required & {"wasd-object-movement", "space-object-jump"}
        if control_required:
            matching_chain = None
            movement_types = {"object_third_person_move"}
            if requirements["firstPersonRequested"]:
                movement_types.add("object_first_person_move")
            for graph_workspace in cls._reachable_node_workspaces(workspace):
                for root in cls._workspace_roots(graph_workspace):
                    if root.get("type") != "node_while_active":
                        continue
                    do_input = root.get("inputs", {}).get("DO", {}) if isinstance(root.get("inputs"), dict) else {}
                    first = do_input.get("block") if isinstance(do_input, dict) else None
                    chain = list(cls._walk_block(first)) if isinstance(first, dict) else []
                    movements = [block for block in chain if block.get("type") in movement_types]
                    jumps = [block for block in chain if block.get("type") == "object_arcade_jump"]
                    if (
                        ("wasd-object-movement" not in control_required or movements)
                        and ("space-object-jump" not in control_required or jumps)
                    ):
                        matching_chain = (movements, jumps)
                        break
                if matching_chain:
                    break

            if not matching_chain:
                missing = []
                if "wasd-object-movement" in control_required:
                    missing.append("node_while_active 中的 object_third_person_move")
                if "space-object-jump" in control_required:
                    missing.append("同一持续循环中的 object_arcade_jump")
                raise ValueError("请求的控制能力没有完整实现：" + "；".join(missing))

            movements, jumps = matching_chain
            targets = []
            for block in movements + jumps:
                fields = block.get("fields") if isinstance(block.get("fields"), dict) else {}
                targets.append(str(fields.get("NAME") or "").strip())
            if not targets or any(not target for target in targets):
                raise ValueError("WASD 移动和空格跳跃必须绑定明确的同一对象")
            if len(set(targets)) != 1:
                raise ValueError("WASD 移动和空格跳跃必须绑定同一个对象")
            if actor_names and targets[0] not in actor_names:
                raise ValueError(f"对象 {targets[0]} 不存在于当前场景")

        reachable_types = set(cls._reachable_block_types(workspace))
        alternative_groups = [
            [str(capability) for capability in group if str(capability)]
            for group in (requirements.get("capabilityAlternatives") or [])
            if isinstance(group, list)
        ]
        alternative_members = {capability for group in alternative_groups for capability in group}
        missing_capabilities = []
        for capability in sorted(required - control_required - alternative_members):
            accepted = cls.CAPABILITY_VALIDATION_TYPES.get(capability)
            if accepted and not reachable_types.intersection(accepted):
                missing_capabilities.append(capability)
        for group in alternative_groups:
            if not any(
                reachable_types.intersection(cls.CAPABILITY_VALIDATION_TYPES.get(capability, set()))
                for capability in group
            ):
                missing_capabilities.append("(" + " or ".join(group) + ")")
        if missing_capabilities:
            raise ValueError(
                "生成节点图缺少请求的关键能力：" + ", ".join(missing_capabilities)
            )

        if "attraction" in required and "node_while_active" not in reachable_types:
            raise ValueError("持续吸附逻辑必须从 node_while_active 生命周期入口可达")

        actor_requirement = requirements.get("actors") if isinstance(requirements.get("actors"), dict) else {}
        targets = [str(value) for value in (actor_requirement.get("targets") or []) if str(value)]
        if actor_requirement.get("multiTarget") is True and len(targets) >= 2:
            referenced = cls._referenced_actor_names(workspace)
            referenced_tags = cls._referenced_tags(workspace)
            actor_tags = cls._actor_tags(request.get("projectContext") or {})
            missing_actors = [
                name
                for name in targets
                if name not in referenced and not actor_tags.get(name, set()).intersection(referenced_tags)
            ]
            if missing_actors:
                raise ValueError(
                    "多目标请求没有覆盖以下明确目标：" + ", ".join(missing_actors[:6])
                )

        actor_tags = cls._actor_tags(request.get("projectContext") or {})
        for relation in requirements.get("dynamicActorRelations") or []:
            if not isinstance(relation, dict):
                continue
            source = str(relation.get("source") or "").strip()
            targets = [str(value).strip() for value in (relation.get("targets") or []) if str(value).strip()]
            axes = {str(value).upper() for value in (relation.get("axes") or []) if str(value).upper() in {"X", "Y", "Z"}}
            if not source or not targets or not axes:
                continue
            coverage = cls._dynamic_relation_coverage(workspace, source, targets, actor_tags)
            incomplete = [
                f"{target} missing {','.join(sorted(axes - coverage.get(target, set())))}"
                for target in targets
                if not axes.issubset(coverage.get(target, set()))
            ]
            if incomplete:
                raise ValueError(
                    "Dynamic actor dependency is incomplete: movement for each target must read "
                    f"the live {','.join(sorted(axes))} coordinates of {source}; "
                    + "; ".join(incomplete[:6])
                )

        for relation in requirements.get("distanceActorRelations") or []:
            if not isinstance(relation, dict):
                continue
            source = str(relation.get("source") or "").strip()
            targets = [str(value).strip() for value in (relation.get("targets") or []) if str(value).strip()]
            axes = {str(value).upper() for value in (relation.get("axes") or []) if str(value).upper() in {"X", "Y", "Z"}}
            if not source or not targets or not axes:
                continue
            missing = [
                target
                for target in targets
                if not cls._condition_relation_satisfied(workspace, source, target, axes)
            ]
            if missing:
                raise ValueError(
                    "Distance condition must contain both requested actors and use live coordinates: "
                    f"source={source}, missingTargets={','.join(missing[:6])}"
                )

        camera_targets = [str(value).strip() for value in (requirements.get("cameraTargets") or []) if str(value).strip()]
        if camera_targets:
            expected = camera_targets[0]
            matched = False
            for graph_workspace in cls._reachable_node_workspaces(workspace):
                for block in cls._workspace_blocks(graph_workspace):
                    block_type = str(block.get("type") or "").strip()
                    if block_type not in cls.CAMERA_FOLLOW_TYPES:
                        continue
                    for field_name in NodeGraphReviewService.ACTOR_REFERENCE_FIELDS.get(block_type, ()):
                        state, actor_name = NodeGraphReviewService._actor_reference(block, field_name)
                        if state == "resolved" and actor_name == expected:
                            matched = True
                            break
                    if matched:
                        break
                if matched:
                    break
            if not matched:
                raise ValueError(f"Camera follow block must target the requested actor {expected}")

        if requirements["narrowObjectControl"]:
            before = cls._graph_block_types(request["workspace"])
            after = cls._graph_block_types(workspace)
            prohibited = {"ui_set_score", "ui_add_score", "ui_set_lives", "ui_game_win", "ui_game_over"}
            newly_added = after - before
            bad_types = sorted(
                block_type
                for block_type, count in newly_added.items()
                if count > 0 and (block_type in prohibited or block_type.startswith("combat_"))
            )
            if bad_types:
                raise ValueError("局部对象控制请求不应新增计分、生命、胜负或战斗模板：" + ", ".join(bad_types))
            old_count = len(request["workspace"].get("nodes") or [])
            new_count = len(workspace.get("nodes") or [])
            if new_count - old_count > 2:
                raise ValueError("局部对象控制请求新增了过多节点，请只修改最小必要逻辑")

    @classmethod
    def _validate_operation_scope(
        cls, result: dict[str, Any], request: dict[str, Any]
    ) -> None:
        operation = request.get("operation")
        if operation not in {"extend", "edit"}:
            return
        before = request.get("workspace") if isinstance(request.get("workspace"), dict) else {}
        after = result.get("workspace") if isinstance(result.get("workspace"), dict) else {}

        def ids(workspace: dict[str, Any], key: str) -> set[str]:
            return {
                str(item.get("id") or "").strip()
                for item in (workspace.get(key) or [])
                if isinstance(item, dict) and str(item.get("id") or "").strip()
            }

        missing_nodes = sorted(ids(before, "nodes") - ids(after, "nodes"))
        missing_edges = sorted(ids(before, "edges") - ids(after, "edges"))
        if missing_nodes or missing_edges:
            details = []
            if missing_nodes:
                details.append("nodes=" + ",".join(missing_nodes[:6]))
            if missing_edges:
                details.append("edges=" + ",".join(missing_edges[:6]))
            raise ValueError(
                "Incremental edit removed existing structures without an explicit delete request: "
                + "; ".join(details)
            )
        if operation == "edit" and before == after:
            raise ValueError("The edit request did not change any node logic")

    @classmethod
    def _validate_result(
        cls, result: dict[str, Any], request: dict[str, Any], contract_path: Path
    ) -> dict[str, Any]:
        if not isinstance(result, dict):
            raise ValueError("DeepSeek 必须返回一个 JSON 对象")
        for key in ("requestId", "targetId", "projectScopeId", "baseGraphRevision", "operation"):
            if str(result.get(key) or "") != str(request.get(key) or ""):
                raise ValueError(f"DeepSeek 返回的 {key} 与当前请求不一致")
        if result.get("schemaVersion") != 1 or isinstance(result.get("schemaVersion"), bool):
            raise ValueError("DeepSeek 返回的 schemaVersion 必须为 1")
        summary = str(result.get("summary") or "").strip()
        if not summary:
            raise ValueError("DeepSeek 返回结果缺少 summary")
        forbidden = cls._contains_forbidden_key(result)
        if forbidden:
            raise ValueError(f"DeepSeek 返回了禁止字段：{forbidden}")

        normalized_result, normalization_warnings = cls._normalize_model_block_serialization(
            result, contract_path
        )
        validated = validate_generated_node_graph(normalized_result, catalog_path=contract_path)
        if validated.get("success") is not True:
            errors = validated.get("errors") or []
            detail = "；".join(str(item) for item in errors[:6])
            raise ValueError("\u751f\u6210\u7684\u8282\u70b9\u56fe\u672a\u901a\u8fc7\u79ef\u6728\u5408\u540c\u6821\u9a8c" + (f"\uff1a{detail}" if detail else ""))
        cls._validate_response_language(normalized_result, request)
        cls._validate_actor_references(normalized_result, request)
        cls._validate_operation_scope(normalized_result, request)
        cls._validate_requested_semantics(normalized_result, request)
        return {
            "schemaVersion": 1,
            "requestId": request["requestId"],
            "targetId": cls.TARGET_ID,
            "projectScopeId": request["projectScopeId"],
            "baseGraphRevision": request["baseGraphRevision"],
            "operation": request["operation"],
            "summary": summary[:600],
            "workspace": normalized_result["workspace"],
            "warnings": normalization_warnings + list(validated.get("warnings") or []),
        }

    @staticmethod
    def _redact_secret(value: Any, secret: str | None) -> str:
        text = str(value or "")
        secret_text = str(secret or "")
        return text.replace(secret_text, "[redacted]") if secret_text else text

    @staticmethod
    def _needs_contract_expansion(error: Exception | str | None) -> bool:
        text = str(error or "").casefold()
        markers = (
            "unknown block type",
            "缺少请求的关键能力",
            "控制能力没有完整实现",
        )
        return any(marker.casefold() in text for marker in markers)

    def generate(self, payload: Any, cancel_event: threading.Event | None = None) -> dict[str, Any]:
        try:
            request = self._normalize_payload(payload)
            if cancel_event and cancel_event.is_set():
                return self._error("GENERATION_CANCELLED", "已停止本次节点生成。")
            settings = NodeGraphReviewService._resolve_settings("node_graph")
            if not settings.api_key:
                return self._error("AI_NOT_CONFIGURED", "DeepSeek 未配置，无法生成节点逻辑。")
            contract_path, contract_text = self._load_contract()
            requirements = self._instruction_requirements(
                request["instruction"], request["operation"], request.get("projectContext")
            )
            primary_selection = self._select_contract(
                request, contract_path, contract_text, requirements
            )
            normalized = None
            validation_error = None
            for attempt in range(2):
                selection = primary_selection
                if (
                    attempt == 1
                    and primary_selection["mode"] != "full"
                    and self._needs_contract_expansion(validation_error)
                ):
                    selection = self._select_contract(
                        request, contract_path, contract_text, requirements, expanded=True
                    )
                    error_text = str(validation_error or "").casefold()
                    if (
                        selection["selectedTypes"] == primary_selection["selectedTypes"]
                        or "unknown block type" in error_text
                    ):
                        selection = {
                            "text": contract_text,
                            "mode": "full",
                            "selectedTypes": sorted(
                                (load_contract_catalog(contract_path).get("blocks") or {}).keys()
                            ),
                        }
                current_prompt = self._build_prompt(
                    request,
                    selection["text"],
                    requirements,
                    contract_mode=selection["mode"],
                    selected_types=selection["selectedTypes"],
                )
                if validation_error is not None:
                    current_prompt += (
                        "\n\nPREVIOUS_RESULT_REJECTED:\n"
                        + str(validation_error)
                        + "\nReturn a corrected complete JSON result. Do not repeat the rejected structure."
                    )
                logger.info(
                    "Generating node graph [attempt=%d, contract=%s, blocks=%d]",
                    attempt + 1,
                    selection["mode"],
                    len(selection["selectedTypes"]),
                )
                raw = self._call_deepseek(settings, current_prompt)
                if cancel_event and cancel_event.is_set():
                    return self._error("GENERATION_CANCELLED", "已停止本次节点生成。")
                try:
                    result = NodeGraphReviewService._parse_model_result(raw)
                    normalized = self._validate_result(result, request, contract_path)
                    break
                except ValueError as exc:
                    validation_error = exc
                    if attempt == 0:
                        logger.info("Retrying rejected node graph generation: %s", exc)
                        continue
                    raise
            if normalized is None:
                raise validation_error or ValueError("DeepSeek 没有返回可应用的节点图")
            logger.info(
                "Node graph generation completed [source=%s, model=%s, operation=%s, revision=%s, nodes=%d, edges=%d]",
                settings.source,
                settings.model,
                request["operation"],
                request["baseGraphRevision"][:12],
                len(normalized["workspace"].get("nodes") or []),
                len(normalized["workspace"].get("edges") or []),
            )
            return {"success": True, "status": "ok", **normalized}
        except ValueError as exc:
            safe_message = self._redact_secret(
                exc, getattr(locals().get("settings"), "api_key", "")
            )
            logger.warning("Node graph generation rejected: %s", safe_message)
            return self._error("INVALID_GENERATION_DATA", safe_message)
        except urllib.error.HTTPError as exc:
            status = int(getattr(exc, "code", 0) or 0)
            logger.warning("Node graph generation provider HTTP error [status=%s]", status)
            if status in (401, 403):
                return self._error("AI_AUTH_FAILED", "DeepSeek 身份验证失败，请检查现有 AI 配置。")
            if status == 429:
                return self._error("AI_RATE_LIMITED", "DeepSeek 请求过于频繁，请稍后再试。")
            return self._error("AI_PROVIDER_ERROR", f"DeepSeek 服务暂时不可用（HTTP {status}）。")
        except (urllib.error.URLError, TimeoutError, socket.timeout):
            logger.warning("Node graph generation network error")
            return self._error("AI_NETWORK_ERROR", "暂时无法连接 DeepSeek，当前节点图没有被修改。")
        except Exception as exc:
            logger.exception("Node graph generation failed: %s", type(exc).__name__)
            return self._error("AI_GENERATION_FAILED", "节点逻辑生成失败，当前节点图没有被修改。")

    def start(self, payload: Any) -> dict[str, Any]:
        try:
            request = self._normalize_payload(payload)
        except (ValueError, json.JSONDecodeError) as exc:
            return self._error("INVALID_GENERATION_REQUEST", str(exc))
        with self._lock:
            if self._closed:
                return self._error("GENERATION_SERVICE_CLOSED", "节点生成服务已经关闭。")
            self._prune_locked()
            task_id = f"node_generate_task_{uuid.uuid4().hex}"
            cancel_event = threading.Event()
            self._tasks[task_id] = {
                "taskId": task_id,
                "requestId": request["requestId"],
                "status": "pending",
                "createdAt": time.time(),
                "updatedAt": time.time(),
                "cancel": cancel_event,
            }
            future = self._executor.submit(self.generate, request, cancel_event)
            future.add_done_callback(lambda completed, current=task_id: self._complete(current, completed))
        return {"success": True, "status": "pending", "taskId": task_id, "requestId": request["requestId"]}

    def _complete(self, task_id: str, future: Any) -> None:
        try:
            result = future.result()
        except Exception:
            logger.exception("Background node graph generation failed")
            result = self._error("AI_GENERATION_FAILED", "节点逻辑生成失败，当前节点图没有被修改。")
        with self._lock:
            state = self._tasks.get(task_id)
            if not state:
                return
            if state["cancel"].is_set():
                state["status"] = "cancelled"
                state["message"] = "已停止本次节点生成。"
            else:
                state["status"] = "completed"
                state["result"] = result
            state["updatedAt"] = time.time()

    def status(self, task_id: str) -> dict[str, Any]:
        key = str(task_id or "").strip()
        if not key:
            return self._error("INVALID_TASK_ID", "缺少节点生成任务 ID。")
        with self._lock:
            state = self._tasks.get(key)
            if not state:
                return self._error("GENERATION_TASK_NOT_FOUND", "节点生成任务不存在或已经过期。")
            response = {
                "success": True,
                "status": state["status"],
                "taskId": state["taskId"],
                "requestId": state["requestId"],
            }
            if state["status"] == "completed":
                response["result"] = json.loads(json.dumps(state.get("result") or {}, ensure_ascii=False))
            if state["status"] == "cancelled":
                response["message"] = str(state.get("message") or "已停止本次节点生成。")
            return response

    def cancel(self, task_id: str) -> dict[str, Any]:
        key = str(task_id or "").strip()
        if not key:
            return self._error("INVALID_TASK_ID", "缺少节点生成任务 ID。")
        with self._lock:
            state = self._tasks.get(key)
            if not state:
                return self._error("GENERATION_TASK_NOT_FOUND", "节点生成任务不存在或已经过期。")
            state["cancel"].set()
            if state["status"] != "completed":
                state["status"] = "cancelled"
                state["message"] = "已停止本次节点生成。"
                state["updatedAt"] = time.time()
        return {"success": True, "status": "cancelled", "taskId": key}

    def _prune_locked(self) -> None:
        if len(self._tasks) < self.MAX_TASKS:
            return
        finished = sorted(
            (
                (task_id, state)
                for task_id, state in self._tasks.items()
                if state.get("status") in {"completed", "cancelled"}
            ),
            key=lambda item: float(item[1].get("updatedAt") or item[1].get("createdAt") or 0),
        )
        while len(self._tasks) >= self.MAX_TASKS and finished:
            task_id, _ = finished.pop(0)
            self._tasks.pop(task_id, None)

    def shutdown(self) -> None:
        with self._lock:
            if self._closed:
                return
            self._closed = True
            for state in self._tasks.values():
                state["cancel"].set()
        self._executor.shutdown(wait=False, cancel_futures=True)


_service: NodeGraphGenerationService | None = None
_service_lock = threading.Lock()


def get_node_graph_generation_service() -> NodeGraphGenerationService:
    global _service
    with _service_lock:
        if _service is None:
            _service = NodeGraphGenerationService()
        return _service

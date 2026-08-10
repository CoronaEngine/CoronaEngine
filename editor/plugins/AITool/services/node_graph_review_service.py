from __future__ import annotations

import hashlib
import importlib
import json
import logging
import socket
import threading
import time
import unicodedata
import uuid
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Any, Iterable

logger = logging.getLogger(__name__)


@dataclass(frozen=True)
class DeepSeekSettings:
    api_key: str
    base_url: str
    model: str
    source: str
    temperature: float = 0.1
    max_tokens: int = 1200
    thinking_enabled: bool = False


class NodeGraphReviewService:
    """Review a visible project node graph without executing its generated script."""

    DEFAULT_BASE_URL = "https://api.deepseek.com"
    DEFAULT_MODEL = "deepseek-v4-flash"
    TIMEOUT_SECONDS = 35
    CONTRACT_FILENAME = "CoronaBlocksDocument.internal-ai-contract.xml"
    MAX_TASKS = 32
    MAX_CACHE_ENTRIES = 32
    ISSUE_PATTERN_FIELDS = (
        "blockType", "workspaceRole", "relationType",
        "missingInput", "objectRequirement", "edgeId",
    )
    ISSUE_CODE_ALIASES = {"dangling_edge": "invalid_edge_endpoint"}
    FACT_REQUIRED_ISSUE_CODES = {
        "missing_actor_target",
        "actor_target_not_found",
        "missing_required_input",
    }
    ACTOR_NAME_FIELDS = (
        "name", "actor_name", "actorName", "alias", "displayName",
        "display_name", "nativeName", "native_name",
    )
    ACTOR_ALIAS_FIELDS = ("aliases", "displayNames", "display_names", "names")
    OPTIONAL_FIELD_ALIASES = {
        "OBSTACLE_TAG": ("obstacle_tag", "obstacletag", "obstacle tag", "障碍标签"),
    }

    # Only these fields identify an existing scene actor. TAG, variable names,
    # cooldown names and spawn names are intentionally excluded.
    ACTOR_REFERENCE_FIELDS: dict[str, tuple[str, ...]] = {
        "engine_move": ("OBJECT",),
        "engine_rotateX": ("OBJECT",),
        "engine_rotateY": ("OBJECT",),
        "engine_rotateZ": ("OBJECT",),
        "engine_face": ("OBJECT",),
        "engine_moveto": ("OBJECT",),
        "engine_movetoXYZ": ("OBJECT",),
        "engine_movetoXYZtime": ("OBJECT",),
        "engine_Xset": ("OBJECT",),
        "engine_Yset": ("OBJECT",),
        "engine_Zset": ("OBJECT",),
        "engine_Xadd": ("OBJECT",),
        "engine_Yadd": ("OBJECT",),
        "engine_Zadd": ("OBJECT",),
        "engine_X": ("OBJECT",),
        "engine_Y": ("OBJECT",),
        "engine_Z": ("OBJECT",),
        "engine_rotationX": ("OBJECT",),
        "engine_rotationY": ("OBJECT",),
        "engine_rotationZ": ("OBJECT",),
        "object_set_position": ("NAME",),
        "object_move_direction": ("NAME",),
        "object_get_x": ("NAME",),
        "object_get_y": ("NAME",),
        "object_get_z": ("NAME",),
        "object_hide": ("NAME",),
        "object_show": ("NAME",),
        "object_delete": ("NAME",),
        "object_exists": ("NAME",),
        "object_set_tag": ("NAME",),
        "object_clamp_axis": ("NAME",),
        "object_set_native_physics": ("NAME",),
        "object_set_logical_collision": ("NAME",),
        "object_logical_collision_enabled": ("NAME",),
        "object_save_checkpoint": ("NAME",),
        "object_restore_checkpoint": ("NAME",),
        "object_reset_crossed_once": ("NAME",),
        "object_move_to_lane": ("NAME",),
        "object_move_to_lane_smooth": ("NAME",),
        "object_lane_index": ("NAME",),
        "object_set_random_position": ("NAME",),
        "object_third_person_move": ("NAME",),
        "object_arcade_jump": ("NAME",),
        "object_first_person_move": ("NAME",),
        "camera_follow_object": ("NAME",),
        "camera_third_person_orbit": ("NAME",),
        "camera_first_person_follow": ("NAME",),
        "combat_melee_attack": ("PLAYER",),
        "combat_enemy_chase_tag": ("PLAYER",),
        "combat_enemy_contact_damage": ("PLAYER",),
        "object_breakout_reset_round": ("BALL", "PADDLE"),
        "object_breakout_paddle_control": ("PADDLE",),
        "object_breakout_step": ("BALL", "PADDLE"),
        "detect_object_exists": ("NAME",),
        "detect_object_not_exists": ("NAME",),
        "detect_inside_axis": ("NAME",),
        "detect_outside_axis": ("NAME",),
        "detect_inside_box": ("NAME",),
        "detect_position_near": ("NAME",),
        "detect_passed_x": ("NAME",),
        "detect_passed_z": ("NAME",),
        "detect_crossed_x_once": ("NAME",),
        "detect_crossed_z_once": ("NAME",),
    }
    ACTOR_PLACEHOLDERS = {
        "", "__manual__", "none", "null", "undefined", "actor", "object",
        "target", "current actor", "current object", "请选择", "请选择对象",
        "选择对象", "未选择", "对象", "物体", "对象名称", "物体名称",
        "当前对象", "当前物体",
    }

    def __init__(self) -> None:
        self._executor = ThreadPoolExecutor(
            max_workers=1, thread_name_prefix="NodeGraphReview"
        )
        self._lock = threading.RLock()
        self._tasks: dict[str, dict[str, Any]] = {}
        self._result_cache: dict[str, dict[str, Any]] = {}
        self._closed = False

    def start(self, payload: Any) -> dict[str, Any]:
        """Queue a review and return immediately without waiting for DeepSeek."""
        try:
            request = self._normalize_payload(payload)
        except ValueError as exc:
            return self._error("INVALID_REVIEW_DATA", str(exc))

        revision = request["graphRevision"]
        cache_key = self._cache_key(request)
        task_id = "node_review_" + uuid.uuid4().hex
        now = time.time()
        with self._lock:
            if self._closed:
                return self._error("AI_REVIEW_STOPPED", "Node graph review service has stopped.")
            cached = self._result_cache.get(cache_key)
            if cached is not None:
                self._tasks[task_id] = {
                    "taskId": task_id,
                    "graphRevision": revision,
                    "cacheKey": cache_key,
                    "status": "completed",
                    "createdAt": now,
                    "completedAt": now,
                    "result": json.loads(json.dumps(cached, ensure_ascii=False)),
                }
                self._prune_tasks_locked()
                return {
                    "success": True,
                    "status": "completed",
                    "taskId": task_id,
                    "graphRevision": revision,
                }

            self._tasks[task_id] = {
                "taskId": task_id,
                "graphRevision": revision,
                "cacheKey": cache_key,
                "status": "pending",
                "createdAt": now,
            }
            self._prune_tasks_locked()
            future = self._executor.submit(self.review, request)
            future.add_done_callback(
                lambda completed, current_task_id=task_id, current_revision=revision, current_cache_key=cache_key: self._complete_task(
                    current_task_id, current_revision, current_cache_key, completed
                )
            )

        return {
            "success": True,
            "status": "pending",
            "taskId": task_id,
            "graphRevision": revision,
        }

    def status(self, task_id: str) -> dict[str, Any]:
        task_key = str(task_id or "").strip()
        if not task_key:
            return self._error("INVALID_TASK_ID", "Missing node graph review task ID.")
        with self._lock:
            task = self._tasks.get(task_key)
            if task is None:
                return self._error("REVIEW_TASK_NOT_FOUND", "Node graph review task was not found or has expired.")
            response = {
                "success": True,
                "status": task["status"],
                "taskId": task["taskId"],
                "graphRevision": task["graphRevision"],
            }
            if task["status"] == "completed":
                response["result"] = json.loads(
                    json.dumps(task.get("result") or {}, ensure_ascii=False)
                )
            return response

    def shutdown(self) -> None:
        with self._lock:
            if self._closed:
                return
            self._closed = True
        self._executor.shutdown(wait=False, cancel_futures=True)

    def _complete_task(self, task_id: str, revision: str, cache_key: str, future: Any) -> None:
        try:
            result = future.result()
        except Exception as exc:
            logger.exception("Background node graph review failed: %s", type(exc).__name__)
            result = self._error(
                "AI_REVIEW_FAILED", "Node graph review failed and will retry later."
            )
        completed_at = time.time()
        with self._lock:
            task = self._tasks.get(task_id)
            if task is None:
                return
            task["status"] = "completed"
            task["completedAt"] = completed_at
            task["result"] = result
            if result.get("success") is True and result.get("status") == "ok":
                self._result_cache[cache_key] = json.loads(
                    json.dumps(result, ensure_ascii=False)
                )
                while len(self._result_cache) > self.MAX_CACHE_ENTRIES:
                    self._result_cache.pop(next(iter(self._result_cache)))
            self._prune_tasks_locked()

    def _prune_tasks_locked(self) -> None:
        if len(self._tasks) <= self.MAX_TASKS:
            return
        completed = sorted(
            (task for task in self._tasks.values() if task.get("status") == "completed"),
            key=lambda item: float(item.get("completedAt") or item.get("createdAt") or 0),
        )
        for task in completed[: max(0, len(self._tasks) - self.MAX_TASKS)]:
            self._tasks.pop(str(task.get("taskId") or ""), None)

    @staticmethod
    def _cache_key(request: dict[str, Any]) -> str:
        context = request.get("projectContext") if isinstance(request.get("projectContext"), dict) else {}
        context_text = json.dumps(context, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
        digest = hashlib.sha256(context_text.encode("utf-8")).hexdigest()[:20]
        return f"{request.get('graphRevision', '')}:{digest}"

    @classmethod
    def _find_contract_path(cls, start_path: Path | str | None = None) -> Path:
        """Find the repository contract from both source and packaged editor layouts."""
        start = Path(start_path or __file__).resolve()
        search_roots = [start] if start.is_dir() else list(start.parents)
        for root in search_roots:
            candidate = root / "docs" / cls.CONTRACT_FILENAME
            if candidate.is_file():
                return candidate
        # Keep a deterministic path in diagnostics when the checkout is incomplete.
        fallback_root = search_roots[-1] if search_roots else start.parent
        return fallback_root / "docs" / cls.CONTRACT_FILENAME

    def review(self, payload: Any) -> dict[str, Any]:
        try:
            request = self._normalize_payload(payload)
            settings = self._resolve_settings()
            if not settings.api_key:
                return self._error(
                    "AI_NOT_CONFIGURED",
                    "DeepSeek 未配置，节点审查暂不可用。",
                )

            facts = self._collect_local_facts(
                request["workspace"], request.get("projectContext") or {}
            )
            catalog = self._catalog_summary(request["workspace"])
            prompt = self._build_prompt(request, facts, catalog)
            raw_result = self._call_deepseek(settings, prompt)
            result = self._parse_model_result(raw_result)
            self._validate_model_result(result, request)
            result.update(
                {
                    "schemaVersion": 1,
                    "graphRevision": request["graphRevision"],
                    "provider": "deepseek",
                    "model": settings.model,
                }
            )
            logger.info(
                "Node graph review completed [source=%s, model=%s, revision=%s, "
                "nodes=%d, edges=%d, facts=%d, has_problems=%s]",
                settings.source,
                settings.model,
                request["graphRevision"][:12],
                len(request["workspace"]["nodes"]),
                len(request["workspace"]["edges"]),
                len(facts),
                bool(result.get("hasProblems")),
            )
            return {"success": True, "status": "ok", **result}
        except ValueError as exc:
            logger.warning("Node graph review rejected: %s", exc)
            return self._error("INVALID_REVIEW_DATA", str(exc))
        except urllib.error.HTTPError as exc:
            status = int(getattr(exc, "code", 0) or 0)
            logger.warning("Node graph review provider HTTP error [status=%s]", status)
            if status in (401, 403):
                return self._error(
                    "AI_AUTH_FAILED",
                    "DeepSeek 身份验证失败，请检查 editor/.env 中的 API key 配置。",
                )
            if status == 429:
                return self._error(
                    "AI_RATE_LIMITED", "DeepSeek 请求过于频繁，请稍后再试。"
                )
            return self._error(
                "AI_PROVIDER_ERROR", f"DeepSeek 服务暂时不可用（HTTP {status}）。"
            )
        except (urllib.error.URLError, TimeoutError, socket.timeout) as exc:
            logger.warning(
                "Node graph review network error: %s", type(exc).__name__
            )
            return self._error(
                "AI_NETWORK_ERROR",
                "暂时无法连接 DeepSeek，节点审查将在下一轮重试。",
            )
        except Exception as exc:
            logger.exception("Node graph review failed: %s", type(exc).__name__)
            return self._error(
                "AI_REVIEW_FAILED", "节点逻辑审查失败，将在下一轮自动重试。"
            )

    @staticmethod
    def _error(code: str, message: str) -> dict[str, Any]:
        return {
            "success": False,
            "status": "error",
            "error": code,
            "message": message,
        }

    @staticmethod
    def _normalize_payload(payload: Any) -> dict[str, Any]:
        if isinstance(payload, str):
            payload = json.loads(payload)
        if not isinstance(payload, dict):
            raise ValueError("节点审查请求必须是对象")

        workspace = payload.get("workspace")
        if not isinstance(workspace, dict):
            raise ValueError("缺少节点 workspace")
        nodes = workspace.get("nodes")
        edges = workspace.get("edges")
        if not isinstance(nodes, list) or not isinstance(edges, list):
            raise ValueError("workspace.nodes 和 workspace.edges 必须是数组")

        revision = str(payload.get("graphRevision") or "").strip()
        if not revision:
            raise ValueError("缺少 graphRevision")

        return {
            "schemaVersion": 1,
            "requestId": str(payload.get("requestId") or ""),
            "graphRevision": revision,
            "targetId": "node_graph:project:global",
            "workspace": {
                "version": int(workspace.get("version") or 1),
                "nodes": nodes,
                "edges": edges,
                "globalVariablesWorkspace": workspace.get(
                    "globalVariablesWorkspace"
                )
                or {},
            },
            "projectContext": payload.get("projectContext")
            if isinstance(payload.get("projectContext"), dict)
            else {},
        }

    @classmethod
    def _resolve_settings(cls, purpose: str | None = None) -> DeepSeekSettings:
        def lookup_editor_configuration() -> tuple[Any, dict[str, Any], dict[str, Any]]:
            provider: Any = None
            raw_provider: dict[str, Any] = {}
            raw_settings: dict[str, Any] = {}
            try:
                from Quasar.ai_service.entrance import get_ai_entrance

                collector = get_ai_entrance().collector
                raw = getattr(collector, "AI_SETTINGS", {})
                raw_settings = raw if isinstance(raw, dict) else {}
                purpose_config = (
                    raw_settings.get(purpose)
                    if purpose and isinstance(raw_settings.get(purpose), dict)
                    else {}
                )
                provider_name = str(purpose_config.get("provider") or "deepseek").strip()
                providers = getattr(collector.AIConfig, "providers", {}) or {}
                provider = providers.get(provider_name) if hasattr(providers, "get") else None
                raw_providers = raw_settings.get("providers")
                if isinstance(raw_providers, list):
                    raw_provider = next(
                        (
                            item
                            for item in raw_providers
                            if isinstance(item, dict)
                            and str(item.get("name") or "").strip() == provider_name
                        ),
                        {},
                    )
                elif isinstance(raw_providers, dict):
                    candidate = raw_providers.get(provider_name)
                    raw_provider = candidate if isinstance(candidate, dict) else {}
            except Exception as exc:
                logger.debug(
                    "DeepSeek editor configuration lookup failed: %s",
                    type(exc).__name__,
                )
            return provider, raw_provider, raw_settings

        provider, raw_provider, raw_settings = lookup_editor_configuration()

        def read_provider(name: str) -> str:
            value = getattr(provider, name, "") if provider is not None else ""
            return str(value or raw_provider.get(name, "") or "").strip()

        editor_key = read_provider("api_key")
        if not editor_key:
            # Review/generation can run before the LAN-chat worker finishes warm-up.
            # Lazily register the editor-owned settings before falling back to env.
            try:
                from ..configuration.local_secrets import load_ai_setting

                load_ai_setting()
            except Exception as exc:
                logger.debug(
                    "DeepSeek editor settings lazy-load failed: %s",
                    type(exc).__name__,
                )
            provider, raw_provider, raw_settings = lookup_editor_configuration()
            editor_key = read_provider("api_key")

        purpose_config = (
            raw_settings.get(purpose)
            if purpose and isinstance(raw_settings.get(purpose), dict)
            else {}
        )

        def as_float(value: Any, default: float) -> float:
            try:
                return float(value)
            except (TypeError, ValueError):
                return default

        def as_int(value: Any, default: int) -> int:
            try:
                parsed = int(value)
            except (TypeError, ValueError):
                return default
            return parsed if parsed > 0 else default

        if purpose == "node_graph":
            model = (
                str(purpose_config.get("model") or "").strip()
                or read_provider("model")
                or cls.DEFAULT_MODEL
            )
            temperature = as_float(purpose_config.get("temperature"), 0.05)
            max_tokens = as_int(purpose_config.get("max_tokens"), 12000)
            thinking_enabled = purpose_config.get("thinking") is True
        else:
            model = read_provider("model") or cls.DEFAULT_MODEL
            temperature = 0.1
            max_tokens = 1200
            thinking_enabled = False

        base_url = read_provider("base_url") or cls.DEFAULT_BASE_URL
        return DeepSeekSettings(
            editor_key,
            base_url,
            model,
            "editor-ai-setting" if editor_key else "unconfigured",
            temperature,
            max_tokens,
            thinking_enabled,
        )

    @classmethod
    def _call_deepseek(cls, settings: DeepSeekSettings, prompt: str) -> str:
        base = settings.base_url.rstrip("/")
        endpoint = base if base.endswith("/chat/completions") else base + "/chat/completions"
        body = {
            "model": settings.model,
            "temperature": 0.1,
            "max_tokens": 1200,
            "response_format": {"type": "json_object"},
            "thinking": {"type": "disabled"},
            "messages": [
                {
                    "role": "system",
                    "content": (
                        "你是 CoronaEngine 3D 游戏节点逻辑审查员。只审查给定的可见节点、"
                        "连线、条件和积木，不编造对象或积木。发现问题时合并成一句委婉、"
                        "可执行的中文提示，句式接近‘……有问题，原因是……；这样做就好了。’"
                        "只输出 JSON。"
                    ),
                },
                {"role": "user", "content": prompt},
            ],
        }
        request = urllib.request.Request(
            endpoint,
            data=json.dumps(body, ensure_ascii=False).encode("utf-8"),
            headers={
                "Authorization": "Bearer " + settings.api_key,
                "Content-Type": "application/json",
            },
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=cls.TIMEOUT_SECONDS) as response:
            data = json.loads(response.read().decode("utf-8"))

        choices = data.get("choices") if isinstance(data, dict) else None
        message = (
            choices[0].get("message")
            if isinstance(choices, list)
            and choices
            and isinstance(choices[0], dict)
            else None
        )
        content = message.get("content") if isinstance(message, dict) else None
        if not isinstance(content, str) or not content.strip():
            raise ValueError("DeepSeek 返回了空内容")
        return content.strip()

    @staticmethod
    def _parse_model_result(text: str) -> dict[str, Any]:
        cleaned = text.strip()
        fence = chr(96) * 3
        if cleaned.startswith(fence):
            cleaned = cleaned.split("\n", 1)[1] if "\n" in cleaned else cleaned[3:]
            if cleaned.rstrip().endswith(fence):
                cleaned = cleaned.rstrip()[:-3]
            cleaned = cleaned.strip()

        try:
            value = json.loads(cleaned)
        except json.JSONDecodeError:
            start = cleaned.find("{")
            if start < 0:
                raise ValueError("DeepSeek 未返回 JSON")
            value, _ = json.JSONDecoder().raw_decode(cleaned[start:])
        if not isinstance(value, dict):
            raise ValueError("DeepSeek 审查结果必须是 JSON 对象")
        return value

    @classmethod
    def _validate_model_result(
        cls, result: dict[str, Any], request: dict[str, Any]
    ) -> None:
        if not isinstance(result.get("hasProblems"), bool):
            raise ValueError("DeepSeek 审查结果缺少 hasProblems")

        project_context = request.get("projectContext") if isinstance(request.get("projectContext"), dict) else {}
        optimization_enabled = project_context.get("optimizationHintsEnabled") is True
        raw_tip = result.get("optimizationTip")
        normalized_tip = None
        if optimization_enabled and isinstance(raw_tip, dict):
            tip_key = str(raw_tip.get("tipKey") or "").strip()[:120]
            title = str(raw_tip.get("title") or "").strip()[:80]
            message = str(raw_tip.get("message") or "").strip()[:360]
            title_en = str(raw_tip.get("titleEn") or title).strip()[:80]
            message_en = str(raw_tip.get("messageEn") or message).strip()[:360]
            if tip_key and title and message:
                normalized_tip = {
                    "tipKey": tip_key,
                    "title": title,
                    "titleEn": title_en,
                    "message": message,
                    "messageEn": message_en,
                }

        summary = result.get("summary", "")
        if result["hasProblems"] and (
            not isinstance(summary, str) or not summary.strip()
        ):
            raise ValueError("DeepSeek 审查结果缺少可显示的 summary")
        result["summary"] = summary.strip()[:160] if isinstance(summary, str) else ""
        summary_en = result.get("summaryEn", "")
        result["summaryEn"] = (
            summary_en.strip()[:160]
            if isinstance(summary_en, str) and summary_en.strip()
            else result["summary"]
        )

        issues = result.get("issues", [])
        if not isinstance(issues, list):
            raise ValueError("DeepSeek issues 必须是数组")
        if not result["hasProblems"]:
            result["summary"] = ""
            result["summaryEn"] = ""
            result["issues"] = []
            result["optimizationTip"] = normalized_tip
            return

        result["optimizationTip"] = None
        node_ids = {
            str(node.get("id"))
            for node in request["workspace"]["nodes"]
            if isinstance(node, dict)
        }
        blocks_by_id = {
            str(block.get("id")): block
            for block in cls._walk_blocks(request["workspace"])
            if block.get("id")
        }
        block_ids = set(blocks_by_id)
        edge_ids = {
            str(edge.get("id"))
            for edge in request["workspace"].get("edges", [])
            if isinstance(edge, dict) and edge.get("id")
        }
        facts = cls._collect_local_facts(request["workspace"], project_context)
        normalized: list[dict[str, Any]] = []
        for item in issues[:6]:
            if not isinstance(item, dict):
                continue
            node_id = str(item.get("nodeId") or "")
            block_id = str(item.get("blockId") or "")
            edge_id = str(item.get("edgeId") or "")
            if node_id and node_id not in node_ids:
                continue
            if block_id and block_id not in block_ids:
                continue
            if edge_id and edge_id not in edge_ids:
                continue
            try:
                confidence = max(
                    0.0, min(1.0, float(item.get("confidence") or 0.0))
                )
            except (TypeError, ValueError):
                confidence = 0.0
            if confidence < 0.8:
                continue
            code = str(item.get("code") or "logic_issue")[:80]
            code = cls.ISSUE_CODE_ALIASES.get(code, code)
            matching_fact = next((fact for fact in facts if (
                cls.ISSUE_CODE_ALIASES.get(str(fact.get("code") or ""), str(fact.get("code") or "")) == code
                and (not node_id or not fact.get("nodeId") or str(fact.get("nodeId")) == node_id)
                and (not block_id or not fact.get("blockId") or str(fact.get("blockId")) == block_id)
                and (not edge_id or not fact.get("edgeId") or str(fact.get("edgeId")) == edge_id)
            )), None)
            # Object existence and required-input errors are deterministic. DeepSeek
            # may explain them, but it cannot create them without a matching local fact.
            if code in cls.FACT_REQUIRED_ISSUE_CODES and not matching_fact:
                continue
            actual_block = blocks_by_id.get(block_id) or {}
            if cls._issue_reports_legal_optional_empty_field(item, actual_block):
                continue
            if not edge_id and matching_fact and str(matching_fact.get("edgeId") or "") in edge_ids:
                edge_id = str(matching_fact.get("edgeId") or "")
            raw_pattern = item.get("pattern") if isinstance(item.get("pattern"), dict) else {}
            pattern = cls._normalize_issue_pattern(raw_pattern)
            if actual_block.get("type"):
                pattern["blockType"] = str(actual_block.get("type"))[:180]
            if matching_fact:
                if matching_fact.get("blockType"):
                    pattern["blockType"] = str(matching_fact.get("blockType"))[:180]
                if matching_fact.get("field"):
                    pattern["missingInput"] = str(matching_fact.get("field"))[:180]
            if code in {"missing_actor_target", "actor_target_not_found"}:
                pattern["objectRequirement"] = "scene_actor"
            if code in {"invalid_visible_condition_count", "non_boolean_condition"}:
                pattern["workspaceRole"] = "condition"
                pattern["relationType"] = "transition"
            elif code == "invalid_edge_endpoint":
                pattern["relationType"] = "transition"
            elif code == "start_node_count":
                pattern["workspaceRole"] = "node_graph"
            if edge_id:
                pattern["edgeId"] = edge_id
            title = str(item.get("title") or "节点逻辑需要调整").strip()[:80]
            message = str(item.get("message") or result["summary"]).strip()[:500]
            suggestion = str(item.get("suggestion") or result["summary"]).strip()[:500]
            title_en = str(item.get("titleEn") or title).strip()[:80]
            message_en = str(item.get("messageEn") or result["summaryEn"] or message).strip()[:500]
            suggestion_en = str(item.get("suggestionEn") or result["summaryEn"] or suggestion).strip()[:500]
            normalized.append(
                {
                    "issueKey": f"{code}|{node_id}|{block_id}" + (f"|{edge_id}" if edge_id else ""),
                    "severity": str(item.get("severity") or "warning")[:16],
                    "confidence": confidence,
                    "nodeId": node_id,
                    "blockId": block_id,
                    "edgeId": edge_id,
                    "code": code,
                    "pattern": pattern,
                    "title": title,
                    "titleEn": title_en,
                    "message": message,
                    "messageEn": message_en,
                    "suggestion": suggestion,
                    "suggestionEn": suggestion_en,
                }
            )
        if result["hasProblems"] and not normalized:
            result["hasProblems"] = False
            result["summary"] = ""
            result["summaryEn"] = ""
            result["issues"] = []
            result["optimizationTip"] = normalized_tip
            return
        result["issues"] = normalized

    @classmethod
    def _issue_reports_legal_optional_empty_field(
        cls, item: dict[str, Any], block: dict[str, Any]
    ) -> bool:
        block_type = str(block.get("type") or "")
        contract = cls._catalog_index().get(block_type) or {}
        fields = block.get("fields") if isinstance(block.get("fields"), dict) else {}
        searchable = " ".join(
            str(item.get(key) or "")
            for key in ("code", "title", "message", "suggestion")
        ).casefold()
        pattern = item.get("pattern") if isinstance(item.get("pattern"), dict) else {}
        missing_input = str(pattern.get("missingInput") or "").strip()
        for field_contract in contract.get("fields") or []:
            field_name = str(field_contract.get("name") or "").strip()
            if not field_name or field_contract.get("required") is not False:
                continue
            value = fields.get(field_name)
            if value not in (None, ""):
                continue
            aliases = {field_name.casefold(), field_name.replace("_", "").casefold()}
            aliases.update(cls.OPTIONAL_FIELD_ALIASES.get(field_name, ()))
            mentioned = missing_input.casefold() == field_name.casefold() or any(
                str(alias).casefold() in searchable for alias in aliases if alias
            )
            if mentioned:
                return True
        return False

    @classmethod
    def _normalize_issue_pattern(cls, raw: Any) -> dict[str, str]:
        if not isinstance(raw, dict):
            return {}
        normalized: dict[str, str] = {}
        for field in cls.ISSUE_PATTERN_FIELDS:
            value = str(raw.get(field) or "").strip()[:180]
            if value:
                normalized[field] = value
        return normalized

    @staticmethod
    def _walk_blocks(value: Any) -> Iterable[dict[str, Any]]:
        if isinstance(value, dict):
            if isinstance(value.get("type"), str):
                yield value
            for child in value.values():
                yield from NodeGraphReviewService._walk_blocks(child)
        elif isinstance(value, list):
            for child in value:
                yield from NodeGraphReviewService._walk_blocks(child)

    @classmethod
    def _normalize_actor_name(cls, value: Any) -> str:
        if isinstance(value, dict):
            value = value.get("name") or value.get("value") or value.get("id") or ""
        if value is None:
            return ""
        return unicodedata.normalize("NFKC", str(value)).strip()

    @classmethod
    def _actor_name_key(cls, value: Any) -> str:
        return cls._normalize_actor_name(value).casefold()

    @classmethod
    def _actor_names_from_context(cls, actor: Any) -> set[str]:
        if not isinstance(actor, dict):
            name = cls._normalize_actor_name(actor)
            return {name} if name else set()
        names: set[str] = set()
        for field in cls.ACTOR_NAME_FIELDS:
            name = cls._normalize_actor_name(actor.get(field))
            if name:
                names.add(name)
        for field in cls.ACTOR_ALIAS_FIELDS:
            aliases = actor.get(field)
            if isinstance(aliases, dict):
                aliases = list(aliases.values())
            if not isinstance(aliases, (list, tuple, set)):
                aliases = [aliases]
            for alias in aliases:
                name = cls._normalize_actor_name(alias)
                if name:
                    names.add(name)
        return names

    @classmethod
    def _is_missing_actor_name(cls, value: Any) -> bool:
        name = cls._actor_name_key(value)
        return name in {cls._actor_name_key(item) for item in cls.ACTOR_PLACEHOLDERS}

    @classmethod
    def _connected_actor_reference(
        cls, block: dict[str, Any], input_name: str
    ) -> tuple[str, str]:
        """Return (resolved|missing|dynamic|absent, actor_name)."""
        inputs = block.get("inputs") if isinstance(block.get("inputs"), dict) else {}
        connection = inputs.get(input_name)
        if not isinstance(connection, dict):
            return "absent", ""
        child = connection.get("block")
        if not isinstance(child, dict):
            child = connection.get("shadow")
        if not isinstance(child, dict):
            return "missing", ""

        child_type = str(child.get("type") or "")
        fields = child.get("fields") if isinstance(child.get("fields"), dict) else {}
        if child_type == "text":
            name = cls._normalize_actor_name(fields.get("TEXT"))
            return ("missing", "") if cls._is_missing_actor_name(name) else ("resolved", name)
        if child_type == "object_reference":
            selected = cls._normalize_actor_name(fields.get("OBJECT"))
            if selected == "__manual__":
                manual = cls._normalize_actor_name(fields.get("MANUAL"))
                return ("missing", "") if cls._is_missing_actor_name(manual) else ("resolved", manual)
            # Empty OBJECT means the old implicit current actor, which does not
            # exist in the project-global node graph.
            return ("missing", "") if cls._is_missing_actor_name(selected) else ("resolved", selected)

        # Variables, functions and composed text may resolve to an actor at
        # runtime. They are not deterministic enough for a local error fact.
        return "dynamic", ""

    @classmethod
    def _actor_reference(
        cls, block: dict[str, Any], field_name: str
    ) -> tuple[str, str]:
        connected_state, connected_name = cls._connected_actor_reference(block, field_name)
        if connected_state != "absent":
            return connected_state, connected_name

        fields = block.get("fields") if isinstance(block.get("fields"), dict) else {}
        aliases = (field_name, f"{field_name}_TEXT")
        present = False
        for alias in aliases:
            if alias not in fields:
                continue
            present = True
            name = cls._normalize_actor_name(fields.get(alias))
            if not cls._is_missing_actor_name(name):
                return "resolved", name
        configured = field_name in cls.ACTOR_REFERENCE_FIELDS.get(str(block.get("type") or ""), ())
        return ("missing", "") if present or configured else ("absent", "")

    @classmethod
    def _collect_local_facts(
        cls, workspace: dict[str, Any], project_context: dict[str, Any] | None = None
    ) -> list[dict[str, Any]]:
        """只收集能够确定的结构事实，不用固定玩法模板推断错误。"""
        nodes = [node for node in workspace.get("nodes", []) if isinstance(node, dict)]
        edges = [edge for edge in workspace.get("edges", []) if isinstance(edge, dict)]
        facts: list[dict[str, Any]] = []

        node_id_values = [str(node.get("id") or "").strip() for node in nodes]
        node_ids = {node_id for node_id in node_id_values if node_id}
        if any(not node_id for node_id in node_id_values):
            facts.append({"code": "missing_node_id", "detail": "存在没有 ID 的节点"})
        duplicate_node_ids = sorted(
            node_id for node_id in node_ids if node_id_values.count(node_id) > 1
        )
        if duplicate_node_ids:
            facts.append(
                {
                    "code": "duplicate_node_id",
                    "detail": "节点 ID 重复：" + "、".join(duplicate_node_ids[:6]),
                }
            )

        starts = [node for node in nodes if node.get("nodeType") == "start"]
        if len(starts) != 1:
            facts.append(
                {
                    "code": "start_node_count",
                    "detail": f"开始节点数量为 {len(starts)}，可运行节点图必须恰好有 1 个开始节点",
                }
            )

        edge_id_values = [str(edge.get("id") or "").strip() for edge in edges]
        edge_ids = {edge_id for edge_id in edge_id_values if edge_id}
        if any(not edge_id for edge_id in edge_id_values):
            facts.append({"code": "missing_edge_id", "detail": "存在没有 ID 的连线"})
        duplicate_edge_ids = sorted(
            edge_id for edge_id in edge_ids if edge_id_values.count(edge_id) > 1
        )
        if duplicate_edge_ids:
            facts.append(
                {
                    "code": "duplicate_edge_id",
                    "detail": "连线 ID 重复：" + "、".join(duplicate_edge_ids[:6]),
                }
            )

        catalog = cls._catalog_index()
        all_blocks = list(cls._walk_blocks(workspace))
        block_scopes: list[tuple[str, Any]] = [
            ("全局变量池", workspace.get("globalVariablesWorkspace") or {})
        ]
        block_scopes.extend(
            (f"节点 {str(node.get('id') or '')}", node.get("workspace") or {})
            for node in nodes
        )
        block_scopes.extend(
            (f"连线 {str(edge.get('id') or '')}", edge.get("conditionWorkspace") or {})
            for edge in edges
        )
        for scope_name, scope_workspace in block_scopes:
            scope_blocks = list(cls._walk_blocks(scope_workspace))
            block_id_values = [
                str(block.get("id") or "").strip() for block in scope_blocks
            ]
            if any(not block_id for block_id in block_id_values):
                facts.append(
                    {
                        "code": "missing_block_id",
                        "detail": f"{scope_name}中存在没有 ID 的积木",
                    }
                )
            duplicate_block_ids = sorted(
                {
                    block_id
                    for block_id in block_id_values
                    if block_id and block_id_values.count(block_id) > 1
                }
            )
            if duplicate_block_ids:
                facts.append(
                    {
                        "code": "duplicate_block_id",
                        "detail": f"{scope_name}中的积木 ID 重复："
                        + "、".join(duplicate_block_ids[:6]),
                    }
                )

        if catalog:
            unknown_types = sorted(
                {
                    str(block.get("type") or "").strip()
                    for block in all_blocks
                    if str(block.get("type") or "").strip()
                    and str(block.get("type") or "").strip() not in catalog
                }
            )
            if unknown_types:
                facts.append(
                    {
                        "code": "unknown_block_type",
                        "detail": "存在引擎未登记的积木类型：" + "、".join(unknown_types[:8]),
                    }
                )

        for edge in edges:
            edge_id = str(edge.get("id") or "")
            source_value = edge.get("source") if isinstance(edge.get("source"), dict) else {}
            target_value = edge.get("target") if isinstance(edge.get("target"), dict) else {}
            source = str(source_value.get("nodeId") or "")
            target = str(target_value.get("nodeId") or "")
            if source not in node_ids or target not in node_ids:
                facts.append(
                    {
                        "code": "dangling_edge",
                        "edgeId": edge_id,
                        "detail": "连线端点引用了不存在的节点",
                    }
                )

            condition_workspace = edge.get("conditionWorkspace") or {}
            top_blocks = cls._top_level_blocks(condition_workspace)
            if len(top_blocks) != 1:
                facts.append(
                    {
                        "code": "invalid_visible_condition_count",
                        "edgeId": edge_id,
                        "detail": f"连线条件顶层积木数量为 {len(top_blocks)}，必须恰好为 1",
                    }
                )
                continue
            condition_type = str(top_blocks[0].get("type") or "")
            condition_contract = catalog.get(condition_type) if catalog else None
            if condition_contract and condition_contract.get("outputCheck") != "Boolean":
                facts.append(
                    {
                        "code": "non_boolean_condition",
                        "edgeId": edge_id,
                        "blockId": str(top_blocks[0].get("id") or ""),
                        "detail": f"连线条件积木 {condition_type} 的输出不是 Boolean",
                    }
                )

        context = project_context or {}
        actors_value = context.get("actors")
        availability_flag = context.get("actorContextAvailable")
        actor_context_available = (
            availability_flag is True
            or (availability_flag is None and isinstance(actors_value, list))
        )
        known_actors = {
            cls._actor_name_key(name)
            for actor in (actors_value or [])
            for name in cls._actor_names_from_context(actor)
            if cls._actor_name_key(name)
        }

        scoped_blocks: list[tuple[str, dict[str, Any]]] = []
        for node in nodes:
            node_id = str(node.get("id") or "")
            scoped_blocks.extend(
                (node_id, block) for block in cls._walk_blocks(node.get("workspace") or {})
            )
        scoped_blocks.extend(
            ("", block)
            for block in cls._walk_blocks(workspace.get("globalVariablesWorkspace") or {})
        )
        scoped_blocks.extend(
            ("", block)
            for edge in edges
            for block in cls._walk_blocks(edge.get("conditionWorkspace") or {})
        )

        for node_id, block in scoped_blocks:
            block_type = str(block.get("type") or "")
            block_id = str(block.get("id") or "")
            contract = catalog.get(block_type) if catalog else None
            if contract and contract.get("projectUsage") == "actor-context":
                facts.append(
                    {
                        "code": "missing_actor_target",
                        "nodeId": node_id,
                        "blockId": block_id,
                        "blockType": block_type,
                        "detail": f"积木 {block_type} 依赖绑定脚本的当前物体，但项目全局节点没有隐式物体上下文",
                        "suggestion": "改用带对象名称或标签参数的项目级积木，并选择当前场景中的具体物体",
                    }
                )
                continue

            for field_name in cls.ACTOR_REFERENCE_FIELDS.get(block_type, ()):
                state, actor_name = cls._actor_reference(block, field_name)
                if state == "missing":
                    facts.append(
                        {
                            "code": "missing_actor_target",
                            "nodeId": node_id,
                            "blockId": block_id,
                            "blockType": block_type,
                            "field": field_name,
                            "detail": f"积木 {block_type} 的对象参数 {field_name} 没有指定具体物体",
                            "suggestion": "在该对象参数中选择当前场景里的目标物体",
                        }
                    )
                elif (
                    state == "resolved"
                    and actor_context_available
                    and cls._actor_name_key(actor_name) not in known_actors
                ):
                    facts.append(
                        {
                            "code": "actor_target_not_found",
                            "nodeId": node_id,
                            "blockId": block_id,
                            "blockType": block_type,
                            "field": field_name,
                            "actorName": actor_name,
                            "detail": f"积木 {block_type} 指向的对象 {actor_name} 在当前场景中不存在",
                            "suggestion": "改成场景已有对象名称，或先创建同名对象",
                        }
                    )
        return facts

    @staticmethod
    def _top_level_blocks(workspace: Any) -> list[dict[str, Any]]:
        if not isinstance(workspace, dict):
            return []
        blocks_container = workspace.get("blocks")
        if not isinstance(blocks_container, dict):
            return []
        blocks = blocks_container.get("blocks")
        if not isinstance(blocks, list):
            return []
        return [block for block in blocks if isinstance(block, dict)]

    @classmethod
    @lru_cache(maxsize=1)
    def _catalog_index(cls) -> dict[str, dict[str, Any]]:
        try:
            root = ET.parse(cls._find_contract_path()).getroot()
        except Exception as exc:
            logger.warning(
                "Node graph review catalog unavailable: %s", type(exc).__name__
            )
            return {}

        index: dict[str, dict[str, Any]] = {}
        for element in root.findall(".//Block"):
            block_type = str(element.get("type") or "")
            if not block_type:
                continue
            index[block_type] = {
                "type": block_type,
                "category": str(element.get("category") or ""),
                "shape": str(element.get("shape") or ""),
                "outputCheck": str(element.get("outputCheck") or ""),
                "projectUsage": str(element.get("projectUsage") or ""),
                "label": str(element.get("label") or ""),
                "inputs": [
                    {
                        "name": str(item.get("name") or ""),
                        "kind": str(item.get("kind") or ""),
                        "check": str(item.get("check") or ""),
                        "required": str(item.get("required") or "true").strip().casefold() != "false",
                        "emptyMeaning": str(item.get("emptyMeaning") or ""),
                    }
                    for item in element.findall("Input")
                ],
                "fields": [
                    {
                        "name": str(item.get("name") or ""),
                        "kind": str(item.get("kind") or ""),
                        "required": str(item.get("required") or "true").strip().casefold() != "false",
                        "emptyMeaning": str(item.get("emptyMeaning") or ""),
                    }
                    for item in element.findall("Field")
                ],
            }
        return index

    @classmethod
    def _catalog_summary(cls, workspace: dict[str, Any]) -> list[dict[str, Any]]:
        used_types = sorted(
            {
                str(block.get("type") or "")
                for block in cls._walk_blocks(workspace)
                if block.get("type")
            }
        )
        index = cls._catalog_index()
        return [index[block_type] for block_type in used_types if block_type in index]

    @staticmethod
    def _build_prompt(
        request: dict[str, Any],
        facts: list[dict[str, Any]],
        catalog: list[dict[str, Any]],
    ) -> str:
        graph_json = json.dumps(
            request["workspace"], ensure_ascii=False, separators=(",", ":")
        )
        facts_json = json.dumps(facts, ensure_ascii=False, separators=(",", ":"))
        context_json = json.dumps(
            request.get("projectContext") or {},
            ensure_ascii=False,
            separators=(",", ":"),
        )
        catalog_json = json.dumps(
            catalog, ensure_ascii=False, separators=(",", ":")
        )
        project_context = request.get("projectContext") or {}
        assistance_profile = (
            project_context.get("assistanceProfile")
            if isinstance(project_context.get("assistanceProfile"), dict)
            else {}
        )
        try:
            score = max(
                0,
                min(
                    int(
                        round(
                            float(
                                assistance_profile.get(
                                    "score", assistance_profile.get("fluencyScore", 0)
                                )
                                or 0
                            )
                        )
                    ),
                    100,
                ),
            )
        except (TypeError, ValueError):
            score = 0
        has_score = int(assistance_profile.get("updatedAt") or 0) > 0
        optimization_enabled = project_context.get("optimizationHintsEnabled") is True

        if not has_score:
            score_instruction = (
                "尚无稳定的操作评分。请使用平和、清楚、适中详细度的表达，"
                "既说明问题影响，也给出可执行的修复步骤。"
            )
        elif score >= 75:
            score_instruction = (
                f"内部操作评分为 {score}/100。请给用户保留更多自主编辑空间，回答简洁、专业，"
                "不要展开基础教学。仅在与当前问题直接相关时，补充状态机、控制流、数据流、"
                "Boolean 求值、对象引用、实时计算机图形学、变换或物理方面的专业知识。"
            )
        elif score <= 45:
            score_instruction = (
                f"内部操作评分为 {score}/100。请使用平和、通俗的语言，减少专业术语，"
                "明确说明需要点击、拖拽、连接或修改哪个节点和积木，并说明如何验证修复结果。"
            )
        else:
            score_instruction = (
                f"内部操作评分为 {score}/100。请保持适中详细度，给出关键操作步骤，"
                "必要时解释少量与当前问题直接相关的术语。"
            )

        optimization_instruction = (
            "若确认当前节点逻辑没有错误，可以额外给出一条与现有逻辑直接相关的优化建议。"
            "此时填写 optimizationTip={tipKey,title,titleEn,message,messageEn}，建议只能改善当前控制流、数据流、"
            "对象引用、可读性或稳定性，不得要求用户添加额外玩法；没有可靠建议时返回 null。"
            if optimization_enabled
            else "本次不需要优化建议，optimizationTip 必须返回 null。"
        )
        return (
            "你是 CoronaEngine 节点逻辑审查助手。请在游戏运行前审查项目级节点图。\n"
            "只判断当前可见节点、连线、跳转条件和积木是否存在会导致逻辑错误、无法执行、"
            "对象引用缺失或结果明显不符合当前逻辑意图的问题。不要评价玩法是否丰富，"
            "不要因为 Demo 简单就建议增加功能。\n"
            "本地事实是确定性线索，必须优先核对。发现真实问题时 hasProblems=true；"
            "没有真实问题时 hasProblems=false。不要把布局位置、节点数量少或缺少额外玩法当成错误。"
            "对象相关错误只能在本地确定性事实明确给出时返回：对象字段为空或占位时使用 "
            "missing_actor_target，引用的名称在可靠场景对象列表中不存在时使用 actor_target_not_found；"
            "不得只凭模型猜测对象不存在。"
            "积木合同中 required=false 的字段允许为空，不能作为 missing_required_input；"
            "其中 object_third_person_move 和 object_first_person_move 的 OBSTACLE_TAG 为空表示关闭标签障碍检测，"
            "不得要求用户虚构或补填障碍标签。"
            "issues 中的 nodeId 和 blockId 必须引用输入中真实存在的 ID；无法定位时可留空，不能编造。\n"
            + score_instruction
            + "\n"
            + optimization_instruction
            + "\n"
            "不要在输出中显示内部评分，也不要给用户贴美术、程序、入门、熟悉或熟练标签。\n"
            "只返回一个 JSON 对象，不要 Markdown。无问题示例："
            '{"hasProblems":false,"summary":"","summaryEn":"","issues":[],"optimizationTip":null}'
            "；若有可靠优化建议，可将 optimizationTip 替换为"
            '{"tipKey":"stable_tip_key","title":"中文短标题","titleEn":"short English title",'
            '"message":"中文建议","messageEn":"relevant English suggestion"}。\n'
            "有问题示例："
            '{"hasProblems":true,"summary":"中文问题总结","summaryEn":"English issue summary",'
            '"issues":[{"severity":"warning","confidence":0.95,"nodeId":"real node ID or empty",'
            '"blockId":"real block ID or empty","code":"stable_issue_code","title":"中文短标题",'
            '"titleEn":"short English title","message":"中文原因","messageEn":"English cause",'
            '"suggestion":"中文修复建议","suggestionEn":"specific English fix"}],"optimizationTip":null}。\n'
            "summary、title、message、suggestion 使用自然中文；summaryEn、titleEn、messageEn、suggestionEn "
            "必须提供含义一致的自然英文。summary 和 summaryEn 各不超过 160 字符。"
            "issues 用于任务定位，内容必须与 summary 一致。\n"
            "本地确定性事实："
            + facts_json
            + "\n项目上下文："
            + context_json
            + "\n相关积木能力摘要："
            + catalog_json
            + "\n节点图："
            + graph_json
        )


_SERVICE = NodeGraphReviewService()


def get_node_graph_review_service() -> NodeGraphReviewService:
    return _SERVICE

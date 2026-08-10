"""Low-risk adapters from existing capabilities into AgentRuntime providers.

These adapters are intentionally narrow and capability-oriented.
They expose function-sized resource providers to ToolCallGraph execution without
keeping provider and engine integration behind the canonical runtime boundary.
"""

from __future__ import annotations

import base64
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import re
import threading
import time
from typing import Any, Callable, Mapping
from urllib.parse import unquote, urlparse

from ..gameplay_contracts import validate_gameplay_manifest_payload
from ..schema_versions import (
    ENGINE_SNAPSHOT_INPUT_CONTRACT_VERSION,
    PLAN_PATCH_PAYLOAD_SCHEMA_VERSION,
)
from .engine_snapshot_input import (
    EngineSnapshotInputContractError,
    current_unversioned_v1_schema_fingerprint,
    validate_current_unversioned_v1_snapshot,
)
from .support_semantics import classify_support_type
from .tools import ResourceProvider


@dataclass(frozen=True)
class RuntimeCppBridgeResult:
    """Normalized result for a single C++/engine binding call."""

    success: bool
    payload: dict[str, Any]
    error_code: str = ""
    message: str = ""
    boundary_fact: dict[str, Any] | None = None


class RuntimeCppBridge:
    """Narrow Runtime boundary for C++/engine writes.

    The bridge does not know about workflow orchestration.  It only invokes an already selected low-level binding through
    EngineWriteGate and normalizes the binding result into a Runtime-safe shape.
    """

    def __init__(
        self,
        *,
        engine_gate: Any,
        parse_result: Callable[[Any], dict[str, Any]] | None = None,
    ) -> None:
        if engine_gate is None:
            raise ValueError("engine_gate is required")
        self._engine_gate = engine_gate
        self._parse_result = parse_result

    def invoke_tool(
        self,
        tool: Any,
        payload: dict[str, Any],
        *,
        error_code: str = "cpp_tool_failed",
    ) -> RuntimeCppBridgeResult:
        method = "invoke_tool"
        if tool is None:
            return self._result(
                False,
                {},
                error_code="cpp_tool_missing",
                message="C++ tool is missing",
                method=method,
            )
        invoke_tool = getattr(self._engine_gate, "invoke_tool", None)
        if not callable(invoke_tool):
            return self._result(
                False,
                {},
                error_code="cpp_gate_method_missing",
                message="C++ engine write gate method is missing",
                method=method,
            )
        try:
            raw = invoke_tool(tool, payload)
            return self._normalize(raw, error_code=error_code, method=method)
        except Exception:  # noqa: BLE001
            return self._result(False, {}, error_code=error_code, message="C++ tool failed", method=method)

    def set_transform(
        self,
        tool: Any,
        payload: dict[str, Any],
        *,
        error_code: str = "cpp_transform_failed",
    ) -> RuntimeCppBridgeResult:
        method = "set_transform"
        if tool is None:
            return self._result(
                False,
                {},
                error_code="cpp_tool_missing",
                message="C++ transform tool is missing",
                method=method,
            )
        set_transform = getattr(self._engine_gate, "set_transform", None)
        if not callable(set_transform):
            return self._result(
                False,
                {},
                error_code="cpp_gate_method_missing",
                message="C++ engine write gate method is missing",
                method=method,
            )
        try:
            raw = set_transform(tool, payload)
            return self._normalize(raw, error_code=error_code, method=method)
        except Exception:  # noqa: BLE001
            return self._result(False, {}, error_code=error_code, message="C++ transform failed", method=method)

    def remove_actor(
        self,
        tool: Any,
        payload: dict[str, Any],
        *,
        error_code: str = "cpp_actor_delete_failed",
    ) -> RuntimeCppBridgeResult:
        method = "remove_actor"
        if tool is None:
            return self._result(
                False,
                {},
                error_code="cpp_tool_missing",
                message="C++ delete tool is missing",
                method=method,
            )
        remove_actor = getattr(self._engine_gate, "remove_actor", None)
        if not callable(remove_actor):
            return self._result(
                False,
                {},
                error_code="cpp_gate_method_missing",
                message="C++ engine write gate method is missing",
                method=method,
            )
        try:
            raw = remove_actor(tool, payload)
            return self._normalize(raw, error_code=error_code, method=method)
        except Exception:  # noqa: BLE001
            return self._result(False, {}, error_code=error_code, message="C++ actor delete failed", method=method)

    def _normalize(self, raw: Any, *, error_code: str, method: str) -> RuntimeCppBridgeResult:
        parsed = self._parse_result(raw) if self._parse_result is not None else _parse_tool_result(raw)
        if _is_unstructured_raw_result(parsed):
            return self._result(
                False,
                {},
                error_code=error_code,
                message="C++ binding returned invalid result",
                method=method,
            )
        status_text = str(parsed.get("status") or parsed.get("status_info") or "").strip().lower()
        type_text = str(parsed.get("type") or parsed.get("event_type") or "").strip().lower()
        success_value = parsed.get("success")
        explicit_failure = (
            (isinstance(success_value, bool) and not success_value)
            or (
                isinstance(success_value, (int, float))
                and not isinstance(success_value, bool)
                and float(success_value) == 0.0
            )
            or (
                isinstance(success_value, str)
                and success_value.strip().lower() in {"0", "false", "no", "off", "failed", "failure", "error"}
            )
        )
        native_error_code = parsed.get("error_code")
        native_error_text = str(native_error_code or "").strip()
        has_native_error_code = bool(native_error_text) and native_error_text.lower() not in {
            "0",
            "ok",
            "success",
        }
        if (
            explicit_failure
            or status_text in {"error", "failed", "failure", "fail"}
            or type_text in {"error", "failed", "failure", "fail"}
            or parsed.get("error")
            or (
                has_native_error_code and status_text not in {"ok", "success"}
            )
        ):
            message = _safe_cpp_error_message(
                parsed.get("message")
                or parsed.get("error")
                or parsed.get("status_info")
                or native_error_text
            )
            if isinstance(native_error_code, str):
                normalized_error_code = native_error_code.strip()
                if normalized_error_code and not normalized_error_code.isdigit():
                    error_code = normalized_error_code
            return self._result(
                False,
                {"status": "error", "error": message, "error_code": error_code},
                error_code=error_code,
                message=message,
                method=method,
            )
        return self._result(True, _safe_cpp_success_payload(parsed), method=method)

    @staticmethod
    def _result(
        success: bool,
        payload: dict[str, Any],
        *,
        error_code: str = "",
        message: str = "",
        method: str,
    ) -> RuntimeCppBridgeResult:
        safe_method = _safe_component_token(method, fallback="engine_call")
        safe_error = _safe_component_token(error_code, fallback="", allow_empty=True)
        fact = {
            "bridge_call_count": 1,
            "bridge_success_count": 1 if success else 0,
            "bridge_failed_count": 0 if success else 1,
            "bridge_method_counts": {safe_method: 1},
            "bridge_error_code_counts": {safe_error: 1} if safe_error and not success else {},
        }
        return RuntimeCppBridgeResult(
            success,
            dict(payload or {}),
            error_code=safe_error,
            message=_safe_cpp_error_message(message) if message else "",
            boundary_fact=fact,
        )


def make_engine_gameplay_manifest_provider(
    *,
    engine_gate: Any,
    gameplay_apply_tool: Any,
    capability_manifest_reader: Callable[[], Any],
    parse_result: Callable[[Any], dict[str, Any]] | None = None,
) -> Callable[[dict[str, Any]], dict[str, Any]]:
    """Route a validated GameplayManifest through EngineWriteGate only."""

    if engine_gate is None:
        raise ValueError("engine_gate is required")
    if gameplay_apply_tool is None:
        raise ValueError("gameplay_apply_tool is required")
    if not callable(capability_manifest_reader):
        raise ValueError("capability_manifest_reader is required")
    bridge = RuntimeCppBridge(engine_gate=engine_gate, parse_result=parse_result)

    def provider(request: dict[str, Any]) -> dict[str, Any]:
        try:
            manifest = validate_gameplay_manifest_payload(request.get("manifest"))
        except (TypeError, ValueError):
            return {
                "success": False,
                "error_code": "gameplay_manifest_payload_invalid",
                "message": "Gameplay manifest payload is invalid.",
            }
        if (
            str(request.get("manifest_schema_version") or "") != PLAN_PATCH_PAYLOAD_SCHEMA_VERSION
            or str(request.get("manifest_hash") or "") != str(manifest.get("content_hash") or "")
            or str(request.get("plan_id") or "") != str(manifest.get("plan_id") or "")
        ):
            return {
                "success": False,
                "error_code": "gameplay_manifest_envelope_mismatch",
                "message": "Gameplay manifest envelope does not match the validated payload.",
            }
        try:
            raw_capabilities = capability_manifest_reader()
        except Exception:  # noqa: BLE001
            return {
                "success": False,
                "error_code": "engine_capability_manifest_unavailable",
                "message": "Engine capability manifest is unavailable.",
            }
        if isinstance(raw_capabilities, dict):
            capabilities = dict(raw_capabilities)
        elif callable(getattr(raw_capabilities, "as_dict", None)):
            capabilities = dict(raw_capabilities.as_dict())
        else:
            capabilities = {
                "supported_operations": getattr(raw_capabilities, "supported_operations", ()),
                "supported_gameplay_primitives": getattr(
                    raw_capabilities,
                    "supported_gameplay_primitives",
                    (),
                ),
            }
        operations = {str(item or "").strip() for item in capabilities.get("supported_operations") or ()}
        supported_primitives = {
            str(item or "").strip()
            for item in capabilities.get("supported_gameplay_primitives") or ()
        }
        required_primitives = {
            str(item.get("kind") or "").strip()
            for item in manifest.get("primitives") or ()
            if isinstance(item, dict)
        }
        if "gameplay.apply_manifest" not in operations:
            return {
                "success": False,
                "error_code": "engine_gameplay_operation_unsupported",
                "message": "Engine does not advertise gameplay.apply_manifest.",
            }
        if not required_primitives.issubset(supported_primitives):
            return {
                "success": False,
                "error_code": "engine_gameplay_primitives_unsupported",
                "message": "Engine does not advertise every required gameplay primitive.",
            }
        bridge_result = bridge.invoke_tool(
            gameplay_apply_tool,
            {
                "operation": "gameplay.apply_manifest",
                "patch_type": "gameplay_manifest_apply",
                "room_id": str(request.get("room_id") or ""),
                "plan_id": str(request.get("plan_id") or ""),
                "proposal_id": str(request.get("proposal_id") or ""),
                "payload_schema_version": str(request.get("manifest_schema_version") or ""),
                "structured_payload": manifest,
                "payload_hash": str(request.get("manifest_hash") or ""),
                "idempotency_key": str(request.get("idempotency_key") or ""),
            },
            error_code="engine_gameplay_manifest_apply_failed",
        )
        return {
            "success": bridge_result.success,
            "payload": dict(bridge_result.payload or {}),
            "error_code": bridge_result.error_code,
            "message": bridge_result.message,
            "boundary_fact": dict(bridge_result.boundary_fact or {}),
        }

    provider.__name__ = "engine_gameplay_manifest_provider"
    return provider


class EngineCapabilityManifestReadError(RuntimeError):
    """A sanitized failure from the read-only Engine capability boundary."""

    def __init__(self, error_code: str, message: str) -> None:
        super().__init__(message)
        self.error_code = str(error_code or "engine_capability_manifest_read_failed")


class ImageResourceProviderError(RuntimeError):
    def __init__(self, error_code: str, message: str) -> None:
        super().__init__(message)
        self.error_code = str(error_code or "image_resource_resolve_failed")


def make_image_resource_provider(
    *,
    image_tool: Any,
    parse_result: Callable[[Any], dict[str, Any]] | None = None,
    resolution: str = "1:1",
    media_resolver: Callable[..., Any] | None = None,
    resolve_timeout: float = 120.0,
) -> ResourceProvider:
    """Create a Runtime image-resource provider from a function-sized image tool.

    The adapter normalizes reference-image facts for RuntimeState.  It does not
    start a model workflow, import actors, or mutate engine
    state.  The injected image_tool may expose ``invoke(payload)`` or be a plain
    callable that accepts the same payload.
    """

    if image_tool is None:
        raise ValueError("image_tool is required")

    def _provider(payload: dict[str, Any]) -> dict[str, dict[str, Any]]:
        batch_id = str(payload.get("batch_id") or "")
        model_items = [str(item) for item in (payload.get("model_items") or []) if str(item or "")]
        resources: dict[str, dict[str, Any]] = {}
        for index, name in enumerate(model_items, start=1):
            prompt = str(_item_value(payload, name, "image_prompt") or _image_prompt_for_item(name))
            tool_payload = {
                "prompt": prompt,
                "object_name": name,
                "object_id": f"{batch_id}-img-{index:02d}" if batch_id else f"runtime-img-{index:02d}",
                "resolution": resolution,
            }
            try:
                raw = _invoke_image_tool(image_tool, tool_payload)
            except Exception as exc:  # noqa: BLE001
                resources[name] = _failed_image_resource(
                    name=name,
                    batch_id=batch_id,
                    index=index,
                    failure_code="image_tool_call_failed",
                    failure_message=str(exc) or "image tool call failed",
                )
                continue
            try:
                parsed = parse_result(raw) if parse_result is not None else _parse_tool_result(raw)
                resources[name] = _normalize_image_result(
                    parsed,
                    name=name,
                    batch_id=batch_id,
                    index=index,
                    prompt=prompt,
                    media_resolver=media_resolver,
                    resolve_timeout=resolve_timeout,
                )
            except ImageResourceProviderError as exc:
                resources[name] = _failed_image_resource(
                    name=name,
                    batch_id=batch_id,
                    index=index,
                    failure_code=exc.error_code,
                    failure_message=str(exc),
                )
            except Exception as exc:  # noqa: BLE001
                resources[name] = _failed_image_resource(
                    name=name,
                    batch_id=batch_id,
                    index=index,
                    failure_code="image_resource_resolve_failed",
                    failure_message=str(exc) or "image resource resolve failed",
                )
        return resources

    return _provider


def make_model_resource_provider(
    *,
    model_tool: Any,
    parse_result: Callable[[Any], dict[str, Any]] | None = None,
    max_concurrency: int = 3,
    wait_for_ready: Callable[[str], Any] | None = None,
    require_image_input: bool = False,
) -> ResourceProvider:
    """Create a Runtime model-resource provider from a function-sized model tool.

    The adapter prepares model resource facts only.  It does not call
    legacy orchestration or import actors.
    The injected model_tool may expose ``invoke(payload)`` or be a plain
    callable accepting the same payload.
    """

    if model_tool is None:
        raise ValueError("model_tool is required")

    worker_limit = max(1, min(4, int(max_concurrency or 1)))

    def _provider(payload: dict[str, Any]) -> dict[str, dict[str, Any]]:
        batch_id = str(payload.get("batch_id") or "")
        model_items = [str(item) for item in (payload.get("model_items") or []) if str(item or "")]

        def _prepare_one(index: int, name: str) -> tuple[str, dict[str, Any]]:
            prompt_text = str(_item_value(payload, name, "prompt_text") or name)
            image_resource = _image_resource_entry(payload, name)
            image_url = str(_item_value(payload, name, "image_url") or _image_resource_value(payload, name) or "")
            source_image_ref = str(image_resource.get("resource_ref") or "")
            source_image_hash = str(image_resource.get("content_hash") or "")
            if require_image_input and (
                not image_url
                or not source_image_ref
                or not source_image_hash.startswith("sha256:")
            ):
                return name, _failed_model_resource(
                    name=name,
                    batch_id=batch_id,
                    index=index,
                    source="model_resource",
                    failure_code="source_image_lineage_missing",
                )
            tool_payload = {
                # Hunyuan3D's narrow tool schema is mode/images/prompt.  Keep no
                # provider-private object/path fields here; RuntimeState receives
                # sanitized resource facts after the tool returns.
                "mode": "image_to_3d" if image_url else "text_to_3d",
                "images": [image_url] if image_url else None,
                "prompt": prompt_text,
            }
            try:
                raw = _invoke_tool_safely(model_tool, tool_payload, fallback="model resource failed")
                parsed = parse_result(raw) if parse_result is not None else _parse_tool_result(raw)
                metadata = parsed.get("metadata") if isinstance(parsed.get("metadata"), dict) else {}
                pending = bool(metadata.get("has_mesh_pending")) or str(
                    metadata.get("mesh_download_status") or ""
                ).strip().lower() in {"scheduled", "running", "pending", "queued"}
                object_id = str(
                    metadata.get("folder_object_id")
                    or metadata.get("object_id")
                    or ""
                ).strip()
                if pending and object_id and wait_for_ready is not None:
                    wait_for_ready(object_id)
                return name, _normalize_model_tool_result(
                    parsed,
                    name=name,
                    batch_id=batch_id,
                    index=index,
                    generation_mode="image_to_3d" if image_url else "text_to_3d",
                    source_image_ref=source_image_ref,
                    source_image_hash=source_image_hash,
                )
            except Exception:  # noqa: BLE001
                return name, _failed_model_resource(
                    name=name,
                    batch_id=batch_id,
                    index=index,
                    source="model_resource",
                    failure_code="model_resource_tool_failed",
                )

        if len(model_items) <= 1 or worker_limit <= 1:
            prepared = [_prepare_one(index, name) for index, name in enumerate(model_items, start=1)]
        else:
            prepared = []
            with ThreadPoolExecutor(
                max_workers=min(worker_limit, len(model_items)),
                thread_name_prefix="AgentRuntimeModelBatch",
            ) as executor:
                futures = {
                    executor.submit(_prepare_one, index, name): index
                    for index, name in enumerate(model_items, start=1)
                }
                indexed_results: dict[int, tuple[str, dict[str, Any]]] = {}
                for future in as_completed(futures):
                    indexed_results[futures[future]] = future.result()
                prepared = [indexed_results[index] for index in sorted(indexed_results)]
        return {name: resource for name, resource in prepared}

    return _provider


def make_legacy_model_resource_provider(
    model_provider_factory: Callable[[], Any] | None = None,
) -> ResourceProvider:
    """Compatibility provider retained for isolated migration tests only.

    It is not registered by the production composition root; production model
    generation uses the function-sized provider above.
    """
    provider_instance: Any | None = None

    def _provider(payload: dict[str, Any]) -> dict[str, dict[str, Any]]:
        nonlocal provider_instance
        batch_id = str(payload.get("batch_id") or "")
        model_items = [str(item) for item in (payload.get("model_items") or []) if str(item or "")]
        resources: dict[str, dict[str, Any]] = {}
        if provider_instance is None:
            try:
                provider_instance = _create_model_provider(model_provider_factory)
            except Exception:  # noqa: BLE001
                return {
                    name: _failed_model_resource(
                        name=name, batch_id=batch_id, index=index,
                        source="legacy_model_adapter_unavailable",
                        failure_code="legacy_model_adapter_unavailable",
                    )
                    for index, name in enumerate(model_items, start=1)
                }
        for index, name in enumerate(model_items, start=1):
            try:
                result = provider_instance.acquire(
                    name,
                    image_url=str(_item_value(payload, name, "image_url") or _image_resource_value(payload, name) or ""),
                    prompt_text=str(_item_value(payload, name, "prompt_text") or name),
                    object_id=f"{batch_id}-{index:02d}" if batch_id else f"runtime-{index:02d}",
                )
                resources[name] = _normalize_acquire_result(result, name=name, batch_id=batch_id, index=index)
            except Exception:  # noqa: BLE001
                resources[name] = _failed_model_resource(
                    name=name, batch_id=batch_id, index=index,
                    failure_code="legacy_model_acquire_exception",
                )
        return resources

    return _provider


def make_scene_snapshot_provider(
    *,
    snapshot_tool: Any,
    scene_name: str = "",
    wait_for_bounds: bool = True,
    parse_result: Callable[[Any], dict[str, Any]] | None = None,
) -> Callable[[str], dict[str, Any]]:
    """Create a Runtime scene-snapshot provider from a function-sized scene tool.

    The provider only reads native scene facts and normalizes them for
    RuntimeState.  It does not import actors, mutate transforms, or call any
    legacy compose/progressive workflow path.
    """

    if snapshot_tool is None:
        raise ValueError("snapshot_tool is required")

    def _provider(request: Any) -> dict[str, Any]:
        if isinstance(request, dict):
            room_id = str(request.get("room_id") or "")
            effective_scene_name = str(request.get("scene_name") or scene_name or "")
            native_scene_route = str(
                request.get("scene_route")
                or request.get("native_scene_route")
                or scene_name
                or ""
            )
        else:
            room_id = str(request or "")
            effective_scene_name = scene_name
            native_scene_route = scene_name
        payload = {
            # Runtime scene_name is a semantic plan label. Only an explicitly
            # configured/native route may select a C++ scene; forwarding the
            # semantic label caused the native reader to reload the live scene
            # on every readiness poll.
            "scene_name": native_scene_route,
            "wait_for_bounds": bool(wait_for_bounds),
        }
        raw = _invoke_tool_safely(snapshot_tool, payload, fallback="scene snapshot failed")
        parsed = parse_result(raw) if parse_result is not None else _parse_tool_result(raw)
        return _normalize_scene_snapshot_result(
            parsed,
            room_id=str(room_id or ""),
            scene_name=effective_scene_name,
        )

    return _provider


def make_current_unversioned_v1_scene_snapshot_reader(
    *,
    snapshot_tool: Any,
    build_fingerprint: str,
    scene_name: str = "",
    wait_for_bounds: bool = True,
    parse_result: Callable[[Any], dict[str, Any]] | None = None,
) -> Callable[[Any], dict[str, Any]]:
    """Read one current-build native Snapshot through the strict input contract."""

    if snapshot_tool is None:
        raise ValueError("snapshot_tool is required")

    def _reader(request: Any) -> dict[str, Any]:
        if isinstance(request, dict):
            room_id = str(request.get("room_id") or "")
            effective_scene_name = str(request.get("scene_name") or scene_name or "")
            native_scene_route = str(
                request.get("scene_route")
                or request.get("native_scene_route")
                or scene_name
                or ""
            )
        else:
            room_id = str(request or "")
            effective_scene_name = scene_name
            native_scene_route = scene_name
        payload = {
            "scene_name": native_scene_route,
            "wait_for_bounds": bool(wait_for_bounds),
        }
        try:
            raw = _invoke_tool(snapshot_tool, payload)
        except Exception as exc:  # noqa: BLE001
            raise EngineSnapshotInputContractError(
                "engine_snapshot_read_failed",
                "Engine scene snapshot read failed.",
            ) from exc
        parsed = parse_result(raw) if parse_result is not None else _parse_tool_result(raw)
        return normalize_current_unversioned_v1_scene_snapshot(
            parsed,
            room_id=str(room_id or "default"),
            scene_name=effective_scene_name,
            build_fingerprint=build_fingerprint,
        )

    return _reader


def make_engine_capability_manifest_reader(
    *,
    capability_tool: Any,
    parse_result: Callable[[Any], dict[str, Any]] | None = None,
) -> Callable[[], dict[str, Any]]:
    """Create a read-only adapter for the Engine capability manifest.

    The manifest tool is intentionally separate from ``RuntimeCppBridge``:
    capability discovery must not pass through an Engine write gate.  The
    returned callable preserves a small, sanitized failure vocabulary for the
    collaboration boundary while leaving native field normalization to that
    boundary's injected port implementation.
    """

    if capability_tool is None:
        raise ValueError("capability_tool is required")

    def _reader() -> dict[str, Any]:
        try:
            raw = _invoke_tool(capability_tool, {})
        except ConnectionError as exc:
            raise EngineCapabilityManifestReadError(
                "bridge_not_connected",
                "Engine capability bridge is not connected.",
            ) from exc
        except Exception as exc:  # noqa: BLE001
            raise EngineCapabilityManifestReadError(
                "engine_capability_manifest_read_failed",
                "Engine capability manifest read failed.",
            ) from exc
        parsed = parse_result(raw) if parse_result is not None else _parse_tool_result(raw)
        if _is_unstructured_raw_result(parsed):
            raise EngineCapabilityManifestReadError(
                "engine_capability_manifest_invalid",
                "Engine capability manifest response is not structured.",
            )
        status = str(parsed.get("status") or "").strip().lower()
        success = _coerce_adapter_bool(parsed.get("success"), default=True)
        if status in {"error", "failed", "failure", "fail"} or not success or parsed.get("error"):
            native_code = str(parsed.get("error_code") or "").strip().lower()
            error_code = (
                "bridge_not_connected"
                if native_code in {"bridge_not_connected", "engine_not_connected", "missing_engine"}
                else "engine_capability_manifest_unavailable"
            )
            raise EngineCapabilityManifestReadError(
                error_code,
                "Engine capability manifest is unavailable.",
            )
        return dict(parsed)

    return _reader


def make_scene_review_provider(
    *,
    review_tool: Any,
    output_dir_provider: Callable[[dict[str, Any]], str] | None = None,
    max_images: int = 12,
    parse_result: Callable[[Any], dict[str, Any]] | None = None,
) -> ResourceProvider:
    """Create a Runtime review provider from a function-sized scene review tool.

    The provider produces advisory review facts only.  If no screenshot directory
    is available it returns a skipped review instead of blocking generation or
    pretending VLM succeeded.
    """

    if review_tool is None:
        raise ValueError("review_tool is required")

    def _provider(payload: dict[str, Any]) -> dict[str, Any]:
        output_dir = str(output_dir_provider(payload) if output_dir_provider else payload.get("review_output_dir") or "")
        if not output_dir:
            return {
                "plan_id": str(payload.get("plan_id") or ""),
                "batch_id": str(payload.get("batch_id") or ""),
                "contract_version": int(payload.get("contract_version") or 0),
                "checkpoint_type": str(payload.get("checkpoint_type") or "geometry_review"),
                "reviewed_targets": [
                    str(item or "").strip()
                    for item in (payload.get("reviewed_targets") or [])
                    if str(item or "").strip()
                ],
                "status": "skipped",
                "overall": "SKIPPED",
                "score": -1,
                "issues": [],
                "advisory_items": [
                    {"type": "review_skipped", "reason": "missing screenshot directory"},
                ],
                "source": "scene_review_provider",
            }
        tool_payload = {
            "output_dir": output_dir,
            "scene_description": str(payload.get("scene_description") or payload.get("scene_name") or ""),
            "max_images": int(max_images),
        }
        raw = _invoke_tool_safely(review_tool, tool_payload, fallback="scene review failed")
        parsed = parse_result(raw) if parse_result is not None else _parse_tool_result(raw)
        return _normalize_scene_review_result(parsed, payload=payload)

    return _provider


def make_environment_component_provider(
    *,
    environment_tool: Any,
    parse_result: Callable[[Any], dict[str, Any]] | None = None,
) -> ResourceProvider:
    """Create a Runtime environment-component provider from a function tool.

    The adapter returns terrain / skybox / boundary component facts for
    RuntimeState only.  Real engine writes must be implemented as dedicated
    engine providers through RuntimeCppBridge / EngineWriteGate, not hidden
    behind this fact provider.
    """

    if environment_tool is None:
        raise ValueError("environment_tool is required")

    def _provider(payload: dict[str, Any]) -> dict[str, dict[str, Any]]:
        batch_id = str(payload.get("batch_id") or "")
        identity_scope = str(payload.get("plan_id") or batch_id or "runtime")
        components: dict[str, dict[str, Any]] = {}
        for index, item in enumerate(payload.get("substrate_resolutions") or [], start=1):
            if not isinstance(item, dict):
                continue
            name = str(item.get("name") or "").strip()
            if not name:
                continue
            component_type = str(item.get("component_type") or "environment").strip() or "environment"
            tool_payload = {
                "room_id": str(payload.get("room_id") or ""),
                "plan_id": str(payload.get("plan_id") or ""),
                "batch_id": batch_id,
                "scene_name": str(payload.get("scene_name") or ""),
                "name": name,
                "component_type": component_type,
                "handler": str(item.get("handler") or ""),
                "object_id": f"{identity_scope}-env-{index:02d}",
                "requires_engine_write": _coerce_adapter_bool(item.get("requires_engine_write"), default=False),
            }
            raw = _invoke_tool_safely(environment_tool, tool_payload, fallback="environment component failed")
            parsed = parse_result(raw) if parse_result is not None else _parse_tool_result(raw)
            component = _normalize_environment_component_result(
                parsed,
                fallback=tool_payload,
                index=index,
            )
            if component.get("requires_engine_write"):
                raise RuntimeError("environment component requires dedicated engine bridge")
            components[component["component_id"]] = component
        return components

    return _provider


def make_engine_environment_component_import_provider(
    *,
    environment_import_tool: Any,
    engine_gate: Any,
    scene_name: str = "",
    scene_snapshot_provider: Callable[[Any], dict[str, Any]] | None = None,
    parse_result: Callable[[Any], dict[str, Any]] | None = None,
) -> ResourceProvider:
    """Create a Runtime environment-component import provider.

    This is the dedicated engine-write bridge for future terrain / boundary /
    room-framework actor creation.  It is intentionally separate from
    make_environment_component_provider(), which only produces Runtime facts.
    """

    if environment_import_tool is None:
        raise ValueError("environment_import_tool is required")
    if engine_gate is None:
        raise ValueError("engine_gate is required")
    bridge = RuntimeCppBridge(engine_gate=engine_gate, parse_result=parse_result)
    imported_component_cache: dict[tuple[str, str], dict[str, Any]] = {}
    imported_component_cache_lock = threading.Lock()

    def _provider(payload: dict[str, Any]) -> dict[str, Any]:
        batch_id = str(payload.get("batch_id") or "")
        plan_id = str(payload.get("plan_id") or "")
        effective_scene_name = str(payload.get("scene_name") or scene_name or "")
        requested = _environment_components_from_payload(payload)
        component_updates: dict[str, dict[str, Any]] = {}
        import_results: list[dict[str, Any]] = []
        bridge_results: list[RuntimeCppBridgeResult] = []
        for index, component in enumerate(requested, start=1):
            component_id = _safe_component_token(
                component.get("component_id"),
                fallback=(f"{batch_id}-env-import-{index:02d}" if batch_id else f"runtime-env-import-{index:02d}"),
            )
            name = _safe_component_text(component.get("name"), fallback=component_id)
            component_type = _safe_component_token(
                component.get("component_type"),
                fallback="environment",
            )
            cache_key = (plan_id, component_id)
            with imported_component_cache_lock:
                cached_component = dict(imported_component_cache.get(cache_key) or {})
            if cached_component and str(cached_component.get("actor_id") or "").strip():
                component_updates[component_id] = cached_component
                import_results.append({
                    "component_id": component_id,
                    "name": str(cached_component.get("name") or name),
                    "component_type": str(cached_component.get("component_type") or component_type),
                    "status": "reused",
                    "actor_id": str(cached_component.get("actor_id") or ""),
                    "entity_id": str(cached_component.get("entity_id") or ""),
                    "entity_version": int(cached_component.get("entity_version") or 1),
                    "asset_id": str(cached_component.get("asset_id") or component_id),
                    "model_ref": str(cached_component.get("model_ref") or component_id),
                    "bounds_ready": bool(cached_component.get("bounds_ready")),
                    "bounds_source": str(cached_component.get("bounds_source") or "estimated"),
                    "engine_lifecycle_status": str(
                        cached_component.get("engine_lifecycle_status") or "engine_accepted"
                    ),
                })
                continue
            import_payload = {
                "component_id": component_id,
                "name": name,
                "component_type": component_type,
                "entity_type": "environment",
                "semantic_role": (
                    "indoor_enclosure"
                    if component_type == "room_box"
                    else "walkable_floor"
                    if component_type == "room_floor"
                    else "environment_component"
                ),
                "handler": _safe_component_token(component.get("handler"), fallback="", allow_empty=True),
                "object_id": component_id,
                "scene_name": str(component.get("scene_name") or effective_scene_name),
            }
            asset_id = _safe_component_token(
                _first_present(component.get("asset_id"), component_id),
                fallback=component_id,
            )
            model_ref = _safe_component_text(
                _first_present(component.get("model_ref"), component.get("resource_id"), asset_id),
                fallback=asset_id,
            )
            import_payload["asset_id"] = asset_id
            import_payload["model_ref"] = model_ref
            actor_guid = _stable_runtime_actor_guid(
                plan_id=plan_id,
                # Environment components belong to the scene plan, not to the
                # business batch that happened to materialize them first.
                batch_id="__environment__",
                asset_id=asset_id,
                requested_name=component_id,
                source_index=0,
            )
            import_payload["actor_guid"] = actor_guid
            import_payload["entity_id"] = _stable_runtime_entity_id(actor_guid)
            import_payload["entity_version"] = 1
            import_payload["source_plan_id"] = plan_id
            import_payload["source_batch_id"] = batch_id
            import_payload["source_scene_version"] = max(1, int(payload.get("scene_version") or 1))
            normalized_component_type = component_type.strip().lower()
            if normalized_component_type in {"room_box", "room_shell", "indoor_enclosure"}:
                import_payload["grounding_status"] = "enclosure"
            elif normalized_component_type in {
                "room_floor",
                "terrain",
                "ground",
                "walkable_floor",
                "transition_zone",
            }:
                import_payload["grounding_status"] = "grounded"
            elif normalized_component_type in {"sky", "skybox"}:
                import_payload["grounding_status"] = "not_applicable"
            for field in ("position", "rotation", "scale"):
                vector = _vector3(component.get(field))
                if vector:
                    import_payload[field] = vector
            bounds = _normalized_bounds_from(component.get("aabb"), component.get("bounds"), component)
            if bounds:
                import_payload["aabb"] = bounds
            for field in ("surface", "terrain_profile", "sky_mode", "boundary_style"):
                value = _safe_component_text(component.get(field), fallback="", allow_empty=True)
                if value:
                    import_payload[field] = value
            if plan_id:
                import_payload["plan_id"] = plan_id
            if batch_id:
                import_payload["batch_id"] = batch_id
            bridge_result = bridge.invoke_tool(
                environment_import_tool,
                import_payload,
                error_code="cpp_environment_component_import_failed",
            )
            bridge_results.append(bridge_result)
            if not bridge_result.success:
                import_results.append({
                    "component_id": component_id,
                    "name": name,
                    "component_type": component_type,
                    "status": "failed",
                    "failure_code": "cpp_environment_component_import_failed",
                    "reason": _safe_adapter_error_message(
                        {"message": bridge_result.message},
                        fallback="environment component import failed",
                    ),
                })
                continue
            update = _normalize_environment_component_import_result(
                bridge_result.payload,
                fallback=import_payload,
            )
            if not str(update.get("actor_id") or "").strip():
                import_results.append({
                    "component_id": update["component_id"],
                    "name": update["name"],
                    "component_type": update["component_type"],
                    "status": "failed",
                    "failure_code": "environment_import_missing_actor_identity",
                    "reason": "engine environment import returned no actor identity",
                })
                continue
            component_updates[update["component_id"]] = update
            with imported_component_cache_lock:
                imported_component_cache[cache_key] = dict(update)
            result_row = {
                "component_id": update["component_id"],
                "name": update["name"],
                "component_type": update["component_type"],
                "status": "success",
            }
            for field in (
                "actor_id",
                "entity_id",
                "entity_version",
                "asset_id",
                "model_ref",
                "display_name",
                "native_name",
                "requested_name",
                "aliases",
                "sync_status",
                "sync_lifecycle_status",
                "position",
                "rotation",
                "scale",
                "aabb",
                "bounds_ready",
                "bounds_source",
                "engine_lifecycle_status",
                "render_status_observed",
                "render_ready",
                "render_failed",
                "gpu_build_state",
                "mesh_count",
                "renderable_mesh_count",
                "invalid_mesh_count",
                "entity_type",
                "semantic_role",
                "grounding_status",
                "size",
            ):
                if field in update:
                    result_row[field] = update[field]
            import_results.append(result_row)
        if scene_snapshot_provider is not None and component_updates:
            _reconcile_engine_ready_facts(
                component_updates,
                snapshot_provider=scene_snapshot_provider,
                room_id=str(payload.get("room_id") or ""),
                scene_name=effective_scene_name,
            )
            for row in import_results:
                component = component_updates.get(str(row.get("component_id") or ""))
                if not component:
                    continue
                row["bounds_ready"] = bool(component.get("bounds_ready"))
                row["bounds_source"] = str(component.get("bounds_source") or "estimated")
                row["engine_lifecycle_status"] = str(
                    component.get("engine_lifecycle_status") or "engine_loading"
                )
                for field in (
                    "render_status_observed",
                    "render_ready",
                    "render_failed",
                    "gpu_build_state",
                    "mesh_count",
                    "renderable_mesh_count",
                    "invalid_mesh_count",
                ):
                    if field in component:
                        row[field] = component.get(field)
        status_counts: dict[str, int] = {}
        for item in import_results:
            status_key = str(item.get("status") or "unknown").strip().lower() or "unknown"
            status_counts[status_key] = status_counts.get(status_key, 0) + 1
        return {
            "environment_components": component_updates,
            "environment_import_results": import_results,
            "source": "engine_environment_import_provider",
            "engine_write_result": {
                "provider_source": "engine_environment_import_provider",
                "requested_count": len(requested),
                "identity_result_count": len(component_updates),
                "missing_identity_count": sum(
                    1
                    for item in import_results
                    if str(item.get("status") or "").strip().lower() == "failed"
                    and (
                        str(item.get("failure_code") or "")
                        == "environment_import_missing_actor_identity"
                        or "actor identity" in str(item.get("reason") or "").lower()
                    )
                ),
                "status_counts": status_counts,
                **_merge_bridge_boundary_facts(bridge_results),
            },
        }

    return _provider


def _normalize_environment_component_result(
    parsed: dict[str, Any],
    *,
    fallback: dict[str, Any],
    index: int,
) -> dict[str, Any]:
    if str(parsed.get("status") or "").lower() == "error" or parsed.get("error"):
        raise RuntimeError(_safe_adapter_error_message(parsed, fallback="environment component failed"))
    fallback_component_id = str(fallback.get("object_id") or f"runtime-env-{index:02d}")
    component_id = _safe_component_token(
        _first_present(parsed.get("component_id"), parsed.get("actor_id"), parsed.get("object_id")),
        fallback=fallback_component_id,
    )
    component_type = _safe_component_token(
        _first_present(
            fallback.get("component_type"),
            parsed.get("component_type"),
            parsed.get("type"),
            "environment",
        ),
        fallback=str(fallback.get("component_type") or "environment"),
    )
    native_name = _safe_component_text(
        _first_present(parsed.get("name"), parsed.get("actor_name"), fallback.get("name")),
        fallback=str(fallback.get("name") or component_id),
    )
    requested_name = _safe_component_text(
        fallback.get("name"),
        fallback=str(fallback.get("name") or native_name or component_id),
    )
    name = native_name
    handler = _safe_component_token(
        _first_present(fallback.get("handler"), parsed.get("handler")),
        fallback=str(fallback.get("handler") or ""),
        allow_empty=True,
    )
    status = _safe_component_token(parsed.get("status"), fallback="created")
    scene_name = _safe_component_text(
        _first_present(fallback.get("scene_name"), parsed.get("scene_name")),
        fallback=str(fallback.get("scene_name") or ""),
        allow_empty=True,
    )
    result = {
        "component_id": component_id,
        "name": name,
        "display_name": name,
        "native_name": native_name,
        "requested_name": requested_name,
        "component_type": component_type,
        "handler": handler,
        "status": status,
        "source": "environment_component",
        "scene_name": scene_name,
        "requires_engine_write": _coerce_adapter_bool(
            parsed.get("requires_engine_write") if "requires_engine_write" in parsed else fallback.get("requires_engine_write"),
            default=False,
        ),
    }
    aliases: list[str] = []
    for alias in (requested_name, native_name, name, component_id):
        safe_alias = _safe_component_text(alias, fallback="", allow_empty=True)
        if safe_alias and safe_alias not in aliases:
            aliases.append(safe_alias)
    if aliases:
        result["aliases"] = aliases[:8]
    return result


def _environment_components_from_payload(payload: dict[str, Any]) -> list[dict[str, Any]]:
    raw = payload.get("environment_components")
    if isinstance(raw, dict):
        return [dict(value) for value in raw.values() if isinstance(value, dict)]
    if isinstance(raw, list):
        return [dict(value) for value in raw if isinstance(value, dict)]
    raw = payload.get("substrate_resolutions")
    if isinstance(raw, list):
        return [dict(value) for value in raw if isinstance(value, dict)]
    return []


def _merge_bridge_boundary_facts(results: list[RuntimeCppBridgeResult]) -> dict[str, Any]:
    call_count = 0
    success_count = 0
    failed_count = 0
    method_counts: dict[str, int] = {}
    error_code_counts: dict[str, int] = {}
    for result in results:
        fact = result.boundary_fact if isinstance(result.boundary_fact, dict) else {}
        call_count += int(fact.get("bridge_call_count") or 0)
        success_count += int(fact.get("bridge_success_count") or 0)
        failed_count += int(fact.get("bridge_failed_count") or 0)
        for key, value in dict(fact.get("bridge_method_counts") or {}).items():
            safe_key = _safe_component_token(key, fallback="", allow_empty=True)
            if safe_key:
                method_counts[safe_key] = method_counts.get(safe_key, 0) + int(value or 0)
        for key, value in dict(fact.get("bridge_error_code_counts") or {}).items():
            safe_key = _safe_component_token(key, fallback="", allow_empty=True)
            if safe_key:
                error_code_counts[safe_key] = error_code_counts.get(safe_key, 0) + int(value or 0)
    return {
        "bridge_call_count": call_count,
        "bridge_success_count": success_count,
        "bridge_failed_count": failed_count,
        "bridge_method_counts": dict(sorted(method_counts.items())),
        "bridge_error_code_counts": dict(sorted(error_code_counts.items())),
    }


def _normalize_environment_component_import_result(
    parsed: dict[str, Any],
    *,
    fallback: dict[str, Any],
) -> dict[str, Any]:
    actor = parsed.get("actor") if isinstance(parsed.get("actor"), dict) else {}
    actor_data = parsed.get("actor_data") if isinstance(parsed.get("actor_data"), dict) else {}
    geometry = actor.get("geometry") if isinstance(actor.get("geometry"), dict) else {}
    actor_data_geometry = actor_data.get("geometry") if isinstance(actor_data.get("geometry"), dict) else {}
    component_id = _safe_component_token(
        _first_present(
            parsed.get("component_id"),
            actor_data.get("component_id"),
            actor.get("component_id") if isinstance(actor, dict) else None,
            parsed.get("object_id"),
            fallback.get("component_id"),
        ),
        fallback=str(fallback.get("component_id") or fallback.get("object_id") or "runtime-env-import"),
    )
    native_name = _safe_component_text(
        _first_present(
            parsed.get("name"),
            actor_data.get("name"),
            actor.get("name") if isinstance(actor, dict) else None,
            parsed.get("actor_name"),
            fallback.get("name"),
        ),
        fallback=component_id,
    )
    requested_name = _safe_component_text(
        fallback.get("name"),
        fallback=str(fallback.get("name") or native_name or component_id),
    )
    name = native_name
    component_type = _safe_component_token(
        _first_present(
            fallback.get("component_type"),
            parsed.get("component_type"),
            actor_data.get("component_type"),
            actor.get("component_type") if isinstance(actor, dict) else None,
            parsed.get("type"),
        ),
        fallback=str(fallback.get("component_type") or "environment"),
    )
    handler = _safe_component_token(
        _first_present(fallback.get("handler"), parsed.get("handler"), actor_data.get("handler")),
        fallback="",
        allow_empty=True,
    )
    scene_name = _safe_component_text(
        _first_present(fallback.get("scene_name"), parsed.get("scene_name")),
        fallback="",
        allow_empty=True,
    )
    actor_id = _safe_component_token(
        _first_present(
            parsed.get("actor_id"),
            parsed.get("actor_guid"),
            parsed.get("guid"),
            actor_data.get("actor_id"),
            actor_data.get("actor_guid"),
            actor_data.get("guid"),
            actor.get("actor_id") if isinstance(actor, dict) else None,
            actor.get("actor_guid") if isinstance(actor, dict) else None,
            actor.get("guid") if isinstance(actor, dict) else None,
        ),
        fallback="",
        allow_empty=True,
    )
    asset_id = _safe_component_token(
        _first_present(
            parsed.get("asset_id"),
            actor_data.get("asset_id"),
            actor.get("asset_id") if isinstance(actor, dict) else None,
            fallback.get("asset_id"),
            component_id,
        ),
        fallback=component_id,
    )
    model_ref = _safe_component_text(
        _first_present(
            parsed.get("model_ref"),
            parsed.get("model_id"),
            parsed.get("resource_id"),
            actor_data.get("model_ref"),
            actor_data.get("model_id"),
            actor_data.get("resource_id"),
            actor.get("model_ref") if isinstance(actor, dict) else None,
            actor.get("model_id") if isinstance(actor, dict) else None,
            actor.get("resource_id") if isinstance(actor, dict) else None,
            fallback.get("model_ref"),
            fallback.get("asset_id"),
            asset_id,
        ),
        fallback=asset_id,
    )
    sync_status = _safe_component_token(
        _first_present(
            parsed.get("sync_status"),
            actor_data.get("sync_status"),
            actor.get("sync_status") if isinstance(actor, dict) else None,
            parsed.get("last_sync_status"),
            actor_data.get("last_sync_status"),
            "engine_imported",
        ),
        fallback="engine_imported",
    )
    sync_lifecycle_status = _safe_component_token(
        _first_present(
            parsed.get("sync_lifecycle_status"),
            actor_data.get("sync_lifecycle_status"),
            actor.get("sync_lifecycle_status") if isinstance(actor, dict) else None,
            parsed.get("last_sync_event"),
            actor_data.get("last_sync_event"),
            sync_status,
        ),
        fallback=sync_status,
    )
    update = {
        "component_id": component_id,
        "entity_id": _safe_component_token(
            _first_present(
                parsed.get("entity_id"),
                actor_data.get("entity_id"),
                actor.get("entity_id") if isinstance(actor, dict) else None,
                fallback.get("entity_id"),
            ),
            fallback="",
            allow_empty=True,
        ),
        "asset_id": asset_id,
        "model_ref": model_ref,
        "name": name,
        "display_name": name,
        "native_name": native_name,
        "requested_name": requested_name,
        "component_type": component_type,
        "handler": handler,
        "status": "imported",
        "source": "engine_environment_import",
        "scene_name": scene_name,
        "requires_engine_write": False,
        "sync_status": sync_status,
        "sync_lifecycle_status": sync_lifecycle_status,
    }
    version_value = _first_present(
        actor.get("actor_version") if isinstance(actor, dict) else None,
        actor.get("version") if isinstance(actor, dict) else None,
        actor_data.get("actor_version"),
        actor_data.get("version"),
        parsed.get("actor_version"),
        parsed.get("version"),
        fallback.get("entity_version"),
        1,
    )
    try:
        update["entity_version"] = max(1, int(version_value or 1))
    except (TypeError, ValueError):
        update["entity_version"] = 1
    if actor_id:
        update["actor_id"] = actor_id
    aliases: list[str] = []
    for alias in (
        requested_name,
        native_name,
        name,
        component_id,
        actor_id,
        asset_id,
    ):
        safe_alias = _safe_component_text(alias, fallback="", allow_empty=True)
        if safe_alias and safe_alias not in aliases:
            aliases.append(safe_alias)
    if aliases:
        update["aliases"] = aliases[:8]
    position = _first_present(
        geometry.get("position"),
        actor_data_geometry.get("position"),
        actor_data.get("position"),
        parsed.get("position"),
        fallback.get("position"),
    )
    rotation = _first_present(
        geometry.get("rotation"),
        actor_data_geometry.get("rotation"),
        actor_data.get("rotation"),
        parsed.get("rotation"),
        fallback.get("rotation"),
    )
    scale = _first_present(
        geometry.get("scale"),
        actor_data_geometry.get("scale"),
        actor_data.get("scale"),
        parsed.get("scale"),
        fallback.get("scale"),
    )
    if position is not None:
        vector = _vector3(position)
        if vector:
            update["position"] = vector
    if rotation is not None:
        vector = _vector3(rotation)
        if vector:
            update["rotation"] = vector
    if scale is not None:
        vector = _vector3(scale)
        if vector:
            update["scale"] = vector
    actual_bounds = _normalized_bounds_from(geometry, actor_data_geometry, actor, actor_data, parsed)
    bounds = actual_bounds or _normalized_bounds_from(fallback)
    if bounds:
        update["aabb"] = bounds
    bounds_ready_value = _first_present(
        actor.get("bounds_ready") if isinstance(actor, dict) else None,
        actor_data.get("bounds_ready") if isinstance(actor_data, dict) else None,
        parsed.get("bounds_ready"),
        fallback.get("bounds_ready"),
    )
    if bounds_ready_value is not None:
        update["bounds_ready"] = _coerce_adapter_bool(bounds_ready_value, default=False)
    else:
        update["bounds_ready"] = bool(actual_bounds)
    bounds_ready = bool(update.get("bounds_ready"))
    update["bounds_source"] = "engine_actual" if bounds_ready else "estimated"
    update["engine_lifecycle_status"] = "bounds_ready" if bounds_ready else "engine_loading"
    update["status"] = "ready" if bounds_ready else "engine_loading"
    for field in ("render_status_observed", "render_ready", "render_failed"):
        value = next(
            (
                candidate
                for candidate in (
                    actor.get(field) if isinstance(actor, dict) else None,
                    actor_data.get(field) if isinstance(actor_data, dict) else None,
                    parsed.get(field),
                    fallback.get(field),
                )
                if candidate is not None
            ),
            None,
        )
        if value is not None:
            update[field] = _coerce_adapter_bool(value, default=False)
    gpu_build_state = str(_first_present(
        actor.get("gpu_build_state") if isinstance(actor, dict) else None,
        actor_data.get("gpu_build_state") if isinstance(actor_data, dict) else None,
        parsed.get("gpu_build_state"),
        fallback.get("gpu_build_state"),
    ) or "").strip()
    if gpu_build_state:
        update["gpu_build_state"] = gpu_build_state
    for field in ("mesh_count", "renderable_mesh_count", "invalid_mesh_count"):
        value = next(
            (
                candidate
                for candidate in (
                    actor.get(field) if isinstance(actor, dict) else None,
                    actor_data.get(field) if isinstance(actor_data, dict) else None,
                    parsed.get(field),
                    fallback.get(field),
                )
                if candidate is not None
            ),
            None,
        )
        if value is not None:
            try:
                update[field] = max(0, int(value or 0))
            except (TypeError, ValueError):
                update[field] = 0
    update["entity_type"] = "environment"
    update["semantic_role"] = _safe_component_token(
        _first_present(
            parsed.get("semantic_role"),
            actor_data.get("semantic_role"),
            fallback.get("semantic_role"),
            "indoor_enclosure" if component_type == "room_box" else "walkable_floor" if component_type == "room_floor" else "environment_component",
        ),
        fallback="environment_component",
    )
    normalized_component_type = component_type.strip().lower()
    if normalized_component_type in {"room_box", "room_shell", "indoor_enclosure"}:
        update["grounding_status"] = "enclosure"
    elif normalized_component_type in {
        "room_floor",
        "terrain",
        "ground",
        "walkable_floor",
        "transition_zone",
    }:
        update["grounding_status"] = "grounded"
    elif normalized_component_type in {"sky", "skybox"}:
        update["grounding_status"] = "not_applicable"
    size = _vector3(_first_present(
        actor.get("size") if isinstance(actor, dict) else None,
        actor_data.get("size") if isinstance(actor_data, dict) else None,
        parsed.get("size"),
        fallback.get("size"),
    ))
    if size:
        update["size"] = size
    for field in ("surface", "terrain_profile", "sky_mode", "boundary_style"):
        value = _safe_component_text(
            _first_present(parsed.get(field), actor_data.get(field), actor.get(field) if isinstance(actor, dict) else None, fallback.get(field)),
            fallback="",
            allow_empty=True,
        )
        if value:
            update[field] = value
    return update


def _safe_component_token(raw: Any, *, fallback: str, allow_empty: bool = False) -> str:
    fallback_text = str(fallback or "").strip()
    text = str(raw or "").strip()
    if not text:
        return "" if allow_empty else fallback_text
    if _adapter_text_has_unsafe_token(text):
        return "" if allow_empty and not fallback_text else fallback_text
    if len(text) > 80 or any(ch.isspace() for ch in text) or "/" in text or "\\" in text or ":" in text:
        return "" if allow_empty and not fallback_text else fallback_text
    return text


def _safe_component_text(raw: Any, *, fallback: str, allow_empty: bool = False) -> str:
    fallback_text = str(fallback or "").strip()
    text = str(raw or "").strip()
    if not text:
        return "" if allow_empty else fallback_text
    if _adapter_text_has_unsafe_token(text):
        return "" if allow_empty and not fallback_text else fallback_text
    return text[:160]


def _engine_actor_name_for_transform(actor: Mapping[str, Any], delta: Mapping[str, Any], actor_id: str) -> str:
    """Pick the engine-facing actor name for set_actor_transform."""

    fallback = _safe_component_text(
        _first_present(delta.get("actor_name"), actor.get("requested_name"), actor.get("display_name"), actor_id),
        fallback=str(actor_id or ""),
    )
    for candidate in (
        actor.get("native_name"),
        actor.get("name"),
        delta.get("actor_name"),
        actor.get("requested_name"),
        actor.get("display_name"),
        actor_id,
    ):
        safe = _safe_component_text(candidate, fallback="", allow_empty=True)
        if safe:
            return safe
    return fallback


def _runtime_actor_display_name_for_transform(actor: Mapping[str, Any], delta: Mapping[str, Any], actor_id: str) -> str:
    return _safe_component_text(
        _first_present(
            actor.get("display_name"),
            actor.get("requested_name"),
            delta.get("actor_name"),
            actor.get("name"),
            actor.get("native_name"),
            actor_id,
        ),
        fallback=str(actor_id or ""),
    )


def _coerce_adapter_bool(raw: Any, *, default: bool = False) -> bool:
    if isinstance(raw, bool):
        return raw
    if isinstance(raw, (int, float)):
        return bool(raw)
    text = str(raw or "").strip().lower()
    if not text:
        return default
    if text in {"1", "true", "yes", "y", "on", "enabled"}:
        return True
    if text in {"0", "false", "no", "n", "off", "disabled", "none", "null"}:
        return False
    return default


def _invoke_image_tool(image_tool: Any, payload: dict[str, Any]) -> Any:
    return _invoke_tool_safely(image_tool, payload, fallback="image resource failed")


def _invoke_tool(tool: Any, payload: dict[str, Any]) -> Any:
    invoke = getattr(tool, "invoke", None)
    if callable(invoke):
        return invoke(payload)
    if callable(tool):
        return tool(payload)
    raise TypeError("tool must be callable or expose invoke(payload)")


def _invoke_tool_safely(tool: Any, payload: dict[str, Any], *, fallback: str) -> Any:
    try:
        return _invoke_tool(tool, payload)
    except Exception as exc:  # noqa: BLE001
        raise RuntimeError(fallback) from exc


def _normalize_scene_snapshot_result(parsed: dict[str, Any], *, room_id: str, scene_name: str) -> dict[str, Any]:
    status = str(parsed.get("status") or "").strip().lower()
    success = _coerce_adapter_bool(parsed.get("success"), default=True)
    if status in {"error", "failed", "failure", "fail"} or not success or parsed.get("error"):
        raise RuntimeError(_safe_adapter_error_message(parsed, fallback="scene snapshot failed"))
    actors = parsed.get("actors")
    if not isinstance(actors, list):
        actors = []
    safe_actors = [
        actor
        for actor in (
            _normalize_snapshot_actor(item, scene_name=scene_name, index=index)
            for index, item in enumerate(actors, start=1)
        )
        if actor
    ]
    return {
        "room_id": room_id,
        "scene_name": str(_first_present(scene_name, parsed.get("scene_name"), parsed.get("scene"), room_id)),
        "actor_count": len(safe_actors),
        "actors": safe_actors,
        "source": "scene_snapshot_tool",
    }


def normalize_current_unversioned_v1_scene_snapshot(
    raw_snapshot: dict[str, Any],
    *,
    room_id: str,
    scene_name: str,
    build_fingerprint: str,
) -> dict[str, Any]:
    """Strictly validate the current native DTO before Runtime normalization."""

    validated = validate_current_unversioned_v1_snapshot(
        raw_snapshot,
        build_fingerprint=build_fingerprint,
    )
    actors = tuple(validated.get("actors") or ())
    plan_ids = {str(actor.get("source_plan_id") or "").strip() for actor in actors}
    scene_versions = {int(actor.get("source_scene_version") or 0) for actor in actors}
    actor_ids = [str(actor.get("actor_guid") or "").strip() for actor in actors]
    entity_ids = [str(actor.get("entity_id") or "").strip() for actor in actors]
    if len(plan_ids) > 1:
        raise EngineSnapshotInputContractError(
            "engine_snapshot_plan_identity_drift",
            "Native snapshot actors do not share one source_plan_id.",
        )
    if len(scene_versions) > 1:
        raise EngineSnapshotInputContractError(
            "engine_snapshot_scene_version_drift",
            "Native snapshot actors do not share one source_scene_version.",
        )
    if len(set(actor_ids)) != len(actor_ids) or len(set(entity_ids)) != len(entity_ids):
        raise EngineSnapshotInputContractError(
            "engine_snapshot_actor_identity_drift",
            "Native snapshot contains duplicate actor or entity identity.",
        )
    return {
        "input_contract_version": ENGINE_SNAPSHOT_INPUT_CONTRACT_VERSION,
        "build_fingerprint": str(build_fingerprint),
        "schema_fingerprint": current_unversioned_v1_schema_fingerprint(),
        "plan_id": next(iter(plan_ids), ""),
        "scene_version": next(iter(scene_versions), 0),
        "snapshot": _normalize_scene_snapshot_result(
            validated,
            room_id=str(room_id or "default"),
            scene_name=str(scene_name or ""),
        ),
    }


def _normalize_snapshot_actor(actor: Any, *, scene_name: str, index: int = 0) -> dict[str, Any]:
    if not isinstance(actor, dict):
        return {}
    raw_actor_id = str(
        _first_present(
            actor.get("actor_id"),
            actor.get("actor_guid"),
            actor.get("guid"),
            actor.get("name"),
        )
        or ""
    ).strip()
    raw_actor_name = str(_first_present(actor.get("name"), actor.get("actor_name"), raw_actor_id) or "").strip()
    if not raw_actor_id and not raw_actor_name:
        return {}
    fallback_id = f"snapshot-actor-{max(int(index or 0), 1):02d}"
    actor_id = _safe_component_token(raw_actor_id, fallback=fallback_id)
    actor_name = _safe_component_text(raw_actor_name, fallback=actor_id)
    geometry = actor.get("geometry") if isinstance(actor.get("geometry"), dict) else {}
    safe: dict[str, Any] = {
        "actor_id": actor_id,
        "name": actor_name,
        "source": "scene_snapshot",
        "status": "success",
    }
    token_fields = {
        "entity_id": ("entity_id", "runtime_entity_id"),
        "asset_id": ("asset_id",),
        "entity_type": ("entity_type",),
        "semantic_role": ("semantic_role",),
        "plan_id": ("source_plan_id", "plan_id"),
        "batch_id": ("source_batch_id", "batch_id"),
    }
    for target_field, source_fields in token_fields.items():
        value = _safe_component_token(
            _first_present(*(actor.get(field) for field in source_fields)),
            fallback="",
            allow_empty=True,
        )
        if value:
            safe[target_field] = value
    model_ref = _safe_component_text(
        actor.get("model_ref"),
        fallback="",
        allow_empty=True,
    )
    if model_ref:
        safe["model_ref"] = model_ref
    effective_scene_name = _safe_component_text(
        _first_present(scene_name, actor.get("scene_name"), actor.get("scene")),
        fallback="",
        allow_empty=True,
    )
    if effective_scene_name:
        safe["scene_name"] = effective_scene_name
    for field in ("position", "rotation", "scale"):
        value = _first_present(actor.get(field), geometry.get(field))
        if value is not None:
            safe[field] = value
    aabb = _first_present(
        actor.get("aabb"),
        actor.get("world_aabb"),
        actor.get("bounds"),
        geometry.get("aabb"),
        geometry.get("world_aabb"),
        geometry.get("bounds"),
    )
    if aabb is not None:
        safe["aabb"] = aabb
    bounds_ready = _coerce_adapter_bool(
        actor.get("bounds_ready"),
        default=aabb is not None,
    )
    safe["bounds_ready"] = bounds_ready
    for field in ("render_status_observed", "render_ready", "render_failed"):
        if field in actor:
            safe[field] = _coerce_adapter_bool(actor.get(field), default=False)
    gpu_build_state = str(actor.get("gpu_build_state") or "").strip()
    if gpu_build_state:
        safe["gpu_build_state"] = gpu_build_state
    for field in ("mesh_count", "renderable_mesh_count", "invalid_mesh_count"):
        if field in actor:
            try:
                safe[field] = max(0, int(actor.get(field) or 0))
            except (TypeError, ValueError):
                safe[field] = 0
    if bounds_ready and aabb is not None:
        safe["bounds_source"] = "engine_actual"
        safe["engine_lifecycle_status"] = "bounds_ready"
        safe["sync_status"] = "engine_imported"
    raw_version = _first_present(actor.get("actor_version"), actor.get("version"))
    try:
        version = max(1, int(raw_version or 1))
    except (TypeError, ValueError):
        version = 1
    safe["actor_version"] = version
    safe["entity_version"] = version
    safe["version"] = version
    return safe


def _normalize_scene_review_result(parsed: dict[str, Any], *, payload: dict[str, Any]) -> dict[str, Any]:
    status = str(parsed.get("status") or "").strip().lower()
    success = _coerce_adapter_bool(parsed.get("success"), default=True)
    if status in {"error", "failed", "failure", "fail"} or not success or parsed.get("error"):
        raise RuntimeError(_safe_adapter_error_message(parsed, fallback="scene review failed"))
    raw_issues = parsed.get("issues") or []
    issues: list[dict[str, Any]] = []
    for item in raw_issues:
        issue = _normalize_review_issue(item)
        if issue:
            issues.append(issue)
    overall = _safe_component_text(parsed.get("overall"), fallback=("WARN" if issues else "PASS")).upper()
    status = _safe_component_text(parsed.get("status"), fallback="", allow_empty=True).lower()
    if not status:
        status = "needs_adjustment" if issues and overall not in {"PASS", "SKIPPED"} else overall.lower()
    advisory_items = [
        item
        for item in (_normalize_review_advisory(item) for item in (parsed.get("advisory_items") or []))
        if item
    ]
    return {
        "plan_id": str(payload.get("plan_id") or parsed.get("plan_id") or ""),
        "batch_id": str(payload.get("batch_id") or parsed.get("batch_id") or ""),
        "contract_version": int(payload.get("contract_version") or parsed.get("contract_version") or 0),
        "checkpoint_type": _safe_component_text(
            payload.get("checkpoint_type") or parsed.get("checkpoint_type"),
            fallback="geometry_review",
        ),
        "reviewed_targets": [
            _safe_component_text(item, fallback=f"target-{index:02d}")
            for index, item in enumerate(
                (payload.get("reviewed_targets") or parsed.get("reviewed_targets") or []),
                start=1,
            )
            if str(item or "").strip()
        ],
        "status": status,
        "overall": overall,
        "score": parsed.get("score") if isinstance(parsed.get("score"), (int, float)) else None,
        "issue_count": len(issues),
        "issues": issues,
        "advisory_items": advisory_items,
        "source": "scene_review",
    }


_REVIEW_TEXT_FIELDS = {
    "actor_name",
    "message",
    "name",
    "reason",
    "severity",
    "target_hint",
    "type",
}
_REVIEW_ADVISORY_TEXT_FIELDS = {
    "actor_name",
    "message",
    "reason",
    "summary",
    "target_hint",
    "type",
}
_REVIEW_TOKEN_FIELDS = {"actor_id"}
_REVIEW_ADVISORY_TOKEN_FIELDS = {"actor_id", "batch_id", "checkpoint_type"}
_REVIEW_NUMERIC_FIELDS = {"confidence", "current_y", "suggested_y"}
_REVIEW_VECTOR_FIELDS = {"bounds", "current_position", "suggested_position"}
_REVIEW_ADVISORY_BOOL_FIELDS = {"requires_confirmation"}
_REVIEW_ISSUE_ALLOWED_FIELDS = (
    _REVIEW_TEXT_FIELDS
    | _REVIEW_TOKEN_FIELDS
    | _REVIEW_NUMERIC_FIELDS
    | _REVIEW_VECTOR_FIELDS
)
_REVIEW_ADVISORY_ALLOWED_FIELDS = (
    _REVIEW_ADVISORY_TEXT_FIELDS
    | _REVIEW_ADVISORY_TOKEN_FIELDS
    | {"confidence", "requires_confirmation"}
)


def _normalize_review_issue(item: Any) -> dict[str, Any]:
    if isinstance(item, str):
        message = _safe_component_text(item, fallback="内部细节已隐藏")
        return {"type": "advisory", "message": message, "severity": "low"}
    if not isinstance(item, dict):
        return {}
    safe: dict[str, Any] = {}
    for field, value in item.items():
        key = str(field or "").strip()
        if key not in _REVIEW_ISSUE_ALLOWED_FIELDS:
            continue
        normalized = _normalize_review_field(key, value, advisory=False)
        if normalized is not None:
            safe[key] = normalized
    safe["type"] = _safe_component_text(safe.get("type"), fallback="advisory")
    if "severity" in safe:
        safe["severity"] = _safe_component_text(safe.get("severity"), fallback="low")
    if not any(str(safe.get(field) or "").strip() for field in ("message", "reason", "target_hint")):
        safe["message"] = "内部细节已隐藏"
    return safe


def _normalize_review_advisory(item: Any) -> dict[str, Any]:
    if not isinstance(item, dict):
        return {}
    safe: dict[str, Any] = {}
    for field, value in item.items():
        key = str(field or "").strip()
        if key not in _REVIEW_ADVISORY_ALLOWED_FIELDS:
            continue
        normalized = _normalize_review_field(key, value, advisory=True)
        if normalized is not None:
            safe[key] = normalized
    if not any(str(safe.get(field) or "").strip() for field in ("message", "reason", "summary")):
        safe["summary"] = "内部细节已隐藏"
    return safe


def _normalize_review_field(field: str, value: Any, *, advisory: bool) -> Any:
    token_fields = _REVIEW_ADVISORY_TOKEN_FIELDS if advisory else _REVIEW_TOKEN_FIELDS
    text_fields = _REVIEW_ADVISORY_TEXT_FIELDS if advisory else _REVIEW_TEXT_FIELDS
    if field in token_fields:
        return _safe_component_token(value, fallback="", allow_empty=True)
    if field in text_fields:
        fallback = "advisory" if field == "type" else ("low" if field == "severity" else "内部细节已隐藏")
        return _safe_component_text(value, fallback=fallback)
    if field in _REVIEW_NUMERIC_FIELDS:
        return value if isinstance(value, (int, float)) else None
    if field in _REVIEW_VECTOR_FIELDS:
        if isinstance(value, list) and all(isinstance(number, (int, float)) for number in value):
            return list(value)
        return None
    if field in _REVIEW_ADVISORY_BOOL_FIELDS:
        return bool(value) if isinstance(value, bool) else None
    return None


def make_engine_actor_import_provider(
    *,
    import_tool: Any,
    engine_gate: Any,
    scene_name: str = "",
    scene_snapshot_provider: Callable[[Any], dict[str, Any]] | None = None,
    transform_tool: Any | None = None,
    parse_result: Callable[[Any], dict[str, Any]] | None = None,
) -> ResourceProvider:
    """Create a Runtime actor-import provider backed by EngineWriteGate.

    This is the narrow C++/engine bridge boundary for Runtime.  It imports only
    the batch items whose model resources are already ready in RuntimeState-like
    payload data, and returns actor facts for StatePatch.  It does not create a
    SceneLayout, run progressive workflow, or clear existing scene actors.
    """

    if import_tool is None:
        raise ValueError("import_tool is required")
    if engine_gate is None:
        raise ValueError("engine_gate is required")
    bridge = RuntimeCppBridge(engine_gate=engine_gate, parse_result=parse_result)

    def _provider(payload: dict[str, Any]) -> dict[str, dict[str, Any]]:
        batch_id = str(payload.get("batch_id") or "")
        plan_id = str(payload.get("plan_id") or "")
        model_items = [str(item) for item in (payload.get("model_items") or []) if str(item or "")]
        placements = dict(payload.get("placements") or {})
        model_resources = _model_resources_from_payload(payload)
        actors: dict[str, dict[str, Any]] = {}
        import_results: list[dict[str, Any]] = []
        bridge_results: list[RuntimeCppBridgeResult] = []
        bridge_skip_reason_counts: dict[str, int] = {}
        for index, name in enumerate(model_items, start=1):
            resource = model_resources.get(name)
            model_path = str(_resource_model_path(resource) or "")
            if not model_path:
                bridge_skip_reason_counts["missing_ready_model_resource"] = (
                    bridge_skip_reason_counts.get("missing_ready_model_resource", 0) + 1
                )
                import_results.append({
                    "actor_name": name,
                    "status": "failed",
                    "failure_code": "missing_ready_model_resource",
                    "reason": "missing ready model resource",
                })
                continue
            placement = dict(placements.get(name) or {})
            asset_id = _stable_runtime_asset_id(resource, model_path=model_path)
            model_ref = _safe_component_text(
                _first_present(
                    resource.get("model_ref"),
                    resource.get("model_id"),
                    resource.get("resource_id"),
                    resource.get("model_request_id"),
                    asset_id,
                ),
                fallback=asset_id,
            )
            actor_guid = _stable_runtime_actor_guid(
                plan_id=plan_id,
                batch_id=batch_id,
                asset_id=asset_id,
                requested_name=name,
                source_index=index,
            )
            entity_id = _stable_runtime_entity_id(actor_guid)
            actor_request_id = f"{batch_id}-{index:02d}" if batch_id else f"runtime-{index:02d}"
            import_payload = {
                "model_path": model_path,
                "actor_name": name,
                "model_name": name,
                "asset_id": asset_id,
                "model_ref": model_ref,
                "object_id": actor_request_id,
                "target": name,
                "actor_guid": actor_guid,
                "entity_id": entity_id,
                "entity_version": 1,
                "source_plan_id": plan_id,
                "source_batch_id": batch_id,
                "source_scene_version": max(1, int(payload.get("scene_version") or 1)),
                "skip_if_exists": True,
                "update_if_exists": False,
                "position": list(placement.get("position") or [0.0, 0.0, 0.0]),
                "rotation": list(placement.get("rotation") or [0.0, 0.0, 0.0]),
                "scale": list(placement.get("scale") or [1.0, 1.0, 1.0]),
            }
            if plan_id:
                import_payload["plan_id"] = plan_id
            if batch_id:
                import_payload["batch_id"] = batch_id
            effective_scene_name = str(payload.get("scene_name") or scene_name or "")
            if effective_scene_name:
                import_payload["scene_name"] = effective_scene_name
            bridge_result = bridge.invoke_tool(import_tool, import_payload, error_code="cpp_actor_import_failed")
            bridge_results.append(bridge_result)
            if not bridge_result.success:
                import_results.append({
                    "actor_name": name,
                    "status": "failed",
                    "failure_code": "cpp_actor_import_failed",
                    "reason": (
                        _safe_adapter_error_message(
                            {"message": bridge_result.message},
                            fallback="actor import failed",
                        )
                    ),
                })
                continue
            parsed = bridge_result.payload
            try:
                actor = _normalize_import_result(
                    parsed,
                    fallback_name=name,
                    model_path=model_path,
                    batch_id=batch_id,
                    plan_id=plan_id,
                    scene_name=effective_scene_name,
                    placement=placement,
                    asset_id=asset_id,
                    model_ref=model_ref,
                )
                actor["entity_id"] = entity_id
                actor["actor_request_id"] = actor_request_id
            except Exception as exc:  # noqa: BLE001
                import_results.append({
                    "actor_name": name,
                    "status": "failed",
                    "failure_code": "actor_import_invalid_result",
                    "reason": _safe_adapter_error_message({"message": str(exc)}, fallback="actor import failed"),
                })
                continue
            actors[actor["actor_id"]] = actor
            import_results.append({
                "actor_id": actor["actor_id"],
                "entity_id": actor["entity_id"],
                "actor_version": int(actor.get("actor_version") or 1),
                "actor_name": actor["name"],
                "display_name": actor.get("display_name") or actor["name"],
                "native_name": actor.get("native_name") or actor["name"],
                "requested_name": actor.get("requested_name") or name,
                "aliases": list(actor.get("aliases") or []),
                "status": "success",
            })
        if scene_snapshot_provider is not None and actors:
            _reconcile_engine_ready_facts(
                actors,
                snapshot_provider=scene_snapshot_provider,
                room_id=str(payload.get("room_id") or ""),
                scene_name=str(payload.get("scene_name") or scene_name or ""),
            )
            for row in import_results:
                actor = actors.get(str(row.get("actor_id") or ""))
                if not actor:
                    continue
                row["bounds_ready"] = bool(actor.get("bounds_ready"))
                row["bounds_source"] = str(actor.get("bounds_source") or "estimated")
                row["engine_lifecycle_status"] = str(
                    actor.get("engine_lifecycle_status") or "engine_loading"
                )
                for field in (
                    "render_status_observed",
                    "render_ready",
                    "render_failed",
                    "gpu_build_state",
                    "mesh_count",
                    "renderable_mesh_count",
                    "invalid_mesh_count",
                ):
                    if field in actor:
                        row[field] = actor.get(field)
        if transform_tool is not None and actors:
            for actor_id, actor in list(actors.items()):
                support_type = _runtime_actor_support_type(actor)
                actor["support_type"] = support_type
                if support_type != "floor_supported":
                    # Naming can choose the support domain, but it cannot prove
                    # that a wall/ceiling attachment actually exists. Preserve
                    # an explicit Engine fact; otherwise keep the entity out of
                    # Game-ready until a support review supplies one.
                    actor.setdefault("grounding_status", "needs_review")
                    continue
                if not bool(actor.get("bounds_ready")):
                    actor.setdefault("grounding_status", "needs_review")
                    continue
                engine_actor_name = _engine_actor_name_for_transform(actor, {}, actor_id)
                position = _vector3(actor.get("position"))
                if not engine_actor_name or not position:
                    actor["grounding_status"] = "needs_review"
                    continue
                transform_payload = {
                    "actor_id": actor_id,
                    "actor_name": engine_actor_name,
                    "position": tuple(float(value) for value in position[:3]),
                    "snap_to_ground": True,
                    "ground_y": 0.0,
                    "ground_clearance": 0.02,
                }
                if plan_id:
                    transform_payload["plan_id"] = plan_id
                if batch_id:
                    transform_payload["batch_id"] = batch_id
                effective_scene_name = str(actor.get("scene_name") or payload.get("scene_name") or scene_name or "")
                if effective_scene_name:
                    transform_payload["scene_name"] = effective_scene_name
                transform_result = bridge.set_transform(
                    transform_tool,
                    transform_payload,
                    error_code="cpp_actor_initial_grounding_failed",
                )
                bridge_results.append(transform_result)
                if not transform_result.success:
                    actor["grounding_status"] = "needs_review"
                    continue
                update = _normalize_transform_result(
                    transform_result.payload,
                    actor_id=actor_id,
                    fallback_name=str(actor.get("name") or actor_id),
                    fallback_position=position,
                    scene_name=effective_scene_name,
                )
                engine_result = dict(update.pop("engine_transform_result", None) or {})
                actor.update(update)
                actor["support_type"] = support_type
                actor["grounding_status"] = (
                    "grounded"
                    if bool(engine_result.get("ground_snapped"))
                    or _actor_bottom_is_grounded(actor, ground_y=0.0, clearance=0.02)
                    else "needs_review"
                )
                for row in import_results:
                    if str(row.get("actor_id") or "") != actor_id:
                        continue
                    row["grounding_status"] = actor["grounding_status"]
                    row["ground_snapped"] = bool(engine_result.get("ground_snapped"))
                    row["position"] = list(actor.get("position") or position)
                    if actor.get("aabb"):
                        row["aabb"] = dict(actor.get("aabb") or {})
                    break
        status_counts: dict[str, int] = {}
        for item in import_results:
            status_key = str(item.get("status") or "unknown").strip().lower() or "unknown"
            status_counts[status_key] = status_counts.get(status_key, 0) + 1
        return {
            "actors": actors,
            "import_results": import_results,
            "source": "engine_actor_import_provider",
            "engine_write_result": {
                "provider_source": "engine_actor_import_provider",
                "requested_count": len(model_items),
                "identity_result_count": len(actors),
                "missing_identity_count": sum(
                    1
                    for item in import_results
                    if str(item.get("status") or "").strip().lower() == "failed"
                    and "actor id" in str(item.get("reason") or "").lower()
                ),
                "status_counts": status_counts,
                **_merge_bridge_boundary_facts(bridge_results),
                "bridge_skipped_count": sum(bridge_skip_reason_counts.values()),
                "bridge_skip_reason_counts": dict(sorted(bridge_skip_reason_counts.items())),
            },
        }

    return _provider


def _stable_runtime_asset_id(resource: dict[str, Any], *, model_path: str) -> str:
    for key in ("asset_id", "model_id", "resource_id", "model_request_id", "request_id"):
        value = _safe_component_token(resource.get(key), fallback="", allow_empty=True)
        if value:
            return value
    path = Path(str(model_path or ""))
    fingerprint_parts = [path.name]
    try:
        stat = path.stat()
        fingerprint_parts.extend([str(stat.st_size), str(stat.st_mtime_ns)])
    except OSError:
        fingerprint_parts.append("unobserved")
    digest = hashlib.sha256("|".join(fingerprint_parts).encode("utf-8")).hexdigest()[:24]
    return f"asset-{digest}"


def _stable_runtime_actor_guid(
    *,
    plan_id: str,
    batch_id: str,
    asset_id: str,
    requested_name: str,
    source_index: int,
) -> str:
    identity = "|".join(
        (
            str(plan_id or "runtime"),
            str(batch_id or "batch"),
            str(asset_id or "asset"),
            str(requested_name or "actor"),
            str(max(0, int(source_index))),
        )
    )
    digest = hashlib.sha256(identity.encode("utf-8")).hexdigest()[:32]
    return f"runtime-actor-{digest}"


def _stable_runtime_entity_id(actor_guid: str) -> str:
    digest = hashlib.sha256(f"runtime-entity|{str(actor_guid or '')}".encode("utf-8")).hexdigest()
    return f"entity-{digest[:32]}"


def _runtime_actor_support_type(actor: dict[str, Any]) -> str:
    return classify_support_type(
        (
            actor.get("requested_name"),
            actor.get("name"),
            actor.get("display_name"),
            actor.get("semantic_role"),
        ),
        explicit=actor.get("support_type"),
    )


def _actor_bottom_is_grounded(
    actor: dict[str, Any],
    *,
    ground_y: float,
    clearance: float,
) -> bool:
    bounds = _normalized_bounds_from(actor.get("aabb"), actor.get("bounds"), actor)
    if not bounds:
        return False
    try:
        bottom_y = float(bounds["min"][1])
    except (KeyError, IndexError, TypeError, ValueError):
        return False
    return abs(bottom_y - (float(ground_y) + float(clearance))) <= 0.05


def _reconcile_engine_ready_facts(
    entities: dict[str, dict[str, Any]],
    *,
    snapshot_provider: Callable[[Any], dict[str, Any]],
    room_id: str,
    scene_name: str,
) -> None:
    """Poll authoritative native facts and merge only actual geometry bounds."""

    try:
        timeout_s = max(
            0.0,
            min(
                180.0,
                float(os.getenv("AGENT_RUNTIME_ENGINE_READY_TIMEOUT_S", str(ENGINE_READY_TIMEOUT_DEFAULT_S))),
            ),
        )
    except (TypeError, ValueError):
        timeout_s = ENGINE_READY_TIMEOUT_DEFAULT_S
    try:
        interval_s = max(
            0.01,
            min(
                5.0,
                float(os.getenv("AGENT_RUNTIME_ENGINE_READY_POLL_S", str(ENGINE_READY_POLL_DEFAULT_S))),
            ),
        )
    except (TypeError, ValueError):
        interval_s = ENGINE_READY_POLL_DEFAULT_S
    deadline = time.monotonic() + timeout_s

    while True:
        try:
            snapshot = snapshot_provider({"room_id": room_id, "scene_name": scene_name})
        except Exception:  # noqa: BLE001
            snapshot = {}
        rows = [dict(item) for item in list(snapshot.get("actors") or []) if isinstance(item, dict)]
        all_ready = True
        for entity_id, entity in entities.items():
            observed = _match_engine_snapshot_actor(
                rows,
                entity,
                fallback_actor_id=str(entity_id or ""),
            )
            bounds = _normalized_bounds_from(observed or {}) if observed else None
            observed_bounds_ready = bool(observed and observed.get("bounds_ready"))
            render_status_observed = bool(observed and observed.get("render_status_observed"))
            render_ready = bool(observed and observed.get("render_ready"))
            if observed:
                for field in (
                    "render_status_observed",
                    "render_ready",
                    "render_failed",
                    "gpu_build_state",
                    "mesh_count",
                    "renderable_mesh_count",
                    "invalid_mesh_count",
                ):
                    if field in observed:
                        entity[field] = observed.get(field)
            if observed_bounds_ready and bounds and render_status_observed and render_ready:
                entity["aabb"] = bounds
                entity["bounds_ready"] = True
                entity["bounds_source"] = "engine_actual"
                entity["engine_lifecycle_status"] = "bounds_ready"
                entity["status"] = "ready"
                for field in ("position", "rotation", "scale"):
                    value = _vector3(observed.get(field))
                    if value:
                        entity[field] = value
            else:
                entity["bounds_ready"] = observed_bounds_ready and bool(bounds)
                entity["bounds_source"] = "engine_actual" if entity["bounds_ready"] else "estimated"
                entity["engine_lifecycle_status"] = "engine_loading"
                entity["status"] = "engine_loading"
                all_ready = False
        if all_ready or time.monotonic() >= deadline:
            return
        time.sleep(interval_s)


def _match_engine_snapshot_actor(
    rows: list[dict[str, Any]],
    entity: dict[str, Any],
    *,
    fallback_actor_id: str = "",
) -> dict[str, Any] | None:
    """Match one Runtime entity to one native snapshot actor without guessing.

    Native scene snapshots and import results can expose the same stable actor
    through different identity fields.  Only actor identity or stable entity
    identity may reconcile Engine facts; asset/name similarity is not ownership
    evidence and must never claim an unrelated native actor.
    """

    def normalized(value: Any) -> str:
        return str(value or "").strip().casefold()

    def unique_match(snapshot_fields: tuple[str, ...], values: list[Any]) -> dict[str, Any] | None:
        wanted = {normalized(value) for value in values if normalized(value)}
        if not wanted:
            return None
        matches = [
            row
            for row in rows
            if any(normalized(row.get(field)) in wanted for field in snapshot_fields)
        ]
        return matches[0] if len(matches) == 1 else None

    actor_match = unique_match(
        ("actor_id", "actor_guid", "guid"),
        [entity.get("actor_id"), fallback_actor_id],
    )
    if actor_match is not None:
        return actor_match

    entity_match = unique_match(
        ("entity_id", "runtime_entity_id"),
        [entity.get("entity_id")],
    )
    if entity_match is not None:
        return entity_match

    return None


def make_engine_layout_transform_provider(
    *,
    transform_tool: Any,
    engine_gate: Any,
    scene_name: str = "",
    parse_result: Callable[[Any], dict[str, Any]] | None = None,
) -> ResourceProvider:
    """Create a Runtime layout-transform provider backed by EngineWriteGate.

    The provider applies already-confirmed low-risk Runtime deltas to the engine
    through the narrow set_actor_transform tool.  It does not create new deltas,
    does not call legacy workflows, and returns only observed transform facts for
    RuntimeState reconciliation.
    """

    if transform_tool is None:
        raise ValueError("transform_tool is required")
    if engine_gate is None:
        raise ValueError("engine_gate is required")
    bridge = RuntimeCppBridge(engine_gate=engine_gate, parse_result=parse_result)

    def _provider(payload: dict[str, Any]) -> dict[str, Any]:
        plan_id = str(payload.get("plan_id") or "")
        batch_id = str(payload.get("batch_id") or "")
        applied = [dict(item) for item in (payload.get("applied_deltas") or []) if isinstance(item, dict)]
        actors = {str(key): dict(value) for key, value in (payload.get("actors") or {}).items() if isinstance(value, dict)}
        actor_updates: dict[str, dict[str, Any]] = {}
        transform_results: list[dict[str, Any]] = []
        bridge_results: list[RuntimeCppBridgeResult] = []
        for item in applied:
            actor_id = str(item.get("actor_id") or "")
            actor = dict(actors.get(actor_id) or {})
            actor_name = _runtime_actor_display_name_for_transform(actor, item, actor_id)
            engine_actor_name = _engine_actor_name_for_transform(actor, item, actor_id)
            position = item.get("position")
            if not actor_id or not engine_actor_name or not isinstance(position, list) or len(position) < 3:
                transform_results.append({
                    "actor_id": actor_id,
                    "actor_name": actor_name,
                    "status": "skipped",
                    "failure_code": "missing_transform_target",
                    "reason": "missing actor or position",
                })
                continue
            transform_payload = {
                "actor_id": actor_id,
                "actor_name": engine_actor_name,
                "position": tuple(float(value) for value in position[:3]),
                "snap_to_ground": bool(
                    item.get("snap_to_ground")
                    or item.get("ground_snapped")
                    or str(item.get("support_type") or "").strip().lower() == "floor_supported"
                ),
            }
            if plan_id:
                transform_payload["plan_id"] = plan_id
            if batch_id:
                transform_payload["batch_id"] = batch_id
            if "ground_y" in item:
                transform_payload["ground_y"] = float(item.get("ground_y") or 0.0)
            if "ground_clearance" in item:
                transform_payload["ground_clearance"] = float(item.get("ground_clearance") or 0.02)
            effective_scene_name = str(actor.get("scene_name") or payload.get("scene_name") or scene_name or "")
            if effective_scene_name:
                transform_payload["scene_name"] = effective_scene_name
            bridge_result = bridge.set_transform(transform_tool, transform_payload, error_code="cpp_actor_transform_failed")
            bridge_results.append(bridge_result)
            if not bridge_result.success:
                transform_results.append({
                    "actor_id": actor_id,
                    "actor_name": actor_name,
                    "status": "failed",
                    "failure_code": "cpp_actor_transform_failed",
                    "reason": (
                        _safe_transform_skip_reason(bridge_result.message)
                        or "actor transform failed"
                    ),
                })
                continue
            parsed = bridge_result.payload
            update = _normalize_transform_result(
                parsed,
                actor_id=actor_id,
                fallback_name=actor_name,
                fallback_position=position,
                scene_name=effective_scene_name,
            )
            engine_result = dict(update.get("engine_transform_result") or {})
            update.pop("engine_transform_result", None)
            actor_updates[actor_id] = update
            transform_results.append({
                "actor_id": actor_id,
                "actor_name": actor_name,
                "status": "success",
                "position": list(update.get("position") or position),
                "observed_position": bool(engine_result.get("observed_position")),
                "ground_snapped": bool(engine_result.get("ground_snapped")),
                "overlap_resolved": bool(engine_result.get("overlap_resolved")),
            })
            if engine_result.get("skipped_reason"):
                transform_results[-1]["skipped_reason"] = _safe_transform_skip_reason(
                    engine_result.get("skipped_reason")
                )
            if update.get("aabb"):
                transform_results[-1]["aabb"] = dict(update.get("aabb") or {})
            if update.get("sync_status"):
                transform_results[-1]["sync_status"] = str(update.get("sync_status") or "")
            if update.get("sync_lifecycle_status"):
                transform_results[-1]["sync_lifecycle_status"] = str(update.get("sync_lifecycle_status") or "")
            if update.get("rotation"):
                transform_results[-1]["rotation"] = list(update.get("rotation") or [])
            if update.get("scale"):
                transform_results[-1]["scale"] = list(update.get("scale") or [])
        status_counts: dict[str, int] = {}
        observed_position_count = 0
        for item in transform_results:
            status_key = str(item.get("status") or "unknown").strip().lower() or "unknown"
            status_counts[status_key] = status_counts.get(status_key, 0) + 1
            if item.get("observed_position"):
                observed_position_count += 1
        return {
            "actor_updates": actor_updates,
            "transform_results": transform_results,
            "source": "engine_layout_transform_provider",
            "engine_write_result": {
                "provider_source": "engine_layout_transform_provider",
                "requested_count": len(applied),
                "updated_count": len(actor_updates),
                "observed_position_count": observed_position_count,
                "status_counts": status_counts,
                **_merge_bridge_boundary_facts(bridge_results),
            },
        }

    return _provider


def make_engine_actor_delete_provider(
    *,
    delete_tool: Any,
    engine_gate: Any,
    scene_name: str = "",
    parse_result: Callable[[Any], dict[str, Any]] | None = None,
) -> ResourceProvider:
    """Create a Runtime actor-delete provider backed by EngineWriteGate.

    The provider executes only already-confirmed Runtime delete targets. It does
    not decide whether deletion is safe, and it never receives prompt/provider
    raw context. Failed deletes are returned as per-actor results so RuntimeState
    can preserve the advisory decision without pretending the engine changed.
    """

    if delete_tool is None:
        raise ValueError("delete_tool is required")
    if engine_gate is None:
        raise ValueError("engine_gate is required")
    bridge = RuntimeCppBridge(engine_gate=engine_gate, parse_result=parse_result)

    def _provider(payload: dict[str, Any]) -> dict[str, Any]:
        plan_id = str(payload.get("plan_id") or "")
        proposal_id = str(payload.get("proposal_id") or "")
        actors = {
            str(key): dict(value)
            for key, value in (payload.get("actors") or {}).items()
            if isinstance(value, dict)
        }
        requested = [
            dict(item)
            for item in (payload.get("marked_deleted_actors") or payload.get("target_actors") or [])
            if isinstance(item, dict)
        ]
        actor_updates: dict[str, dict[str, Any]] = {}
        delete_results: list[dict[str, Any]] = []
        bridge_results: list[RuntimeCppBridgeResult] = []
        for item in requested:
            actor_id = str(item.get("actor_id") or "").strip()
            actor = dict(actors.get(actor_id) or {})
            actor_name = str(actor.get("name") or item.get("actor_name") or actor_id)
            if not actor_id or not actor_name:
                delete_results.append({
                    "actor_id": actor_id,
                    "actor_name": actor_name,
                    "status": "skipped",
                    "failure_code": "missing_delete_target",
                    "reason": "missing actor",
                })
                continue
            delete_payload = {
                "actor_id": actor_id,
                "actor_name": actor_name,
                "target_actor_id": actor_id,
                "target_actor_name": actor_name,
            }
            if plan_id:
                delete_payload["plan_id"] = plan_id
            if proposal_id:
                delete_payload["proposal_id"] = proposal_id
            effective_scene_name = str(actor.get("scene_name") or payload.get("scene_name") or scene_name or "")
            if effective_scene_name:
                delete_payload["scene_name"] = effective_scene_name
            bridge_result = bridge.remove_actor(delete_tool, delete_payload, error_code="cpp_actor_delete_failed")
            bridge_results.append(bridge_result)
            if not bridge_result.success:
                delete_results.append({
                    "actor_id": actor_id,
                    "actor_name": actor_name,
                    "status": "failed",
                    "failure_code": "cpp_actor_delete_failed",
                    "reason": _safe_adapter_error_message({"message": bridge_result.message}, fallback="actor delete failed"),
                })
                continue
            parsed = bridge_result.payload
            actor_updates[actor_id] = {
                **actor,
                "actor_id": actor_id,
                "name": actor_name,
                "deleted": True,
                "sync_lifecycle_status": "deleted",
                "last_sync_event": "runtime_engine_delete",
                "last_sync_status": "deleted",
            }
            if effective_scene_name:
                actor_updates[actor_id]["scene_name"] = effective_scene_name
            delete_results.append({
                "actor_id": actor_id,
                "actor_name": actor_name,
                "status": "success",
                "observed_deleted": bool(
                    parsed.get("deleted")
                    or str(parsed.get("status") or "").strip().lower() in {"ok", "success", "deleted", "removed"}
                    or str(parsed.get("event_type") or "").strip().lower() in {"actor_deleted", "actor_removed"}
                ),
            })
        status_counts: dict[str, int] = {}
        observed_deleted_count = 0
        for item in delete_results:
            status_key = str(item.get("status") or "unknown").strip().lower() or "unknown"
            status_counts[status_key] = status_counts.get(status_key, 0) + 1
            if item.get("observed_deleted"):
                observed_deleted_count += 1
        return {
            "actor_updates": actor_updates,
            "delete_results": delete_results,
            "source": "engine_actor_delete_provider",
            "engine_write_result": {
                "provider_source": "engine_actor_delete_provider",
                "requested_count": len(requested),
                "deleted_count": len(actor_updates),
                "observed_deleted_count": observed_deleted_count,
                "status_counts": status_counts,
                **_merge_bridge_boundary_facts(bridge_results),
            },
        }

    return _provider


def _create_model_provider(model_provider_factory: Callable[[], Any] | None) -> Any:
    if model_provider_factory is not None:
        return model_provider_factory()

    raise RuntimeError("legacy model provider factory was not injected")


def _model_resources_from_payload(payload: dict[str, Any]) -> dict[str, dict[str, Any]]:
    resources = payload.get("model_resources")
    if isinstance(resources, dict):
        return {str(key): dict(value) for key, value in resources.items() if isinstance(value, dict)}
    plans = payload.get("model_resource_plans")
    batch_id = str(payload.get("batch_id") or "")
    if isinstance(plans, dict):
        if batch_id and isinstance(plans.get(batch_id), dict):
            return {str(key): dict(value) for key, value in plans[batch_id].items() if isinstance(value, dict)}
        return {str(key): dict(value) for key, value in plans.items() if isinstance(value, dict)}
    return {}


def _resource_model_path(resource: dict[str, Any] | None) -> str:
    if not resource:
        return ""
    if str(resource.get("status") or "").lower() not in {"ready", "prepared", "provider-model"}:
        return ""
    metadata = resource.get("metadata") if isinstance(resource.get("metadata"), dict) else {}
    path = str(
        _first_present(
            resource.get("local_path"),
            resource.get("model_path"),
            resource.get("path"),
            resource.get("model_folder"),
            metadata.get("local_path"),
            metadata.get("model_path"),
            metadata.get("path"),
            metadata.get("model_folder"),
        )
        or ""
    )
    return _resolve_ready_model_local_path(path, metadata=metadata)


def _parse_tool_result(raw: Any) -> dict[str, Any]:
    if isinstance(raw, dict):
        return _unwrap_tool_envelope(raw)
    if isinstance(raw, str):
        import json

        try:
            parsed = json.loads(raw)
            return _unwrap_tool_envelope(parsed) if isinstance(parsed, dict) else {"raw": raw}
        except Exception:  # noqa: BLE001
            return {"raw": raw}
    return {"raw": raw}


def _is_unstructured_raw_result(parsed: dict[str, Any]) -> bool:
    return set(parsed.keys()) == {"raw"}


def _safe_cpp_error_message(raw: Any) -> str:
    text = str(raw or "").strip()
    if not text:
        return "C++ binding failed"
    lowered = text.lower()
    blocked = (
        "api_key",
        "asset_path",
        "authorization",
        "bearer ",
        "c:\\",
        "metadata",
        "model_path",
        "prompt",
        "provider",
        "raw",
        "token",
        "url",
        "://",
    )
    if any(token in lowered for token in blocked):
        return "C++ binding failed"
    return text


def _safe_cpp_success_payload(parsed: dict[str, Any]) -> dict[str, Any]:
    allowed_keys = {
        "actor",
        "actor_data",
        "actor_guid",
        "actor_handle",
        "actor_id",
        "actor_name",
        "aabb",
        "asset_id",
        "bounds",
        "bounds_ready",
        "boundary_style",
        "component_id",
        "component_type",
        "entity_id",
        "entity_version",
        "event_type",
        "geometry",
        "gpu_build_state",
        "ground_snapped",
        "guid",
        "handle",
        "handler",
        "last_sync_event",
        "last_sync_status",
        "max",
        "min",
        "model_id",
        "model_ref",
        "mesh_count",
        "name",
        "native_actor_id",
        "native_handle",
        "observed_position",
        "overlap_resolved",
        "position",
        "proposal_id",
        "receipt_id",
        "render_failed",
        "render_ready",
        "render_status_observed",
        "renderable_mesh_count",
        "rotation",
        "scale",
        "scene_aabb",
        "scene_name",
        "size",
        "skipped_reason",
        "status",
        "status_info",
        "success",
        "payload_hash",
        "surface",
        "sync_lifecycle_status",
        "sync_status",
        "sky_mode",
        "terrain_profile",
        "type",
        "version",
        "actor_version",
        "invalid_mesh_count",
        "world_aabb",
        "world_bounds",
        "x",
        "y",
        "z",
    }
    return {
        str(key): safe_value
        for key, value in parsed.items()
        if (key_text := str(key or "").strip())
        and key_text in allowed_keys
        and not _adapter_text_has_unsafe_token(key_text)
        and (safe_value := _safe_cpp_success_value(value)) is not None
    }


def _safe_cpp_success_value(value: Any) -> Any:
    if value is None:
        return None
    if isinstance(value, bool) or isinstance(value, (int, float)):
        return value
    if isinstance(value, str):
        if _adapter_text_has_unsafe_token(value):
            return None
        return value
    if isinstance(value, list):
        safe_items = [_safe_cpp_success_value(item) for item in value[:16]]
        return [item for item in safe_items if item is not None]
    if isinstance(value, dict):
        return _safe_cpp_success_payload(value)
    return None


def _adapter_text_has_unsafe_token(raw: Any) -> bool:
    lowered = str(raw or "").strip().lower()
    if not lowered:
        return False
    blocked = (
        "api_key",
        "asset_path",
        "authorization",
        "bearer ",
        "metadata",
        "model_path",
        "prompt",
        "provider",
        "raw",
        "token",
        "url",
        "://",
        ":\\",
    )
    return any(token in lowered for token in blocked)


def _safe_adapter_error_message(parsed: dict[str, Any], *, fallback: str) -> str:
    message = _safe_cpp_error_message(parsed.get("message") or parsed.get("error"))
    if message == "C++ binding failed":
        return fallback
    return message or fallback


def _unwrap_tool_envelope(parsed: dict[str, Any]) -> dict[str, Any]:
    error_code = _tool_error_code(parsed.get("error_code"))
    if error_code:
        return {
            "status": "error",
            "error": str(parsed.get("status_info") or f"tool error {error_code}"),
            "error_code": error_code,
        }
    llm_content = parsed.get("llm_content")
    if isinstance(llm_content, list):
        for message in llm_content:
            if not isinstance(message, dict):
                continue
            parts = message.get("part")
            if not isinstance(parts, list):
                continue
            for part in parts:
                if not isinstance(part, dict):
                    continue
                content_text = part.get("content_text")
                if isinstance(content_text, str) and content_text.strip():
                    import json

                    try:
                        payload = json.loads(content_text)
                        if isinstance(payload, dict):
                            return payload
                    except Exception:  # noqa: BLE001
                        continue
                content_url = str(part.get("content_url") or "").strip()
                if content_url:
                    return {
                        "content_url": content_url,
                        "content_type": str(part.get("content_type") or "image"),
                        "parameter": part.get("parameter"),
                    }
    return parsed


def _tool_error_code(raw: Any) -> str:
    if raw is None or raw is False:
        return ""
    if isinstance(raw, (int, float)):
        return "" if int(raw) == 0 else str(int(raw))
    text = str(raw).strip()
    if not text or text in {"0", "0.0"}:
        return ""
    return text


def _normalized_bounds_from(*sources: Any) -> dict[str, list[float]]:
    for source in sources:
        if not isinstance(source, dict):
            continue
        for key in ("aabb", "bounds", "scene_aabb", "world_aabb", "world_bounds"):
            value = source.get(key)
            if isinstance(value, dict):
                min_value = _vector3(value.get("min"))
                max_value = _vector3(value.get("max"))
                if min_value and max_value:
                    return {"min": min_value, "max": max_value}
            elif isinstance(value, (list, tuple)) and len(value) >= 6:
                try:
                    numbers = [round(float(item), 4) for item in list(value[:6])]
                except (TypeError, ValueError):
                    continue
                return {"min": numbers[:3], "max": numbers[3:6]}
    return {}


def _estimated_actor_aabb_from_transform(*, position: Any, scale: Any) -> dict[str, list[float]]:
    """Return a Runtime-estimated actor AABB when the engine omits bounds.

    This is not an observed engine geometry result.  It is a minimal Runtime
    geometry estimate so scene_entity_registry and layout adjustment have a
    bounded fact to work with until the engine/C++ side reports authoritative
    bounds.
    """

    pos = _vector3(position) or [0.0, 0.0, 0.0]
    raw_scale = _vector3(scale) or [1.0, 1.0, 1.0]
    sx = max(0.2, abs(float(raw_scale[0] or 1.0)))
    sy = max(0.2, abs(float(raw_scale[1] or 1.0)))
    sz = max(0.2, abs(float(raw_scale[2] or 1.0)))
    half_x = sx * 0.5
    half_y = sy * 0.5
    half_z = sz * 0.5
    return {
        "min": [
            round(float(pos[0]) - half_x, 4),
            round(float(pos[1]) - half_y, 4),
            round(float(pos[2]) - half_z, 4),
        ],
        "max": [
            round(float(pos[0]) + half_x, 4),
            round(float(pos[1]) + half_y, 4),
            round(float(pos[2]) + half_z, 4),
        ],
    }


def _vector3(value: Any) -> list[float]:
    if isinstance(value, dict):
        raw = [value.get("x"), value.get("y"), value.get("z")]
    elif isinstance(value, (list, tuple)):
        raw = list(value[:3])
    else:
        return []
    if len(raw) < 3:
        return []
    try:
        return [round(float(raw[0]), 4), round(float(raw[1]), 4), round(float(raw[2]), 4)]
    except (TypeError, ValueError):
        return []


def _normalize_import_result(
    parsed: dict[str, Any],
    *,
    fallback_name: str,
    model_path: str,
    batch_id: str,
    plan_id: str,
    scene_name: str,
    placement: dict[str, Any],
    asset_id: str = "",
    model_ref: str = "",
) -> dict[str, Any]:
    status = str(parsed.get("status") or "").strip().lower()
    success = _coerce_adapter_bool(parsed.get("success"), default=True)
    if status in {"error", "failed", "failure", "fail"} or not success or parsed.get("error"):
        raise RuntimeError(_safe_adapter_error_message(parsed, fallback="actor import failed"))
    actor = parsed.get("actor") if isinstance(parsed.get("actor"), dict) else {}
    actor_data = parsed.get("actor_data") if isinstance(parsed.get("actor_data"), dict) else {}
    actor_name = _safe_component_text(
        _first_present(
            actor.get("name") if isinstance(actor, dict) else None,
            actor_data.get("name") if isinstance(actor_data, dict) else None,
            parsed.get("actor_name"),
            parsed.get("name"),
            fallback_name,
            parsed.get("actor_id"),
        ),
        fallback=fallback_name,
    )
    requested_name = _safe_component_text(fallback_name, fallback=fallback_name)
    actor_id = str(
        _first_present(
            actor.get("actor_guid") if isinstance(actor, dict) else None,
            actor.get("guid") if isinstance(actor, dict) else None,
            actor.get("actor_id") if isinstance(actor, dict) else None,
            actor.get("actor_handle") if isinstance(actor, dict) else None,
            actor.get("native_handle") if isinstance(actor, dict) else None,
            actor.get("native_actor_id") if isinstance(actor, dict) else None,
            actor.get("handle") if isinstance(actor, dict) else None,
            actor.get("entity_id") if isinstance(actor, dict) else None,
            actor_data.get("actor_guid") if isinstance(actor_data, dict) else None,
            actor_data.get("guid") if isinstance(actor_data, dict) else None,
            actor_data.get("actor_id") if isinstance(actor_data, dict) else None,
            actor_data.get("actor_handle") if isinstance(actor_data, dict) else None,
            actor_data.get("native_handle") if isinstance(actor_data, dict) else None,
            actor_data.get("native_actor_id") if isinstance(actor_data, dict) else None,
            actor_data.get("handle") if isinstance(actor_data, dict) else None,
            actor_data.get("entity_id") if isinstance(actor_data, dict) else None,
            parsed.get("actor_guid"),
            parsed.get("actor_id"),
            parsed.get("guid"),
            parsed.get("actor_handle"),
            parsed.get("native_handle"),
            parsed.get("native_actor_id"),
            parsed.get("handle"),
            parsed.get("entity_id"),
        )
        or ""
    )
    if not actor_id.strip():
        raise RuntimeError(f"{fallback_name}: actor import returned no actor id")
    geometry = actor.get("geometry") if isinstance(actor.get("geometry"), dict) else {}
    actor_data_geometry = actor_data.get("geometry") if isinstance(actor_data.get("geometry"), dict) else {}
    position = _first_present(
        geometry.get("position"),
        actor_data_geometry.get("position"),
        actor_data.get("position"),
        parsed.get("position"),
        placement.get("position"),
        [0.0, 0.0, 0.0],
    )
    rotation = _first_present(
        geometry.get("rotation"),
        actor_data_geometry.get("rotation"),
        actor_data.get("rotation"),
        parsed.get("rotation"),
        placement.get("rotation"),
        [0.0, 0.0, 0.0],
    )
    scale = _first_present(
        geometry.get("scale"),
        actor_data_geometry.get("scale"),
        actor_data.get("scale"),
        parsed.get("scale"),
        placement.get("scale"),
        [1.0, 1.0, 1.0],
    )
    sync_status = _safe_component_token(
        _first_present(
            parsed.get("sync_status"),
            actor_data.get("sync_status"),
            actor.get("sync_status") if isinstance(actor, dict) else None,
            parsed.get("last_sync_status"),
            actor_data.get("last_sync_status"),
        ),
        fallback="engine_imported",
    )
    sync_lifecycle_status = _safe_component_token(
        _first_present(
            parsed.get("sync_lifecycle_status"),
            actor_data.get("sync_lifecycle_status"),
            actor.get("sync_lifecycle_status") if isinstance(actor, dict) else None,
            parsed.get("last_sync_event"),
            actor_data.get("last_sync_event"),
            sync_status,
        ),
        fallback=sync_status,
    )
    result = {
        "actor_id": actor_id,
        "name": actor_name,
        "display_name": actor_name,
        "native_name": actor_name,
        "requested_name": requested_name,
        "asset_id": _safe_component_text(
            _first_present(
                asset_id,
                parsed.get("asset_id"),
                actor_data.get("asset_id") if isinstance(actor_data, dict) else None,
                actor.get("asset_id") if isinstance(actor, dict) else None,
            ),
            fallback=asset_id or actor_name,
        ),
        "model_ref": _safe_component_text(
            _first_present(
                model_ref,
                asset_id,
                parsed.get("model_ref"),
                parsed.get("model_id"),
                parsed.get("resource_id"),
                actor_data.get("model_ref") if isinstance(actor_data, dict) else None,
                actor_data.get("model_id") if isinstance(actor_data, dict) else None,
                actor_data.get("resource_id") if isinstance(actor_data, dict) else None,
                actor.get("model_ref") if isinstance(actor, dict) else None,
                actor.get("model_id") if isinstance(actor, dict) else None,
                actor.get("resource_id") if isinstance(actor, dict) else None,
            ),
            fallback=model_ref or asset_id or actor_name,
        ),
        "plan_id": plan_id,
        "batch_id": batch_id,
        "scene_name": str(scene_name or ""),
        "model_path": str(model_path),
        "source": "engine_import",
        "position": list(position),
        "rotation": list(rotation),
        "scale": list(scale),
        "sync_status": sync_status,
        "sync_lifecycle_status": sync_lifecycle_status,
        "last_sync_event": _safe_component_text(
            _first_present(parsed.get("last_sync_event"), actor_data.get("last_sync_event"), sync_lifecycle_status),
            fallback=sync_lifecycle_status,
        ),
    }
    version_value = _first_present(
        actor.get("entity_version") if isinstance(actor, dict) else None,
        actor.get("actor_version") if isinstance(actor, dict) else None,
        actor.get("version") if isinstance(actor, dict) else None,
        actor_data.get("entity_version") if isinstance(actor_data, dict) else None,
        actor_data.get("actor_version") if isinstance(actor_data, dict) else None,
        actor_data.get("version") if isinstance(actor_data, dict) else None,
        parsed.get("entity_version"),
        parsed.get("actor_version"),
        parsed.get("version"),
    )
    try:
        normalized_version = int(version_value or 0)
    except (TypeError, ValueError):
        normalized_version = 0
    if normalized_version > 0:
        result["actor_version"] = normalized_version
    aliases: list[str] = []
    for alias in (
        requested_name,
        actor_name,
        str(parsed.get("actor_name") or ""),
        str(actor_data.get("name") if isinstance(actor_data, dict) else ""),
        str(actor.get("name") if isinstance(actor, dict) else ""),
        str(asset_id or ""),
    ):
        safe_alias = _safe_component_text(alias, fallback="")
        if safe_alias and safe_alias not in aliases:
            aliases.append(safe_alias)
    if aliases:
        result["aliases"] = aliases[:8]
    bounds = _normalized_bounds_from(geometry, actor_data_geometry, actor, actor_data, parsed)
    if bounds:
        result["aabb"] = bounds
    else:
        result["aabb"] = _estimated_actor_aabb_from_transform(position=position, scale=scale)
        result["source"] = "engine_import_runtime_estimated_bounds"
        result["review_status"] = "needs_geometry_review"
        result["grounding_status"] = "needs_review"
    bounds_ready_value = _first_present(
        actor.get("bounds_ready") if isinstance(actor, dict) else None,
        actor_data.get("bounds_ready") if isinstance(actor_data, dict) else None,
        parsed.get("bounds_ready"),
    )
    if bounds_ready_value is not None:
        result["bounds_ready"] = _coerce_adapter_bool(bounds_ready_value, default=False)
    else:
        result["bounds_ready"] = bool(bounds)
    bounds_ready = bool(result.get("bounds_ready"))
    result["bounds_source"] = "engine_actual" if bounds_ready else "estimated"
    result["engine_lifecycle_status"] = "bounds_ready" if bounds_ready else "engine_loading"
    result["status"] = "ready" if bounds_ready else "engine_loading"
    for field in ("render_status_observed", "render_ready", "render_failed"):
        value = _first_present(
            actor.get(field) if isinstance(actor, dict) else None,
            actor_data.get(field) if isinstance(actor_data, dict) else None,
            parsed.get(field),
        )
        if value is not None:
            result[field] = _coerce_adapter_bool(value, default=False)
    gpu_build_state = str(_first_present(
        actor.get("gpu_build_state") if isinstance(actor, dict) else None,
        actor_data.get("gpu_build_state") if isinstance(actor_data, dict) else None,
        parsed.get("gpu_build_state"),
    ) or "").strip()
    if gpu_build_state:
        result["gpu_build_state"] = gpu_build_state
    for field in ("mesh_count", "renderable_mesh_count", "invalid_mesh_count"):
        value = _first_present(
            actor.get(field) if isinstance(actor, dict) else None,
            actor_data.get(field) if isinstance(actor_data, dict) else None,
            parsed.get(field),
        )
        if value is not None:
            try:
                result[field] = max(0, int(value or 0))
            except (TypeError, ValueError):
                result[field] = 0
    size_value = _first_present(
        actor.get("size") if isinstance(actor, dict) else None,
        actor_data.get("size") if isinstance(actor_data, dict) else None,
        parsed.get("size"),
    )
    size = _vector3(size_value)
    if size:
        result["size"] = size
    return result


def _normalize_transform_result(
    parsed: dict[str, Any],
    *,
    actor_id: str,
    fallback_name: str,
    fallback_position: list[Any],
    scene_name: str,
) -> dict[str, Any]:
    status = str(parsed.get("status") or "").strip().lower()
    success = _coerce_adapter_bool(parsed.get("success"), default=True)
    if status in {"error", "failed", "failure", "fail"} or not success or parsed.get("error"):
        raise RuntimeError(_safe_adapter_error_message(parsed, fallback="actor transform failed"))
    actor_data = parsed.get("actor_data") if isinstance(parsed.get("actor_data"), dict) else {}
    observed_position = parsed.get("position") is not None or (
        isinstance(actor_data, dict) and actor_data.get("position") is not None
    )
    position = _first_present(
        parsed.get("position"),
        actor_data.get("position") if isinstance(actor_data, dict) else None,
        fallback_position,
    )
    rotation = _first_present(
        parsed.get("rotation"),
        actor_data.get("rotation") if isinstance(actor_data, dict) else None,
    )
    scale = _first_present(
        parsed.get("scale"),
        actor_data.get("scale") if isinstance(actor_data, dict) else None,
    )
    sync_status = _safe_component_token(
        _first_present(
            parsed.get("sync_status"),
            actor_data.get("sync_status") if isinstance(actor_data, dict) else None,
            parsed.get("last_sync_status"),
            actor_data.get("last_sync_status") if isinstance(actor_data, dict) else None,
        ),
        fallback="",
        allow_empty=True,
    )
    sync_lifecycle_status = _safe_component_token(
        _first_present(
            parsed.get("sync_lifecycle_status"),
            actor_data.get("sync_lifecycle_status") if isinstance(actor_data, dict) else None,
            parsed.get("last_sync_event"),
            actor_data.get("last_sync_event") if isinstance(actor_data, dict) else None,
            sync_status,
        ),
        fallback=sync_status,
        allow_empty=True,
    )
    update: dict[str, Any] = {
        "actor_id": actor_id,
        "name": _safe_component_text(
            _first_present(
                fallback_name,
                parsed.get("actor"),
                actor_data.get("name") if isinstance(actor_data, dict) else None,
            ),
            fallback=fallback_name,
        ),
        "scene_name": str(scene_name or ""),
        "position": list(position or fallback_position),
        "source": "engine_transform",
        "engine_transform_result": {
            "ground_snapped": bool(parsed.get("ground_snapped")),
            "overlap_resolved": bool(parsed.get("overlap_resolved")),
            "observed_position": bool(observed_position),
            "skipped_reason": _safe_transform_skip_reason(parsed.get("skipped_reason")),
        },
    }
    if rotation is not None:
        update["rotation"] = list(rotation)
    if scale is not None:
        update["scale"] = list(scale)
    bounds = _normalized_bounds_from(actor_data, parsed)
    if bounds:
        update["aabb"] = bounds
    bounds_ready_value = _first_present(
        actor_data.get("bounds_ready") if isinstance(actor_data, dict) else None,
        parsed.get("bounds_ready"),
    )
    if bounds_ready_value is not None:
        update["bounds_ready"] = _coerce_adapter_bool(bounds_ready_value, default=False)
    size = _vector3(_first_present(
        actor_data.get("size") if isinstance(actor_data, dict) else None,
        parsed.get("size"),
    ))
    if size:
        update["size"] = size
    if sync_status:
        update["sync_status"] = sync_status
        update["last_sync_status"] = sync_status
    if sync_lifecycle_status:
        update["sync_lifecycle_status"] = sync_lifecycle_status
        update["last_sync_event"] = sync_lifecycle_status
    return update


def _safe_transform_skip_reason(raw: Any) -> str:
    if raw is None:
        return ""
    message = _safe_cpp_error_message(raw)
    if message == "C++ binding failed":
        return ""
    return message[:160]


def _normalize_image_result(
    parsed: dict[str, Any],
    *,
    name: str,
    batch_id: str,
    index: int,
    prompt: str,
    media_resolver: Callable[..., Any] | None,
    resolve_timeout: float,
) -> dict[str, Any]:
    status = str(parsed.get("status") or "").strip().lower()
    success = _coerce_adapter_bool(parsed.get("success"), default=True)
    if status in {"error", "failed", "failure", "fail"} or not success or parsed.get("error"):
        raise ImageResourceProviderError(
            "image_tool_call_failed",
            _safe_adapter_error_message(parsed, fallback="image resource failed"),
        )
    source_url = str(
        _first_present(
            parsed.get("image_url"),
            parsed.get("url"),
            parsed.get("content_url"),
            parsed.get("local_path"),
            parsed.get("path"),
        )
        or ""
    )
    image_paths = parsed.get("image_paths") or parsed.get("images") or []
    if not source_url and isinstance(image_paths, list) and image_paths:
        source_url = str(image_paths[0] or "")
    if not source_url:
        raise ImageResourceProviderError(
            "image_resource_resolve_failed",
            f"{name}: image provider returned no image url/path",
        )
    resolved = _resolve_image_resource(
        source_url,
        media_resolver=media_resolver,
        resolve_timeout=resolve_timeout,
    )
    image_url = str(resolved.get("image_url") or "")
    local_path = str(resolved.get("local_path") or "")
    if not image_url and not local_path:
        raise ImageResourceProviderError(
            "image_resource_resolve_failed",
            f"{name}: resolved image has no usable path or URL",
        )
    image_request_id = (
        f"image-resource-{batch_id}-{index:02d}" if batch_id else f"image-resource-{index:02d}"
    )
    content_hash = _authoritative_image_content_hash(
        provider_hash=str(parsed.get("content_hash") or resolved.get("content_hash") or ""),
        content_bytes=resolved.get("content_bytes"),
        local_path=local_path,
        image_url=image_url,
    )
    if not content_hash:
        raise ImageResourceProviderError(
            "image_content_hash_missing",
            f"{name}: resolved image has no authoritative content hash",
        )
    resource = {
        "image_request_id": image_request_id,
        "resource_ref": str(parsed.get("resource_ref") or source_url or image_request_id),
        "name": name,
        "status": "ready",
        "mode": "text_to_image",
        "prompt_hash": _sha256_resource_value(prompt),
        "content_hash": content_hash,
        "source": "image_resource",
    }
    if image_url:
        resource["image_url"] = image_url
    if local_path:
        resource["local_path"] = local_path
    return resource


def _resolve_image_resource(
    source_url: str,
    *,
    media_resolver: Callable[..., Any] | None,
    resolve_timeout: float,
) -> dict[str, Any]:
    source = str(source_url or "").strip()
    if not source.startswith("fileid://"):
        local_path = _local_path_from_image_location(source)
        return {
            "image_url": "" if local_path else source,
            "local_path": local_path,
        }
    if media_resolver is None:
        raise ImageResourceProviderError(
            "image_resource_resolve_failed",
            "fileid image requires a MediaRegistry resolver",
        )
    file_id = source[len("fileid://"):].strip()
    if not file_id:
        raise ImageResourceProviderError(
            "image_resource_resolve_failed",
            "fileid image reference is empty",
        )
    try:
        try:
            raw = media_resolver(file_id, timeout=float(resolve_timeout))
        except TypeError:
            raw = media_resolver(file_id, float(resolve_timeout))
    except TimeoutError as exc:
        raise ImageResourceProviderError(
            "image_resource_timeout",
            f"media resource {file_id} did not become ready before timeout",
        ) from exc
    except Exception as exc:  # noqa: BLE001
        raise ImageResourceProviderError(
            "image_resource_resolve_failed",
            f"media resource {file_id} could not be resolved",
        ) from exc
    if isinstance(raw, Mapping):
        resolved = dict(raw)
        location = str(
            _first_present(
                resolved.get("local_path"),
                resolved.get("image_url"),
                resolved.get("url"),
                resolved.get("path"),
            )
            or ""
        )
        local_path = str(resolved.get("local_path") or _local_path_from_image_location(location))
        return {
            "image_url": "" if local_path else location,
            "local_path": local_path,
            "content_hash": str(resolved.get("content_hash") or ""),
            "content_bytes": resolved.get("content_bytes"),
        }
    location = str(raw or "").strip()
    if not location:
        raise ImageResourceProviderError(
            "image_resource_resolve_failed",
            f"media resource {file_id} resolved to an empty location",
        )
    local_path = _local_path_from_image_location(location)
    return {
        "image_url": "" if local_path else location,
        "local_path": local_path,
    }


def _local_path_from_image_location(value: str) -> str:
    text = str(value or "").strip()
    if not text:
        return ""
    if text.lower().startswith("file://"):
        parsed = urlparse(text)
        candidate = unquote(parsed.path or "")
        if parsed.netloc:
            candidate = f"//{parsed.netloc}{candidate}"
        if re.fullmatch(r"/[A-Za-z]:/.*", candidate):
            candidate = candidate[1:]
        path = Path(candidate)
        return str(path) if path.is_file() else ""
    path = Path(text)
    return str(path) if path.is_file() else ""


def _data_uri_bytes(value: str) -> bytes | None:
    text = str(value or "").strip()
    if not text.startswith("data:") or "," not in text:
        return None
    header, payload = text.split(",", 1)
    try:
        if ";base64" in header.lower():
            return base64.b64decode(payload, validate=True)
        return unquote(payload).encode("utf-8")
    except (ValueError, TypeError):
        return None


def _authoritative_image_content_hash(
    *,
    provider_hash: str,
    content_bytes: Any,
    local_path: str,
    image_url: str,
) -> str:
    digest = str(provider_hash or "").strip()
    if digest.startswith("sha256:") and len(digest) == 71:
        return digest
    if isinstance(content_bytes, (bytes, bytearray)):
        return "sha256:" + hashlib.sha256(bytes(content_bytes)).hexdigest()
    path = Path(str(local_path or ""))
    if str(local_path or "") and path.is_file():
        try:
            return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
        except OSError:
            return ""
    data = _data_uri_bytes(image_url)
    if data is not None:
        return "sha256:" + hashlib.sha256(data).hexdigest()
    return ""


def _failed_image_resource(
    *,
    name: str,
    batch_id: str,
    index: int,
    failure_code: str = "image_resource_resolve_failed",
    failure_message: str = "",
) -> dict[str, Any]:
    return {
        "image_request_id": f"image-resource-{batch_id}-{index:02d}" if batch_id else f"image-resource-{index:02d}",
        "name": name,
        "status": "failed",
        "source": "image_resource",
        "failure_code": str(failure_code or "image_resource_resolve_failed"),
        "failure_message": str(failure_message or "")[:240],
    }


def _normalize_acquire_result(result: Any, *, name: str, batch_id: str, index: int) -> dict[str, Any]:
    success = _coerce_adapter_bool(_result_value(result, "success", default=True), default=True)
    if not success:
        raise RuntimeError(f"{name}: model acquire failed")

    local_path = str(
        _first_present(
            _result_value(result, "local_path"),
            _result_value(result, "model_path"),
            _result_value(result, "path"),
        )
        or ""
    )
    if not local_path:
        raise RuntimeError(f"{name}: model provider returned no local_path")

    source = _safe_model_resource_source(_result_value(result, "source", default="legacy_model_provider"))
    preview_images = [
        str(item)
        for item in (_result_value(result, "preview_images", default=[]) or [])
        if isinstance(item, str) and item
    ]
    return {
        "model_request_id": f"legacy-model-{batch_id}-{index:02d}" if batch_id else f"legacy-model-{index:02d}",
        "name": name,
        "status": "ready",
        "local_path": local_path,
        "source": source,
        "preview_images": preview_images,
    }


def _normalize_model_tool_result(
    parsed: dict[str, Any],
    *,
    name: str,
    batch_id: str,
    index: int,
    generation_mode: str = "text_to_3d",
    source_image_ref: str = "",
    source_image_hash: str = "",
) -> dict[str, Any]:
    status = str(parsed.get("status") or "").strip().lower()
    success = _coerce_adapter_bool(parsed.get("success"), default=True)
    if status in {"error", "failed", "failure", "fail"} or not success or parsed.get("error"):
        raise RuntimeError(_safe_adapter_error_message(parsed, fallback="model resource failed"))
    metadata = parsed.get("metadata") if isinstance(parsed.get("metadata"), dict) else {}
    local_path = str(
        _first_present(
            parsed.get("local_path"),
            parsed.get("model_path"),
            parsed.get("path"),
            parsed.get("model_folder"),
            parsed.get("model_dir"),
            metadata.get("local_path") if isinstance(metadata, dict) else None,
            metadata.get("model_path") if isinstance(metadata, dict) else None,
            metadata.get("path") if isinstance(metadata, dict) else None,
            metadata.get("model_folder") if isinstance(metadata, dict) else None,
        )
        or ""
    )
    local_path = _resolve_ready_model_local_path(local_path, metadata=metadata)
    if not local_path:
        raise RuntimeError(f"{name}: model tool returned no local_path")
    preview_images = [
        str(item)
        for item in (parsed.get("preview_images") or parsed.get("images") or [])
        if isinstance(item, str) and item
    ]
    model_request_id = (
        f"model-resource-{batch_id}-{index:02d}" if batch_id else f"model-resource-{index:02d}"
    )
    return {
        "model_request_id": model_request_id,
        "model_ref": model_request_id,
        "name": name,
        "status": "ready",
        "local_path": local_path,
        "source": _safe_model_resource_source(parsed.get("source") or "generation"),
        "preview_images": preview_images,
        "generation_mode": generation_mode,
        "source_image_ref": source_image_ref,
        "source_image_hash": source_image_hash,
    }


def _failed_model_resource(
    *,
    name: str,
    batch_id: str,
    index: int,
    source: str = "legacy_model_failure",
    failure_code: str = "legacy_model_failure",
) -> dict[str, Any]:
    return {
        "model_request_id": (
            f"{source}-{batch_id}-{index:02d}" if batch_id else f"{source}-{index:02d}"
        ),
        "name": name,
        "status": "failed",
        "source": _safe_model_resource_source(source),
        "failure_code": _safe_model_failure_code(failure_code),
    }


def _resolve_ready_model_local_path(local_path: str, *, metadata: dict[str, Any]) -> str:
    """Resolve Hunyuan's returned model folder to a concrete mesh when visible.

    Hunyuan may return quickly with ``metadata.model_folder`` while the mesh
    download is still being written in the background.  Actor import can accept
    a directory, but importing before any mesh exists fails.  This adapter waits
    briefly only when the folder is visible from this Python process; otherwise
    it preserves the path for the engine-side active-project resolver.
    """

    text = str(local_path or "").strip()
    if not text:
        return ""
    candidates = _visible_model_path_candidates(text)
    if not candidates:
        return text
    has_mesh_pending = bool(metadata.get("has_mesh_pending")) or str(metadata.get("mesh_download_status") or "").lower() in {
        "scheduled",
        "running",
        "pending",
        "queued",
    }
    wait_seconds = _model_folder_wait_seconds() if has_mesh_pending else 0.0
    deadline = _monotonic_now() + wait_seconds
    while True:
        for candidate in candidates:
            mesh = _first_supported_mesh(candidate)
            if mesh:
                return str(mesh)
        if _monotonic_now() >= deadline:
            break
        _sleep_for_model_folder(0.5)
    # If we can inspect the directory and no mesh exists after the wait window,
    # return empty so the model resource is marked failed rather than pretending
    # a ready model exists.
    return ""


def _visible_model_path_candidates(path_text: str) -> list[Any]:
    from pathlib import Path

    raw = Path(path_text)
    if raw.is_absolute():
        candidates = [raw]
    else:
        try:
            from runtime import project_context

            candidates = [project_context.get_project_root() / raw, raw]
        except Exception:  # standalone tests and non-editor callers
            candidates = [raw]
    visible = []
    for candidate in candidates:
        try:
            if candidate.exists():
                visible.append(candidate)
        except OSError:
            continue
    return visible


def _first_supported_mesh(path: Any) -> Any:
    from pathlib import Path

    supported = {".obj", ".dae", ".glb", ".gltf", ".fbx", ".stl", ".usdz"}
    candidate = Path(path)
    try:
        if candidate.is_file() and candidate.suffix.lower() in supported:
            return candidate
        if candidate.is_dir():
            for child in sorted(candidate.iterdir()):
                if child.is_file() and child.suffix.lower() in supported:
                    return child
            for child in sorted(candidate.rglob("*")):
                try:
                    if child.is_file() and child.suffix.lower() in supported:
                        return child
                except OSError:
                    continue
    except OSError:
        return None
    return None


def _model_folder_wait_seconds() -> float:
    import os

    try:
        return max(0.0, min(90.0, float(os.getenv("AGENT_RUNTIME_MODEL_FOLDER_WAIT_SECONDS", "30"))))
    except (TypeError, ValueError):
        return 30.0


def _monotonic_now() -> float:
    import time

    return time.monotonic()


def _sleep_for_model_folder(seconds: float) -> None:
    import time

    time.sleep(max(0.0, float(seconds)))


def _item_value(payload: dict[str, Any], name: str, key: str) -> Any:
    keyed = payload.get(key)
    if isinstance(keyed, dict):
        return keyed.get(name)
    item_metadata = payload.get("item_metadata")
    if isinstance(item_metadata, dict):
        metadata = item_metadata.get(name)
        if isinstance(metadata, dict):
            return metadata.get(key)
    return None


def _safe_model_resource_source(value: Any) -> str:
    source = str(value or "").strip().lower().replace("-", "_")
    allowed = {
        "generation",
        "generated",
        "generated_model",
        "retrieval",
        "retrieved",
        "cache",
        "local",
        "local_asset",
        "legacy_model",
        "legacy_model_failure",
        "legacy_model_adapter_unavailable",
        "model_resource",
    }
    if source in allowed:
        return source
    return "legacy_model"


def _safe_model_failure_code(value: Any) -> str:
    code = str(value or "").strip().lower().replace("-", "_")
    allowed = {
        "legacy_model_failure",
        "legacy_model_adapter_unavailable",
        "legacy_model_acquire_exception",
        "legacy_model_invalid_result",
        "model_resource_tool_failed",
        "source_image_lineage_missing",
    }
    if code in allowed:
        return code
    return "legacy_model_failure"


def _image_resource_value(payload: dict[str, Any], name: str) -> Any:
    resource = _image_resource_entry(payload, name)
    if not resource:
        return None
    if str(resource.get("status") or "").strip().lower() in {"failed", "failure", "error", "missing"}:
        return None
    return _first_present(
        resource.get("image_url"),
        resource.get("url"),
        resource.get("local_path"),
        resource.get("path"),
    )


def _image_resource_entry(payload: dict[str, Any], name: str) -> dict[str, Any]:
    image_resources = payload.get("image_resources")
    if not isinstance(image_resources, dict):
        return {}
    resource = image_resources.get(name)
    return dict(resource) if isinstance(resource, dict) else {}


def _sha256_resource_value(value: Any) -> str:
    text = str(value or "").strip()
    path = Path(text)
    if text and path.is_file():
        try:
            return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
        except OSError:
            pass
    return "sha256:" + hashlib.sha256(text.encode("utf-8")).hexdigest()


def _result_value(result: Any, key: str, default: Any = None) -> Any:
    if isinstance(result, dict):
        return result.get(key, default)
    return getattr(result, key, default)


def _first_present(*values: Any) -> Any:
    for value in values:
        if value:
            return value
    return None


def _image_prompt_for_item(name: str) -> str:
    return (
        f"single standalone physical 3D object reference image of {name}, "
        "centered full object, plain white background, visible thickness and depth, "
        "no text, no labels, no watermark, not a flat poster, not a texture sheet"
    )
ENGINE_READY_TIMEOUT_DEFAULT_S = 90.0
ENGINE_READY_POLL_DEFAULT_S = 1.0

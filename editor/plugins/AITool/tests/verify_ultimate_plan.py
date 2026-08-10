"""Run the current non-native verification suite for the Agent-native plan.

This runner intentionally avoids C++/Ninja/CEF/F5/native build steps. It is the
repeatable gate for the Python, protocol, and static checks that can be
validated in this workstream. Keep this list aligned with files that exist in
the current Agent-native branch; missing listed files are treated as failures.
"""

from __future__ import annotations

import ast
import os
import subprocess
import sys
import tokenize
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[4]
PYCACHE_PREFIX = REPO_ROOT / ".tmp" / "ultimate_plan_pycache"


PYTHON_TESTS = [
    "editor/plugins/AITool/services/tests/test_agent_runtime_phase1.py",
    "editor/plugins/AITool/services/tests/test_lanchat_runtime_guard.py",
    "editor/plugins/AITool/services/tests/test_model_retrieval_provider_helpers.py",
    "editor/plugins/AITool/cai_extensions/mcp/tools/tests/test_set_actor_transform_tool.py",
    "docs/probes/test_v3_f5_log_check.py",
    "docs/probes/test_v3_f5_quick_gate.py",
]

NODE_TESTS: list[str] = []

PY_COMPILE_TARGETS = [
    "editor/plugins/AITool/services/agent_runtime/__init__.py",
    "editor/plugins/AITool/services/agent_runtime/adapters.py",
    "editor/plugins/AITool/services/agent_runtime/core.py",
    "editor/plugins/AITool/services/agent_runtime/flags.py",
    "editor/plugins/AITool/services/agent_runtime/tools.py",
    "editor/plugins/AITool/services/generation_composer_adapter.py",
    "editor/plugins/AITool/services/generation_scheduler.py",
    "editor/plugins/AITool/services/interaction_coordinator.py",
    "editor/plugins/AITool/services/intent_understanding.py",
    "editor/plugins/AITool/services/lanchat_agent_worker.py",
    "editor/plugins/AITool/services/lanchat_host_action_executor.py",
    "editor/plugins/AITool/services/lanchat_scene_runtime.py",
    "editor/plugins/AITool/services/runtime_query_policy.py",
    "editor/plugins/AITool/services/runtime_result_policy.py",
    "editor/plugins/AITool/services/runtime_report_policy.py",
    "editor/plugins/AITool/services/runtime_replay_report_policy.py",
    "editor/plugins/AITool/services/runtime_replay_lifecycle_policy.py",
    "editor/plugins/AITool/services/runtime_replay_event_policy.py",
    "editor/plugins/AITool/services/runtime_replay_detail_policy.py",
    "editor/plugins/AITool/services/runtime_replay_resource_policy.py",
    "editor/plugins/AITool/services/runtime_replay_transfer_policy.py",
    "editor/plugins/AITool/services/runtime_replay_peer_sync_policy.py",
    "editor/plugins/AITool/services/runtime_sync_policy.py",
    "editor/plugins/AITool/services/runtime_replay_sync_policy.py",
    "editor/plugins/AITool/services/runtime_message_delivery_policy.py",
    "editor/plugins/AITool/services/seed_plan.py",
    "editor/plugins/AITool/services/workflow_command_policy.py",
    "editor/plugins/AITool/services/tests/test_agent_runtime_phase1.py",
    "editor/plugins/AITool/services/tests/test_lanchat_runtime_guard.py",
    "editor/plugins/AITool/services/tests/test_model_retrieval_provider_helpers.py",
    "editor/plugins/AITool/cai_extensions/register.py",
    "editor/plugins/AITool/cai_extensions/flows/model_retrieval_workflow/helpers.py",
    "editor/plugins/AITool/cai_extensions/agent/agent_adapter.py",
    "editor/plugins/AITool/cai_extensions/agent/engine_write_gate.py",
    "editor/plugins/AITool/cai_extensions/agent/scene_composer.py",
    "editor/plugins/AITool/cai_extensions/agent/scene_composer_progressive.py",
    "editor/plugins/AITool/cai_extensions/agent/scene_element_classifier.py",
    "editor/plugins/AITool/cai_extensions/mcp/tools/model_import_tools.py",
    "editor/plugins/AITool/cai_extensions/mcp/tools/set_actor_transform.py",
    "editor/plugins/AITool/cai_extensions/mcp/tools/tests/test_set_actor_transform_tool.py",
    "docs/probes/v3_f5_log_check.py",
    "docs/probes/v3_f5_quick_gate.py",
]

DIRECT_SCENE_COMPOSE_SCAN_ROOTS = [
    "editor/plugins/AITool/services",
    "editor/plugins/AITool/cai_extensions/agent",
    "editor/plugins/AITool/main.py",
]

DIRECT_SCENE_COMPOSE_ALLOWED_FILES = {
    "editor/plugins/AITool/main.py",
    "editor/plugins/AITool/cai_extensions/agent/agent_adapter.py",
    "editor/plugins/AITool/services/generation_composer_adapter.py",
}

DIRECT_SCENE_COMPOSE_GUARDS = {
    "editor/plugins/AITool/cai_extensions/agent/agent_adapter.py": (
        "_legacy_main_workflow_allowed",
        "AGENT_RUNTIME_REQUIRED_MESSAGE",
    ),
    "editor/plugins/AITool/services/generation_composer_adapter.py": (
        "can_call_legacy_main_workflow",
        "legacy SceneComposer main workflow is disabled",
    ),
}

DIRECT_SCENE_COMPOSE_ALLOWED_LINE_PATTERNS = {
    "editor/plugins/AITool/main.py": (
        'return SceneComposer(scene_name="Scene/default.scene")',
    ),
    "editor/plugins/AITool/cai_extensions/agent/agent_adapter.py": (
        "composer = SceneComposer(",
        "result = composer.compose(",
    ),
    "editor/plugins/AITool/services/generation_composer_adapter.py": (
        "SceneComposer.compose().",
        "result = composer.compose(",
    ),
}

DIRECT_SCENE_COMPOSE_GUARDED_CALLS = {
    "editor/plugins/AITool/cai_extensions/agent/agent_adapter.py": [
        ("def _handle_scene_compose(", "composer = SceneComposer("),
        ("def _handle_scene_compose(", "result = composer.compose("),
    ],
    "editor/plugins/AITool/services/generation_composer_adapter.py": [
        ("def compose(", "result = composer.compose("),
    ],
}

MASTER_AGENT_LEGACY_COMPOSE_ROUTE_GUARDED_CALLS = {
    "editor/plugins/AITool/cai_extensions/agent/agent_adapter.py": [
        ("if gate_action == \"compose\":", "return self._handle_scene(str(gate_payload or trigger)"),
        ("if intent_class == \"compose\":", "return self._handle_scene(trigger, system, messages, force_compose=True)"),
    ],
}

DIRECT_ENGINE_WRITE_SCAN_ROOTS = [
    "editor/plugins/AITool/cai_extensions/agent/agent_adapter.py",
]

DIRECT_ENGINE_WRITE_GUARDED_CALLS = {
    "editor/plugins/AITool/cai_extensions/agent/agent_adapter.py": [
        ("def _handle_direct_import(", 'get_tool("import_model")'),
        ("def _handle_edit(", "scene_manager.get"),
    ],
}

RUNTIME_ADAPTER_ENGINE_WRITE_BOUNDARY_FILE = (
    "editor/plugins/AITool/services/agent_runtime/adapters.py"
)

RUNTIME_ADAPTER_ENGINE_WRITE_REQUIRED_MARKERS = (
    "class RuntimeCppBridge",
    "engine_gate is required",
    "raw = invoke_tool(tool, payload)",
    "raw = set_transform(tool, payload)",
    "raw = remove_actor(tool, payload)",
    "make_engine_actor_import_provider",
    "bridge.invoke_tool(import_tool",
    "make_engine_environment_component_import_provider",
    "environment_import_tool,",
    "cpp_environment_component_import_failed",
    "make_engine_layout_transform_provider",
    "bridge.set_transform(transform_tool",
    "make_engine_actor_delete_provider",
    "bridge.remove_actor(delete_tool",
)

RUNTIME_ADAPTER_FORBIDDEN_DIRECT_WRITE_MARKERS = (
    ".invoke(",
    "CoronaEditor.CoronaEngine",
    "create_editor_actor(",
    "set_editor_actor_transform(",
    "remove_editor_actor(",
    "get_tool_registry(",
    "get_tool(",
)

DIRECT_PROGRESSIVE_WORKFLOW_SCAN_ROOTS = [
    "editor/plugins/AITool/services",
    "editor/plugins/AITool/cai_extensions/agent",
    "editor/plugins/AITool/main.py",
]

DIRECT_GENERATION_SCHEDULER_SCAN_ROOTS = [
    "editor/plugins/AITool/services",
    "editor/plugins/AITool/cai_extensions/agent",
    "editor/plugins/AITool/main.py",
]

DIRECT_HOST_ACTION_EXECUTOR_SCAN_ROOTS = [
    "editor/plugins/AITool/services",
    "editor/plugins/AITool/cai_extensions/agent",
    "editor/plugins/AITool/main.py",
]

DIRECT_PROGRESSIVE_WORKFLOW_ALLOWED_FILES = {
    "editor/plugins/AITool/cai_extensions/agent/scene_composer.py",
    "editor/plugins/AITool/cai_extensions/agent/scene_composer_progressive.py",
    "editor/plugins/AITool/cai_extensions/agent/scene_session.py",
}

DIRECT_PROGRESSIVE_WORKFLOW_ALLOWED_LINE_PATTERNS = {
    "editor/plugins/AITool/cai_extensions/agent/scene_composer.py": (
        "from .scene_composer_progressive import run_progressive_workflow",
        "result = run_progressive_workflow(",
    ),
    "editor/plugins/AITool/cai_extensions/agent/scene_composer_progressive.py": (
        "鏈ā鍧楁彁渚?`_run_progressive_workflow`",
        "def run_progressive_workflow(",
        "prog_result = session.progressive_compose(",
        '__all__ = ["run_progressive_workflow"]',
    ),
    "editor/plugins/AITool/cai_extensions/agent/scene_session.py": (
        "progressive_compose() 鏄富寰幆",
        "def progressive_compose(",
    ),
}

DIRECT_PROGRESSIVE_WORKFLOW_CONTAINED_CALLS = {
    "editor/plugins/AITool/cai_extensions/agent/scene_composer.py": [
        ("    def compose(", "from .scene_composer_progressive import run_progressive_workflow"),
        ("    def compose(", "result = run_progressive_workflow("),
    ],
    "editor/plugins/AITool/cai_extensions/agent/scene_composer_progressive.py": [
        ("def run_progressive_workflow(", "prog_result = session.progressive_compose("),
    ],
}

DIRECT_PROGRESSIVE_WORKFLOW_REQUIRED_SCOPE_TOKENS = {
    "editor/plugins/AITool/cai_extensions/agent/scene_composer.py": [
        (
            "    def compose(",
            (
                "Agent-native migration: compose() no longer honors",
                "from .scene_composer_progressive import run_progressive_workflow",
                "result = run_progressive_workflow(",
            ),
        ),
    ],
    "editor/plugins/AITool/cai_extensions/agent/scene_composer_progressive.py": [
        (
            "def run_progressive_workflow(",
            (
                "from .engine_write_gate import get_engine_write_gate",
                "engine_gate = get_engine_write_gate()",
                "def importer(",
                "incremental_import(",
                "import_tool=import_tool,\n            scene_layout=scene_layout,\n            engine_gate=engine_gate",
                "session.progressive_compose(",
            ),
        ),
    ],
}

DIRECT_PROGRESSIVE_WORKFLOW_FORBIDDEN_SCOPE_TOKENS = {
    "editor/plugins/AITool/cai_extensions/agent/scene_composer.py": [
        (
            "    def compose(",
            (
                "USE_PROGRESSIVE_COMPOSE",
                "self._run_original_workflow(",
            ),
        ),
    ],
}

DIRECT_GENERATION_SCHEDULER_ALLOWED_FILES = {
    "editor/plugins/AITool/services/interaction_coordinator.py",
    "editor/plugins/AITool/services/lanchat_agent_worker.py",
}

DIRECT_GENERATION_SCHEDULER_ALLOWED_LINE_PATTERNS = {
    "editor/plugins/AITool/services/interaction_coordinator.py": (
        "submitted = self._scheduler.submit(payload)",
        "submitted = self._scheduler.submit(job_payload)",
    ),
    "editor/plugins/AITool/services/lanchat_agent_worker.py": (
        "self._generation_scheduler = GenerationScheduler(",
        "ref = coordinator.execute_confirmed_plan(plan.plan_id)",
    ),
}

DIRECT_GENERATION_SCHEDULER_CONTAINED_CALLS = {
    "editor/plugins/AITool/services/interaction_coordinator.py": [
        ("    def execute_confirmed_plan(", "submitted = self._scheduler.submit(payload)"),
        ("    def execute_post_generation_add(", "submitted = self._scheduler.submit(job_payload)"),
    ],
    "editor/plugins/AITool/services/lanchat_agent_worker.py": [
        ("    def _start_active_coordinator_generation(", "ref = coordinator.execute_confirmed_plan(plan.plan_id)"),
    ],
}

DIRECT_GENERATION_SCHEDULER_REQUIRED_SCOPE_TOKENS = {
    "editor/plugins/AITool/services/lanchat_agent_worker.py": [
        (
            "    def _get_generation_scheduler(",
            (
                "can_call_legacy_main_workflow()",
                "from .generation_scheduler import GenerationScheduler",
                "self._generation_scheduler = GenerationScheduler(",
                "self._install_generation_scheduler_hooks(self._generation_scheduler)",
            ),
        ),
        (
            "    def _start_active_coordinator_generation(",
            (
                "if not self._agent_runtime_flags.can_call_legacy_main_workflow():",
                "return self._execute_confirmed_plan_via_agent_runtime(",
                "ref = coordinator.execute_confirmed_plan(plan.plan_id)",
            ),
        ),
    ],
}

DIRECT_HOST_ACTION_EXECUTOR_ALLOWED_FILES = {
    "editor/plugins/AITool/services/lanchat_agent_worker.py",
}

DIRECT_HOST_ACTION_EXECUTOR_ALLOWED_LINE_PATTERNS = {
    "editor/plugins/AITool/services/lanchat_agent_worker.py": (
        "self._execute_confirmed_action(payload)",
        "executor.enqueue_and_process(payload)",
    ),
}

DIRECT_HOST_ACTION_EXECUTOR_REQUIRED_SCOPE_TOKENS = {
    "editor/plugins/AITool/services/lanchat_agent_worker.py": [
        (
            "    def _broadcast_confirmed_action(",
            (
                "def _broadcast_confirmed_action(",
                "if not self._is_confirmed_action_payload_runtime_approved(payload):",
                "self._record_unapproved_confirmed_action_block(payload, phase=\"broadcast\")",
                "Blocked unapproved confirmed action payload",
                "self._execute_confirmed_action(payload)",
            ),
        ),
        (
            "    def _broadcast_confirmed_action(",
            (
                "if not self._is_confirmed_action_payload_runtime_approved(payload):",
                "return\n        if hasattr(self._corona_engine, \"network_broadcast_intent\"):",
                "self._execute_confirmed_action(payload)",
            ),
        ),
        (
            "    def _filter_confirmed_action_payload_for_runtime(",
            (
                "def _filter_confirmed_action_payload_for_runtime(",
                "if self._is_confirmed_action_payload_runtime_approved(payload):",
                "self._record_unapproved_confirmed_action_block(payload, phase=\"reply_metadata\")",
                "return None",
            ),
        ),
        (
            "    def _is_confirmed_action_payload_runtime_approved(",
            (
                "def _is_confirmed_action_payload_runtime_approved(",
                "if self._agent_runtime_flags.can_call_legacy_main_workflow():",
                "execution not in {\"agent_runtime_structured\", \"coordinator_structured\"}",
                "return bool(payload.get(\"runtime_payload_prepared_by_worker\"))",
            ),
        ),
        (
            "    def _execute_confirmed_action(",
            (
                "def _execute_confirmed_action(",
                "executor = self._get_host_action_executor()",
                "executor.enqueue_and_process(payload)",
                "self._emit_generation_scheduler_disclosure()",
            ),
        ),
    ],
}

REQUIRED_DEPRECATED_WORKFLOW_COMMANDS = {
    "/scene_agent",
    "/sc_agent",
    "/scene_composition",
    "/scene_composition_v2",
    "/sc_v2",
    "/full_pipeline",
    "/pipeline",
    "/full_pipeline_v2",
    "/fp_v2",
    "/multi_scene",
    "/parallel_generate",
    "/parallel_generate_v2",
    "/pg_v2",
}

REQUIRED_INTERNAL_WORKFLOW_COMMANDS = {
    "/model_retrieval",
    "/terrain_generate",
    "/terrain",
}

WORKFLOW_COMMAND_SCAN_ROOTS = [
    "editor/plugins/AITool/cai_extensions/agent/__init__.py",
    "editor/plugins/AITool/cai_extensions/flows",
]

AGENT_RUNTIME_CORE = "editor/plugins/AITool/services/agent_runtime/core.py"
AGENT_RUNTIME_TOOLS = "editor/plugins/AITool/services/agent_runtime/tools.py"
AGENT_RUNTIME_ADAPTERS = "editor/plugins/AITool/services/agent_runtime/adapters.py"
AGENT_RUNTIME_FLAGS = "editor/plugins/AITool/services/agent_runtime/flags.py"
GENERATION_COMPOSER_ADAPTER = "editor/plugins/AITool/services/generation_composer_adapter.py"
LANCHAT_AGENT_WORKER = "editor/plugins/AITool/services/lanchat_agent_worker.py"
LANCHAT_HOST_ACTION_EXECUTOR = "editor/plugins/AITool/services/lanchat_host_action_executor.py"
LEGACY_AGENT_COORDINATOR = "editor/plugins/AITool/cai_extensions/agent/coordinator.py"
AGENT_RUNTIME_PHASE1_TESTS = "editor/plugins/AITool/services/tests/test_agent_runtime_phase1.py"
LANCHAT_RUNTIME_GUARD_TESTS = "editor/plugins/AITool/services/tests/test_lanchat_runtime_guard.py"
SCENE_ELEMENT_CLASSIFIER = "editor/plugins/AITool/cai_extensions/agent/scene_element_classifier.py"

REQUIRED_RUNTIME_VALIDATORS = {
    "ScenePlanValidator",
    "BatchPlanValidator",
    "PlanPatchValidator",
    "StatePatchValidator",
    "ToolCallValidator",
    "ToolResultValidator",
    "ToolCallGraphValidator",
    "AdjustmentProposalValidator",
    "ReviewAdvisoryProposalValidator",
    "ReportRecordValidator",
}

REQUIRED_STATE_PATCH_CONFLICT_TESTS = (
    "test_executor_preserves_explicit_state_patch_expected_version_conflict",
    "test_state_patch_conflict_is_visible_as_reconcile_fact_in_status_and_report",
    "test_state_patch_conflict_reconcile_action_records_decision_without_replaying_patch",
    "test_state_patch_conflict_does_not_emit_result_when_failed_state_persist_fails",
    "test_state_patch_validator_rejects_invalid_operations_schema",
    "test_state_patch_validator_protects_runtime_owned_control_slots",
)

REQUIRED_PHASE6_GEOMETRY_TOOL_TESTS = (
    "test_phase6_geometry_compute_aabb_tool_records_safe_actor_facts",
    "test_phase6_geometry_check_overlap_tool_records_safe_review_fact_without_actor_write",
)

ALLOWED_RUNTIME_STATE_APPLY_PATCH_FUNCTIONS = {
    "execute",
    "_emit_tool_started_runtime_event",
    "_emit_tool_result_runtime_event",
    "_emit_tool_blocked_runtime_event",
    "_emit_graph_stopped_runtime_event",
    "_persist_graph",
}


def _run(label: str, command: list[str]) -> bool:
    print(f"[RUN] {label}")
    env = os.environ.copy()
    if command and Path(command[0]).name.lower().startswith("python"):
        PYCACHE_PREFIX.mkdir(parents=True, exist_ok=True)
        env["PYTHONPYCACHEPREFIX"] = str(PYCACHE_PREFIX)
        env.setdefault("AGENT_RUNTIME_ENGINE_READY_TIMEOUT_S", "0.2")
        env.setdefault("AGENT_RUNTIME_ENGINE_READY_POLL_S", "0.05")
    completed = subprocess.run(command, cwd=REPO_ROOT, env=env)
    if completed.returncode == 0:
        print(f"[OK]  {label}")
        return True
    print(f"[FAIL] {label} (exit={completed.returncode})")
    return False


def _syntax_check(paths: list[str]) -> bool:
    print("[RUN] syntax compile current Agent-native modules")
    for path in paths:
        source_path = REPO_ROOT / path
        try:
            with tokenize.open(source_path) as handle:
                source = handle.read()
            compile(source, str(source_path), "exec")
        except Exception as exc:
            print(f"[FAIL] syntax compile current Agent-native modules: {path}: {exc}")
            return False
    print("[OK]  syntax compile current Agent-native modules")
    return True


def _to_repo_path(path: Path) -> str:
    return path.relative_to(REPO_ROOT).as_posix()

def _safe_console_text(value: object) -> str:
    text = str(value)
    try:
        text.encode(sys.stdout.encoding or "utf-8")
        return text
    except UnicodeEncodeError:
        return text.encode(sys.stdout.encoding or "utf-8", errors="backslashreplace").decode(
            sys.stdout.encoding or "utf-8",
            errors="replace",
        )


def _print_violation(item: object) -> None:
    print(f"       {_safe_console_text(item)}")



def _should_skip_direct_scene_compose_scan(path: Path) -> bool:
    parts = set(path.relative_to(REPO_ROOT).parts)
    if "Quasar" in parts or "__pycache__" in parts or ".tmp" in parts:
        return True
    if path.name == "verify_ultimate_plan.py":
        return True
    if "tests" in parts or path.name.startswith("test_"):
        return True
    if path.name == "scene_composer.py":
        return True
    return path.suffix != ".py"


def _iter_direct_scene_compose_scan_files() -> list[Path]:
    files: list[Path] = []
    for root in DIRECT_SCENE_COMPOSE_SCAN_ROOTS:
        root_path = REPO_ROOT / root
        if root_path.is_file():
            if not _should_skip_direct_scene_compose_scan(root_path):
                files.append(root_path)
            continue
        if root_path.is_dir():
            for path in root_path.rglob("*.py"):
                if not _should_skip_direct_scene_compose_scan(path):
                    files.append(path)
    return sorted(set(files))


def _direct_scene_compose_entry_gate() -> bool:
    print("[RUN] static direct SceneComposer entry gate")
    violations: list[str] = []
    for path in _iter_direct_scene_compose_scan_files():
        rel = _to_repo_path(path)
        try:
            source = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            source = path.read_text(encoding="utf-8-sig")
        interesting_lines = []
        for lineno, line in enumerate(source.splitlines(), start=1):
            if "SceneComposer(" in line or "composer.compose(" in line:
                interesting_lines.append((lineno, line.strip()))
        if not interesting_lines:
            continue
        if rel not in DIRECT_SCENE_COMPOSE_ALLOWED_FILES:
            for lineno, line in interesting_lines:
                violations.append(f"{rel}:{lineno}: unexpected direct SceneComposer entry: {line}")
            continue
        allowed_patterns = DIRECT_SCENE_COMPOSE_ALLOWED_LINE_PATTERNS.get(rel, ())
        for lineno, line in interesting_lines:
            if not any(pattern in line for pattern in allowed_patterns):
                violations.append(f"{rel}:{lineno}: unexpected direct SceneComposer entry: {line}")
        guard_tokens = DIRECT_SCENE_COMPOSE_GUARDS.get(rel, ())
        if guard_tokens and not all(token in source for token in guard_tokens):
            violations.append(
                f"{rel}: allowed legacy SceneComposer entry is missing Runtime guard tokens: "
                + ", ".join(guard_tokens)
            )
        for entry_marker, compose_marker in DIRECT_SCENE_COMPOSE_GUARDED_CALLS.get(rel, []):
            try:
                entry_index = source.index(entry_marker)
            except ValueError:
                violations.append(f"{rel}: missing guarded SceneComposer entry marker {entry_marker!r}")
                continue
            try:
                compose_index = source.index(compose_marker, entry_index)
            except ValueError:
                violations.append(f"{rel}: missing SceneComposer call marker {compose_marker!r}")
                continue
            guarded_prefix = source[entry_index:compose_index]
            for token in guard_tokens:
                if token not in guarded_prefix:
                    violations.append(
                        f"{rel}: {entry_marker} reaches {compose_marker!r} without guard token {token!r}"
                    )
    if violations:
        print("[FAIL] static direct SceneComposer entry gate")
        for item in violations:
            _print_violation(item)
        return False
    print("[OK]  static direct SceneComposer entry gate")
    return True


def _direct_engine_write_entry_gate() -> bool:
    print("[RUN] static direct engine-write entry gate")
    violations: list[str] = []
    for rel in DIRECT_ENGINE_WRITE_SCAN_ROOTS:
        path = REPO_ROOT / rel
        try:
            source = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            source = path.read_text(encoding="utf-8-sig")
        for entry_marker, write_marker in DIRECT_ENGINE_WRITE_GUARDED_CALLS.get(rel, []):
            try:
                entry_index = source.index(entry_marker)
            except ValueError:
                violations.append(f"{rel}: missing guarded entry marker {entry_marker!r}")
                continue
            try:
                write_index = source.index(write_marker, entry_index)
            except ValueError:
                violations.append(f"{rel}: missing direct write marker {write_marker!r}")
                continue
            guarded_prefix = source[entry_index:write_index]
            if "_legacy_main_workflow_allowed()" not in guarded_prefix:
                violations.append(f"{rel}: {entry_marker} reaches {write_marker!r} without legacy-main guard")
            if "AGENT_RUNTIME_REQUIRED_MESSAGE" not in guarded_prefix:
                violations.append(f"{rel}: {entry_marker} reaches {write_marker!r} without Runtime-required reply")
    if violations:
        print("[FAIL] static direct engine-write entry gate")
        for item in violations:
            _print_violation(item)
        return False
    print("[OK]  static direct engine-write entry gate")
    return True


def _runtime_adapter_engine_write_boundary_gate() -> bool:
    print("[RUN] static Runtime adapter engine-write boundary gate")
    violations: list[str] = []
    rel = RUNTIME_ADAPTER_ENGINE_WRITE_BOUNDARY_FILE
    path = REPO_ROOT / rel
    try:
        source = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        source = path.read_text(encoding="utf-8-sig")
    except FileNotFoundError:
        violations.append(f"{rel}: missing Runtime adapter file")
        source = ""

    for marker in RUNTIME_ADAPTER_ENGINE_WRITE_REQUIRED_MARKERS:
        if marker not in source:
            violations.append(f"{rel}: missing Runtime bridge boundary marker {marker!r}")

    for lineno, line in enumerate(source.splitlines(), start=1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        for marker in RUNTIME_ADAPTER_FORBIDDEN_DIRECT_WRITE_MARKERS:
            if marker in stripped:
                violations.append(
                    f"{rel}:{lineno}: Runtime adapter bypasses EngineWriteGate with {marker!r}"
                )

    if violations:
        print("[FAIL] static Runtime adapter engine-write boundary gate")
        for item in violations:
            _print_violation(item)
        return False
    print("[OK]  static Runtime adapter engine-write boundary gate")
    return True


def _master_agent_legacy_compose_route_gate() -> bool:
    print("[RUN] static MasterAgent legacy compose route gate")
    violations: list[str] = []
    for rel, calls in MASTER_AGENT_LEGACY_COMPOSE_ROUTE_GUARDED_CALLS.items():
        path = REPO_ROOT / rel
        try:
            source = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            source = path.read_text(encoding="utf-8-sig")
        for entry_marker, route_marker in calls:
            try:
                entry_index = source.index(entry_marker)
            except ValueError:
                violations.append(f"{rel}: missing MasterAgent route marker {entry_marker!r}")
                continue
            try:
                route_index = source.index(route_marker, entry_index)
            except ValueError:
                violations.append(f"{rel}: missing legacy scene route marker {route_marker!r}")
                continue
            guarded_prefix = source[entry_index:route_index]
            if "_legacy_main_workflow_allowed()" not in guarded_prefix:
                violations.append(f"{rel}: {entry_marker} can reach legacy scene handler without legacy-main guard")
            if "AGENT_RUNTIME_REQUIRED_MESSAGE" not in guarded_prefix:
                violations.append(f"{rel}: {entry_marker} can reach legacy scene handler without Runtime-required reply")
    if violations:
        print("[FAIL] static MasterAgent legacy compose route gate")
        for item in violations:
            _print_violation(item)
        return False
    print("[OK]  static MasterAgent legacy compose route gate")
    return True


def _should_skip_direct_progressive_workflow_scan(path: Path) -> bool:
    parts = set(path.relative_to(REPO_ROOT).parts)
    if "Quasar" in parts or "__pycache__" in parts or ".tmp" in parts:
        return True
    if path.name == "verify_ultimate_plan.py":
        return True
    if "tests" in parts or path.name.startswith("test_"):
        return True
    return path.suffix != ".py"


def _iter_direct_progressive_workflow_scan_files() -> list[Path]:
    files: list[Path] = []
    for root in DIRECT_PROGRESSIVE_WORKFLOW_SCAN_ROOTS:
        root_path = REPO_ROOT / root
        if root_path.is_file():
            if not _should_skip_direct_progressive_workflow_scan(root_path):
                files.append(root_path)
            continue
        if root_path.is_dir():
            for path in root_path.rglob("*.py"):
                if not _should_skip_direct_progressive_workflow_scan(path):
                    files.append(path)
    return sorted(set(files))


def _function_scope(source: str, entry_marker: str) -> tuple[str, list[str]]:
    try:
        entry_index = source.index(entry_marker)
    except ValueError:
        return "", [f"missing entry marker {entry_marker!r}"]
    line_start = source.rfind("\n", 0, entry_index) + 1
    entry_line = source[line_start:source.find("\n", line_start)]
    indent = len(entry_line) - len(entry_line.lstrip())
    sibling_marker = "\n" + (" " * indent) + "def "
    scope_end = source.find(sibling_marker, entry_index + len(entry_marker))
    if scope_end < 0:
        scope_end = len(source)
    return source[entry_index:scope_end], []


def _direct_progressive_workflow_entry_gate() -> bool:
    print("[RUN] static direct ProgressiveWorkflow entry gate")
    violations: list[str] = []
    markers = (
        "run_progressive_workflow(",
        "progressive_compose(",
    )
    for path in _iter_direct_progressive_workflow_scan_files():
        rel = _to_repo_path(path)
        try:
            source = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            source = path.read_text(encoding="utf-8-sig")
        interesting_lines = []
        for lineno, line in enumerate(source.splitlines(), start=1):
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if stripped.startswith(("\"", "'")):
                continue
            if "progressive_compose()" in stripped:
                continue
            if any(marker in stripped for marker in markers):
                interesting_lines.append((lineno, stripped))
        if not interesting_lines:
            continue
        for lineno, line in interesting_lines:
            allowed_patterns = DIRECT_PROGRESSIVE_WORKFLOW_ALLOWED_LINE_PATTERNS.get(rel, ())
            if rel in DIRECT_PROGRESSIVE_WORKFLOW_ALLOWED_FILES and any(
                pattern in line for pattern in allowed_patterns
            ):
                continue
            violations.append(f"{rel}:{lineno}: unexpected direct ProgressiveWorkflow entry: {line}")
        for entry_marker, workflow_marker in DIRECT_PROGRESSIVE_WORKFLOW_CONTAINED_CALLS.get(rel, []):
            scope, scope_errors = _function_scope(source, entry_marker)
            if scope_errors:
                violations.extend(f"{rel}: {item}" for item in scope_errors)
                continue
            try:
                workflow_index = scope.index(workflow_marker)
            except ValueError:
                violations.append(f"{rel}: missing ProgressiveWorkflow call marker {workflow_marker!r}")
                continue
            if workflow_index < 0:
                violations.append(f"{rel}: {workflow_marker!r} is not contained in expected entry {entry_marker!r}")
        for entry_marker, required_tokens in DIRECT_PROGRESSIVE_WORKFLOW_REQUIRED_SCOPE_TOKENS.get(rel, []):
            scope, scope_errors = _function_scope(source, entry_marker)
            if scope_errors:
                violations.extend(f"{rel}: {item}" for item in scope_errors)
                continue
            last_index = -1
            for token in required_tokens:
                token_index = scope.find(token)
                if token_index < 0:
                    violations.append(f"{rel}: {entry_marker} scope missing required token {token!r}")
                    continue
                if token_index < last_index:
                    violations.append(
                        f"{rel}: {entry_marker} scope has required token {token!r} out of execution order"
                    )
                last_index = token_index
        for entry_marker, forbidden_tokens in DIRECT_PROGRESSIVE_WORKFLOW_FORBIDDEN_SCOPE_TOKENS.get(rel, []):
            scope, scope_errors = _function_scope(source, entry_marker)
            if scope_errors:
                violations.extend(f"{rel}: {item}" for item in scope_errors)
                continue
            for token in forbidden_tokens:
                if token in scope:
                    violations.append(f"{rel}: {entry_marker} scope must not contain legacy token {token!r}")
    if violations:
        print("[FAIL] static direct ProgressiveWorkflow entry gate")
        for item in violations:
            _print_violation(item)
        return False
    print("[OK]  static direct ProgressiveWorkflow entry gate")
    return True


def _should_skip_direct_generation_scheduler_scan(path: Path) -> bool:
    parts = set(path.relative_to(REPO_ROOT).parts)
    if "Quasar" in parts or "__pycache__" in parts or ".tmp" in parts:
        return True
    if path.name in {"verify_ultimate_plan.py", "generation_scheduler.py"}:
        return True
    if "tests" in parts or path.name.startswith("test_"):
        return True
    return path.suffix != ".py"


def _iter_direct_generation_scheduler_scan_files() -> list[Path]:
    files: list[Path] = []
    for root in DIRECT_GENERATION_SCHEDULER_SCAN_ROOTS:
        root_path = REPO_ROOT / root
        if root_path.is_file():
            if not _should_skip_direct_generation_scheduler_scan(root_path):
                files.append(root_path)
            continue
        if root_path.is_dir():
            for path in root_path.rglob("*.py"):
                if not _should_skip_direct_generation_scheduler_scan(path):
                    files.append(path)
    return sorted(set(files))


def _direct_generation_scheduler_entry_gate() -> bool:
    print("[RUN] static direct GenerationScheduler entry gate")
    violations: list[str] = []
    markers = (
        "GenerationScheduler(",
        "_scheduler.submit(",
    )
    for path in _iter_direct_generation_scheduler_scan_files():
        rel = _to_repo_path(path)
        try:
            source = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            source = path.read_text(encoding="utf-8-sig")
        interesting_lines = []
        for lineno, line in enumerate(source.splitlines(), start=1):
            stripped = line.strip()
            if not stripped or stripped.startswith("#") or stripped.startswith(("\"", "'")):
                continue
            if stripped.startswith("def "):
                continue
            if any(marker in stripped for marker in markers):
                interesting_lines.append((lineno, stripped))
        if not interesting_lines:
            continue
        allowed_patterns = DIRECT_GENERATION_SCHEDULER_ALLOWED_LINE_PATTERNS.get(rel, ())
        for lineno, line in interesting_lines:
            if rel in DIRECT_GENERATION_SCHEDULER_ALLOWED_FILES and any(
                pattern in line for pattern in allowed_patterns
            ):
                continue
            violations.append(f"{rel}:{lineno}: unexpected direct GenerationScheduler entry: {line}")
        for entry_marker, submit_marker in DIRECT_GENERATION_SCHEDULER_CONTAINED_CALLS.get(rel, []):
            scope, scope_errors = _function_scope(source, entry_marker)
            if scope_errors:
                violations.extend(f"{rel}: {item}" for item in scope_errors)
                continue
            if submit_marker not in scope:
                violations.append(
                    f"{rel}: {submit_marker!r} is not contained in expected entry {entry_marker!r}"
                )
        for entry_marker, required_tokens in DIRECT_GENERATION_SCHEDULER_REQUIRED_SCOPE_TOKENS.get(rel, []):
            scope, scope_errors = _function_scope(source, entry_marker)
            if scope_errors:
                violations.extend(f"{rel}: {item}" for item in scope_errors)
                continue
            last_index = -1
            for token in required_tokens:
                token_index = scope.find(token)
                if token_index < 0:
                    violations.append(f"{rel}: {entry_marker} scope missing required token {token!r}")
                    continue
                if token_index < last_index:
                    violations.append(
                        f"{rel}: {entry_marker} scope has required token {token!r} out of execution order"
                    )
                last_index = token_index
    if violations:
        print("[FAIL] static direct GenerationScheduler entry gate")
        for item in violations:
            _print_violation(item)
        return False
    print("[OK]  static direct GenerationScheduler entry gate")
    return True


def _should_skip_direct_host_action_executor_scan(path: Path) -> bool:
    parts = set(path.relative_to(REPO_ROOT).parts)
    if "Quasar" in parts or "__pycache__" in parts or ".tmp" in parts:
        return True
    if path.name in {"verify_ultimate_plan.py", "lanchat_host_action_executor.py"}:
        return True
    if "tests" in parts or path.name.startswith("test_"):
        return True
    return path.suffix != ".py"


def _iter_direct_host_action_executor_scan_files() -> list[Path]:
    files: list[Path] = []
    for root in DIRECT_HOST_ACTION_EXECUTOR_SCAN_ROOTS:
        root_path = REPO_ROOT / root
        if root_path.is_file():
            if not _should_skip_direct_host_action_executor_scan(root_path):
                files.append(root_path)
            continue
        if root_path.is_dir():
            for path in root_path.rglob("*.py"):
                if not _should_skip_direct_host_action_executor_scan(path):
                    files.append(path)
    return sorted(set(files))


def _direct_host_action_executor_entry_gate() -> bool:
    print("[RUN] static direct host action executor entry gate")
    violations: list[str] = []
    markers = (
        "_execute_confirmed_action(",
        "enqueue_and_process(",
    )
    for path in _iter_direct_host_action_executor_scan_files():
        rel = _to_repo_path(path)
        try:
            source = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            source = path.read_text(encoding="utf-8-sig")
        interesting_lines = []
        for lineno, line in enumerate(source.splitlines(), start=1):
            stripped = line.strip()
            if not stripped or stripped.startswith("#") or stripped.startswith(("\"", "'")):
                continue
            if stripped.startswith("def "):
                continue
            if any(marker in stripped for marker in markers):
                interesting_lines.append((lineno, stripped))
        if not interesting_lines:
            continue
        allowed_patterns = DIRECT_HOST_ACTION_EXECUTOR_ALLOWED_LINE_PATTERNS.get(rel, ())
        for lineno, line in interesting_lines:
            if rel in DIRECT_HOST_ACTION_EXECUTOR_ALLOWED_FILES and any(
                pattern in line for pattern in allowed_patterns
            ):
                continue
            violations.append(f"{rel}:{lineno}: unexpected direct host action execution entry: {line}")
        for entry_marker, required_tokens in DIRECT_HOST_ACTION_EXECUTOR_REQUIRED_SCOPE_TOKENS.get(rel, []):
            scope, scope_errors = _function_scope(source, entry_marker)
            if scope_errors:
                violations.extend(f"{rel}: {item}" for item in scope_errors)
                continue
            last_index = -1
            for token in required_tokens:
                token_index = scope.find(token)
                if token_index < 0:
                    violations.append(f"{rel}: {entry_marker} scope missing required token {token!r}")
                    continue
                if token_index < last_index:
                    violations.append(
                        f"{rel}: {entry_marker} scope has required token {token!r} out of execution order"
                    )
                last_index = token_index
    if violations:
        print("[FAIL] static direct host action executor entry gate")
        for item in violations:
            _print_violation(item)
        return False
    print("[OK]  static direct host action executor entry gate")
    return True


def _host_action_executor_policy_gate() -> bool:
    print("[RUN] static host action executor policy gate")
    path = REPO_ROOT / LANCHAT_HOST_ACTION_EXECUTOR
    try:
        source = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        source = path.read_text(encoding="utf-8-sig")
    violations: list[str] = []
    init_source = _function_source(source, "__init__")
    execute_payload = _function_source(source, "_execute_payload")
    structured_action = _function_source(source, "_is_structured_seed_plan_action")

    if not init_source:
        violations.append("LanChatHostActionExecutor.__init__ not found")
    else:
        for token in (
            "structured_action_handler: Callable[[dict[str, Any]], str] | None = None",
            "allow_legacy_agent_fallback: bool = False",
            "self._structured_action_handler = structured_action_handler",
            "self._allow_legacy_agent_fallback = bool(allow_legacy_agent_fallback)",
        ):
            if token not in init_source:
                violations.append(f"LanChatHostActionExecutor.__init__ missing policy token: {token}")

    if not execute_payload:
        violations.append("LanChatHostActionExecutor._execute_payload not found")
    else:
        required_order = (
            "if self._is_structured_seed_plan_payload(payload):",
            "if not self._is_structured_seed_plan_action(payload):",
            "if self._structured_action_handler is None:",
            "return str(self._structured_action_handler(dict(payload)))",
            "if not self._allow_legacy_agent_fallback:",
            "agent = self._get_agent()",
        )
        last_index = -1
        for token in required_order:
            token_index = execute_payload.find(token)
            if token_index < 0:
                violations.append(f"LanChatHostActionExecutor._execute_payload missing policy token: {token}")
                continue
            if token_index < last_index:
                violations.append(
                    f"LanChatHostActionExecutor._execute_payload policy token out of order: {token}"
                )
            last_index = token_index

    if not structured_action:
        violations.append("LanChatHostActionExecutor._is_structured_seed_plan_action not found")
    else:
        for token in ('"start_generation"', '"execute_seed_plan"', '"post_generation_add"'):
            if token not in structured_action:
                violations.append(
                    f"LanChatHostActionExecutor._is_structured_seed_plan_action missing allowed action: {token}"
                )

    if violations:
        print("[FAIL] static host action executor policy gate")
        for item in violations:
            _print_violation(item)
        return False
    print("[OK]  static host action executor policy gate")
    return True


def _legacy_agent_coordinator_policy_gate() -> bool:
    print("[RUN] static legacy AgentCoordinator policy gate")
    path = REPO_ROOT / LEGACY_AGENT_COORDINATOR
    try:
        source = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        source = path.read_text(encoding="utf-8-sig")
    except OSError as exc:
        print(f"[FAIL] static legacy AgentCoordinator policy gate: cannot read {LEGACY_AGENT_COORDINATOR}: {exc}")
        return False

    violations: list[str] = []
    execute_source = _function_source(source, "execute")
    allow_source = _function_source(source, "_legacy_direct_execute_allowed")

    for token in (
        '_RUNTIME_CONTROLLED_ACTIONS = frozenset({"add", "delete", "move", "modify"})',
        "agent_runtime_required",
    ):
        if token not in source:
            violations.append(f"AgentCoordinator missing required Runtime takeover token: {token}")

    if not execute_source:
        violations.append("AgentCoordinator.execute not found")
    else:
        required_order = (
            "if action in self._RUNTIME_CONTROLLED_ACTIONS and not self._legacy_direct_execute_allowed(scene_state):",
            '"status": "blocked"',
            '"reason": "agent_runtime_required"',
            '"execution": "agent_runtime_required"',
            "self._broadcast(intent, spatial, result)",
            "self._record(intent, spatial, result, scene_state)",
            "return result",
        )
        last_index = -1
        for token in required_order:
            token_index = execute_source.find(token)
            if token_index < 0:
                violations.append(f"AgentCoordinator.execute missing Runtime takeover token: {token}")
                continue
            if token_index < last_index:
                violations.append(f"AgentCoordinator.execute Runtime takeover token out of order: {token}")
            last_index = token_index

    if not allow_source:
        violations.append("AgentCoordinator._legacy_direct_execute_allowed not found")
    else:
        for token in (
            "allow_legacy_direct_agent_execute",
            "allow_legacy_agent_coordinator_execute",
            "AgentRuntimeFlags.from_env().old_workflow_direct_entry_disabled",
            "return False",
        ):
            if token not in allow_source:
                violations.append(f"AgentCoordinator._legacy_direct_execute_allowed missing token: {token}")

    if violations:
        print("[FAIL] static legacy AgentCoordinator policy gate")
        for item in violations:
            _print_violation(item)
        return False
    print("[OK]  static legacy AgentCoordinator policy gate")
    return True


def _legacy_role_agent_scene_write_policy_gate() -> bool:
    print("[RUN] static legacy RoleAgent scene-write policy gate")
    path = REPO_ROOT / LANCHAT_AGENT_WORKER
    try:
        source = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        source = path.read_text(encoding="utf-8-sig")
    except OSError as exc:
        print(f"[FAIL] static legacy RoleAgent scene-write policy gate: cannot read {LANCHAT_AGENT_WORKER}: {exc}")
        return False

    violations: list[str] = []
    process_trigger = _function_source(source, "_process_trigger")
    write_gate = _function_source(source, "_handle_agent_trigger_runtime_write_gate")
    if not process_trigger:
        violations.append("LANChatAgentWorker._process_trigger not found")
    else:
        required_order = (
            "planning_seed = self._seed_agent_trigger_planning_context_in_runtime(trigger)",
            "if self._handle_agent_trigger_planning_gate(trigger):",
            "if self._handle_agent_trigger_runtime_write_gate(trigger, planning_seed=planning_seed):",
            "result = self._run_agent(trigger)",
        )
        last_index = -1
        for token in required_order:
            token_index = process_trigger.find(token)
            if token_index < 0:
                violations.append(f"LANChatAgentWorker._process_trigger missing RoleAgent scene-write token: {token}")
                continue
            if token_index < last_index:
                violations.append(f"LANChatAgentWorker._process_trigger RoleAgent scene-write token out of order: {token}")
            last_index = token_index
    if not write_gate:
        violations.append("LANChatAgentWorker._handle_agent_trigger_runtime_write_gate not found")
    else:
        for token in (
            "if self._agent_runtime_flags.can_call_legacy_main_workflow():",
            "get_intent_understanding_service().classify(",
            '"generation_start"',
            '"intervention_add"',
            '"intervention_modify"',
            '"intervention_delete"',
            '"post_generation_add"',
            '"final_adjustment_request"',
            "legacy_role_agent_scene_write_blocked",
            "agent_runtime_required",
            'self._send_final_reply("gm-system"',
        ):
            if token not in write_gate:
                violations.append(f"LANChatAgentWorker._handle_agent_trigger_runtime_write_gate missing token: {token}")

    if violations:
        print("[FAIL] static legacy RoleAgent scene-write policy gate")
        for item in violations:
            _print_violation(item)
        return False
    print("[OK]  static legacy RoleAgent scene-write policy gate")
    return True


def _agent_runtime_flag_boundary_gate() -> bool:
    print("[RUN] static AgentRuntime flag boundary gate")
    flags_path = REPO_ROOT / AGENT_RUNTIME_FLAGS
    adapter_path = REPO_ROOT / GENERATION_COMPOSER_ADAPTER
    worker_path = REPO_ROOT / LANCHAT_AGENT_WORKER
    core_path = REPO_ROOT / AGENT_RUNTIME_CORE
    runtime_tools_path = REPO_ROOT / AGENT_RUNTIME_TOOLS
    runtime_adapters_path = REPO_ROOT / AGENT_RUNTIME_ADAPTERS
    runtime_report_policy_path = REPO_ROOT / "editor/plugins/AITool/services/runtime_report_policy.py"
    runtime_replay_report_policy_path = REPO_ROOT / "editor/plugins/AITool/services/runtime_replay_report_policy.py"
    runtime_replay_lifecycle_policy_path = REPO_ROOT / "editor/plugins/AITool/services/runtime_replay_lifecycle_policy.py"
    runtime_replay_event_policy_path = REPO_ROOT / "editor/plugins/AITool/services/runtime_replay_event_policy.py"
    runtime_replay_detail_policy_path = REPO_ROOT / "editor/plugins/AITool/services/runtime_replay_detail_policy.py"
    runtime_replay_resource_policy_path = REPO_ROOT / "editor/plugins/AITool/services/runtime_replay_resource_policy.py"
    runtime_replay_transfer_policy_path = REPO_ROOT / "editor/plugins/AITool/services/runtime_replay_transfer_policy.py"
    runtime_replay_peer_sync_policy_path = REPO_ROOT / "editor/plugins/AITool/services/runtime_replay_peer_sync_policy.py"
    runtime_sync_policy_path = REPO_ROOT / "editor/plugins/AITool/services/runtime_sync_policy.py"
    runtime_replay_sync_policy_path = REPO_ROOT / "editor/plugins/AITool/services/runtime_replay_sync_policy.py"
    runtime_message_delivery_policy_path = REPO_ROOT / "editor/plugins/AITool/services/runtime_message_delivery_policy.py"
    runtime_guard_test_path = REPO_ROOT / "editor/plugins/AITool/services/tests/test_lanchat_runtime_guard.py"
    try:
        flags_source = flags_path.read_text(encoding="utf-8")
        adapter_source = adapter_path.read_text(encoding="utf-8")
        worker_source = worker_path.read_text(encoding="utf-8")
        core_source = core_path.read_text(encoding="utf-8")
        runtime_tools_source = runtime_tools_path.read_text(encoding="utf-8")
        runtime_adapters_source = runtime_adapters_path.read_text(encoding="utf-8")
        runtime_report_policy_source = runtime_report_policy_path.read_text(encoding="utf-8")
        runtime_replay_report_policy_source = runtime_replay_report_policy_path.read_text(encoding="utf-8")
        runtime_replay_lifecycle_policy_source = runtime_replay_lifecycle_policy_path.read_text(encoding="utf-8")
        runtime_replay_event_policy_source = runtime_replay_event_policy_path.read_text(encoding="utf-8")
        runtime_replay_detail_policy_source = runtime_replay_detail_policy_path.read_text(encoding="utf-8")
        runtime_replay_resource_policy_source = runtime_replay_resource_policy_path.read_text(encoding="utf-8")
        runtime_replay_transfer_policy_source = runtime_replay_transfer_policy_path.read_text(encoding="utf-8")
        runtime_replay_peer_sync_policy_source = runtime_replay_peer_sync_policy_path.read_text(encoding="utf-8")
        runtime_sync_policy_source = runtime_sync_policy_path.read_text(encoding="utf-8")
        runtime_replay_sync_policy_source = runtime_replay_sync_policy_path.read_text(encoding="utf-8")
        runtime_message_delivery_policy_source = runtime_message_delivery_policy_path.read_text(encoding="utf-8")
        runtime_guard_test_source = runtime_guard_test_path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        flags_source = flags_path.read_text(encoding="utf-8-sig")
        adapter_source = adapter_path.read_text(encoding="utf-8-sig")
        worker_source = worker_path.read_text(encoding="utf-8-sig")
        core_source = core_path.read_text(encoding="utf-8-sig")
        runtime_tools_source = runtime_tools_path.read_text(encoding="utf-8-sig")
        runtime_adapters_source = runtime_adapters_path.read_text(encoding="utf-8-sig")
        runtime_report_policy_source = runtime_report_policy_path.read_text(encoding="utf-8-sig")
        runtime_replay_report_policy_source = runtime_replay_report_policy_path.read_text(encoding="utf-8-sig")
        runtime_replay_lifecycle_policy_source = runtime_replay_lifecycle_policy_path.read_text(encoding="utf-8-sig")
        runtime_replay_event_policy_source = runtime_replay_event_policy_path.read_text(encoding="utf-8-sig")
        runtime_replay_detail_policy_source = runtime_replay_detail_policy_path.read_text(encoding="utf-8-sig")
        runtime_replay_resource_policy_source = runtime_replay_resource_policy_path.read_text(encoding="utf-8-sig")
        runtime_replay_transfer_policy_source = runtime_replay_transfer_policy_path.read_text(encoding="utf-8-sig")
        runtime_replay_peer_sync_policy_source = runtime_replay_peer_sync_policy_path.read_text(encoding="utf-8-sig")
        runtime_sync_policy_source = runtime_sync_policy_path.read_text(encoding="utf-8-sig")
        runtime_replay_sync_policy_source = runtime_replay_sync_policy_path.read_text(encoding="utf-8-sig")
        runtime_message_delivery_policy_source = runtime_message_delivery_policy_path.read_text(encoding="utf-8-sig")
        runtime_guard_test_source = runtime_guard_test_path.read_text(encoding="utf-8-sig")

    violations: list[str] = []

    required_flag_defaults = (
        "agent_runtime_enabled: bool = True",
        "old_workflow_direct_entry_disabled: bool = True",
        "allow_legacy_function_adapter: bool = True",
        "allow_legacy_main_workflow: bool = False",
        "use_scene_snapshot_provider: bool = False",
        "use_scene_review_provider: bool = False",
        "use_image_resource_provider: bool = False",
        "use_model_resource_provider: bool = False",
        "use_legacy_model_resource_provider: bool = False",
        "use_environment_component_provider: bool = False",
        "use_engine_environment_import_provider: bool = False",
        "use_engine_actor_import_provider: bool = False",
        "use_engine_actor_delete_provider: bool = False",
        "use_engine_layout_transform_provider: bool = False",
        'agent_runtime_enabled=_env_bool(values, "AGENT_RUNTIME_ENABLED", True)',
        'old_workflow_direct_entry_disabled=_env_bool(values, "OLD_WORKFLOW_DIRECT_ENTRY_DISABLED", True)',
        'allow_legacy_function_adapter=_env_bool(values, "ALLOW_LEGACY_FUNCTION_ADAPTER", True)',
        'allow_legacy_main_workflow=_env_bool(values, "ALLOW_LEGACY_MAIN_WORKFLOW", False)',
        'use_scene_snapshot_provider=_env_bool(values, "AGENT_RUNTIME_USE_SCENE_SNAPSHOT_PROVIDER", False)',
        'use_scene_review_provider=_env_bool(values, "AGENT_RUNTIME_USE_SCENE_REVIEW_PROVIDER", False)',
        'use_image_resource_provider=_env_bool(values, "AGENT_RUNTIME_USE_IMAGE_PROVIDER", False)',
        'use_model_resource_provider=_env_bool(values, "AGENT_RUNTIME_USE_MODEL_PROVIDER", False)',
        'use_legacy_model_resource_provider=_env_bool(values, "AGENT_RUNTIME_USE_LEGACY_MODEL_PROVIDER", False)',
        'use_environment_component_provider=_env_bool(values, "AGENT_RUNTIME_USE_ENVIRONMENT_PROVIDER", False)',
        'use_engine_environment_import_provider=_env_bool(values, "AGENT_RUNTIME_USE_ENGINE_ENVIRONMENT_IMPORT_PROVIDER", False)',
        'use_engine_actor_import_provider=_env_bool(values, "AGENT_RUNTIME_USE_ENGINE_IMPORT_PROVIDER", False)',
        'use_engine_actor_delete_provider=_env_bool(values, "AGENT_RUNTIME_USE_ENGINE_DELETE_PROVIDER", False)',
        'use_engine_layout_transform_provider=_env_bool(values, "AGENT_RUNTIME_USE_ENGINE_TRANSFORM_PROVIDER", False)',
    )
    for token in required_flag_defaults:
        if token not in flags_source:
            violations.append(f"AgentRuntimeFlags missing required default/env token: {token}")

    can_call_legacy = _function_source(flags_source, "can_call_legacy_main_workflow")
    if not can_call_legacy:
        violations.append("AgentRuntimeFlags.can_call_legacy_main_workflow not found")
    else:
        for token in (
            "self.agent_runtime_enabled",
            "self.allow_legacy_main_workflow",
            "not self.old_workflow_direct_entry_disabled",
        ):
            if token not in can_call_legacy:
                violations.append(f"can_call_legacy_main_workflow missing hard boundary token: {token}")

    assert_blocked = _function_source(flags_source, "assert_legacy_main_workflow_blocked")
    if "if self.can_call_legacy_main_workflow():" not in assert_blocked:
        violations.append("AgentRuntimeFlags.assert_legacy_main_workflow_blocked must fail if legacy main workflow is enabled")

    for method_name, flag_name in (
        ("can_use_scene_snapshot_provider", "use_scene_snapshot_provider"),
        ("can_use_scene_review_provider", "use_scene_review_provider"),
        ("can_use_image_resource_provider", "use_image_resource_provider"),
        ("can_use_model_resource_provider", "use_model_resource_provider"),
        ("can_use_legacy_model_resource_provider", "use_legacy_model_resource_provider"),
        ("can_use_environment_component_provider", "use_environment_component_provider"),
        ("can_use_engine_environment_import_provider", "use_engine_environment_import_provider"),
        ("can_use_engine_actor_import_provider", "use_engine_actor_import_provider"),
        ("can_use_engine_actor_delete_provider", "use_engine_actor_delete_provider"),
        ("can_use_engine_layout_transform_provider", "use_engine_layout_transform_provider"),
    ):
        method_source = _function_source(flags_source, method_name)
        if not method_source:
            violations.append(f"AgentRuntimeFlags.{method_name} not found")
            continue
        for token in ("self.can_call_legacy_function_adapter()", f"self.{flag_name}"):
            if token not in method_source:
                violations.append(f"AgentRuntimeFlags.{method_name} missing provider boundary token: {token}")

    compose = _function_source(adapter_source, "compose")
    if not compose:
        violations.append("SceneComposerJobRunner.compose not found")
    else:
        required_order = (
            "if not self._agent_runtime_flags.can_call_legacy_main_workflow():",
            "legacy SceneComposer main workflow is disabled by AgentRuntimeFlags",
            "composer = self._composer_factory()",
            "result = composer.compose(",
        )
        last_index = -1
        for token in required_order:
            token_index = compose.find(token)
            if token_index < 0:
                violations.append(f"SceneComposerJobRunner.compose missing legacy-main boundary token: {token}")
                continue
            if token_index < last_index:
                violations.append(f"SceneComposerJobRunner.compose legacy-main boundary token out of order: {token}")
            last_index = token_index

    get_scheduler = _function_source(worker_source, "_get_generation_scheduler")
    if not get_scheduler:
        violations.append("LANChatAgentWorker._get_generation_scheduler not found")
    else:
        required_order = (
            "if not self._agent_runtime_flags.can_call_legacy_main_workflow():",
            "return None",
            "from .generation_scheduler import GenerationScheduler",
            "self._generation_scheduler = GenerationScheduler(",
            "self._install_generation_scheduler_hooks(self._generation_scheduler)",
        )
        last_index = -1
        for token in required_order:
            token_index = get_scheduler.find(token)
            if token_index < 0:
                violations.append(f"LANChatAgentWorker._get_generation_scheduler missing legacy-main boundary token: {token}")
                continue
            if token_index < last_index:
                violations.append(f"LANChatAgentWorker._get_generation_scheduler legacy-main boundary token out of order: {token}")
            last_index = token_index

    create_runtime = _function_source(worker_source, "_create_agent_runtime")
    if not create_runtime:
        violations.append("LANChatAgentWorker._create_agent_runtime not found")
    else:
        for guard_token, factory_token in (
            ("can_use_scene_snapshot_provider()", "make_scene_snapshot_provider"),
            ("can_use_image_resource_provider()", "make_image_resource_provider"),
            ("can_use_scene_review_provider()", "make_scene_review_provider"),
            ("can_use_environment_component_provider()", "make_environment_component_provider"),
            ("can_use_engine_environment_import_provider()", "make_engine_environment_component_import_provider"),
            ("can_use_model_resource_provider()", "make_model_resource_provider"),
            ("can_use_legacy_model_resource_provider()", "make_legacy_model_resource_provider"),
            ("can_use_engine_actor_import_provider()", "make_engine_actor_import_provider"),
            ("can_use_engine_actor_delete_provider()", "make_engine_actor_delete_provider"),
            ("can_use_engine_layout_transform_provider()", "make_engine_layout_transform_provider"),
        ):
            guard_index = create_runtime.find(guard_token)
            factory_index = create_runtime.find(factory_token)
            if factory_index < 0:
                violations.append(f"LANChatAgentWorker._create_agent_runtime missing provider factory token: {factory_token}")
                continue
            if guard_index < 0:
                violations.append(f"LANChatAgentWorker._create_agent_runtime missing provider guard token: {guard_token}")
                continue
            if guard_index > factory_index:
                violations.append(
                    "LANChatAgentWorker._create_agent_runtime provider factory appears before its flag guard: "
                    f"{factory_token}"
                )
        for required in (
            "legacy_model_adapter_unavailable",
            "return resources",
        ):
            if required not in runtime_adapters_source:
                violations.append(f"legacy model resource adapter missing provider-unavailable fact token: {required}")
        if 'in {"legacy_model_failure", "legacy_model_adapter_unavailable"}' not in runtime_tools_source:
            violations.append(
                "runtime.asset.model.prepare must treat legacy model adapter unavailable facts as hard model failure"
            )
        for required in (
            "test_model_provider_flag_does_not_fallback_to_legacy_model_provider",
            '"AGENT_RUNTIME_USE_MODEL_PROVIDER": "1"',
            "legacy_model_provider",
            "assertNotIn",
        ):
            if required not in runtime_guard_test_source:
                violations.append(f"LANChat Runtime guard missing model-provider no-legacy-fallback test token: {required}")
        for required in (
            "_failed_environment_import_components",
            "runtime_environment_import_failed",
            "runtime_default_environment_import",
            "runtime_state_only",
            '"environment_components": {batch_id: failed_components}',
            '"custom_import_facts"',
            'f"{batch_id}:environment_import_result"',
            "_environment_import_result_fact(",
            "changes=changes",
        ):
            if required not in runtime_tools_source:
                violations.append(f"runtime.environment.import_components missing failed-fact token: {required}")

    runtime_status_reply = _function_source(worker_source, "_agent_runtime_status_reply")
    gm_summary_reply = _function_source(worker_source, "_agent_runtime_gm_summary_reply")
    runtime_report_reply = _function_source(worker_source, "_handle_agent_runtime_report_query")
    operation_replay_reply = _function_source(worker_source, "_handle_agent_runtime_operation_replay_query")
    runtime_system_event_sender = _function_source(worker_source, "_send_agent_runtime_system_event")
    runtime_event_emitter = _function_source(worker_source, "_emit_agent_runtime_events_since")
    runtime_event_metadata_helper = _function_source(worker_source, "_safe_runtime_event_metadata")
    runtime_event_disclosure_guard = _function_source(worker_source, "_should_auto_disclose_agent_runtime_event")
    runtime_event_disclosure_skip = _function_source(worker_source, "_record_skipped_agent_runtime_event_disclosure")
    runtime_audit_recorder = _function_source(worker_source, "_record_runtime_audit_event")
    lanchat_sync_bridge = _function_source(worker_source, "_record_lanchat_sync_event_in_agent_runtime")
    lanchat_sync_bridge_reason = _function_source(worker_source, "_safe_lanchat_sync_bridge_reason")
    resource_flow_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_resource_flow_report",
    )
    scene_snapshot_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_scene_snapshot_report",
    )
    resource_stage_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_resource_stage_report",
    )
    resource_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_resource_report",
    )
    resource_readiness_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_resource_readiness_report",
    )
    closure_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_closure_report",
    )
    actor_import_boundary_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_actor_import_boundary_report",
    )
    report_health_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_report_health_report",
    )
    context_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_context_report",
    )
    intervention_digest_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_intervention_digest",
    )
    intervention_summary_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_intervention_summary",
    )
    intervention_batch_summary_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_intervention_batch_summary",
    )
    message_delivery_formatter = _function_source(
        runtime_message_delivery_policy_source,
        "format_agent_runtime_message_delivery_report",
    )
    engine_write_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_engine_write_report",
    )
    engine_write_readiness_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_engine_write_readiness_report",
    )
    engine_write_boundary_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_engine_write_boundary_report",
    )
    import_stage_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_import_stage_report",
    )
    geometry_fact_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_geometry_fact_report",
    )
    command_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_command_report",
    )
    review_proposal_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_review_proposal_report",
    )
    review_confirmation_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_review_confirmation_report",
    )
    review_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_review_report",
    )
    tool_queue_health_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_tool_queue_health_report",
    )
    batch_tooling_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_batch_tooling_report",
    )
    batch_resource_lifecycle_formatter = _function_source(
        runtime_report_policy_source,
        "format_agent_runtime_batch_resource_lifecycle_report",
    )
    replay_command_formatter = _function_source(
        runtime_replay_report_policy_source,
        "format_agent_runtime_replay_command_report",
    )
    replay_tool_formatter = _function_source(
        runtime_replay_report_policy_source,
        "format_agent_runtime_replay_tool_execution_report",
    )
    replay_queue_formatter = _function_source(
        runtime_replay_report_policy_source,
        "format_agent_runtime_replay_tool_queue_report",
    )
    replay_state_patch_formatter = _function_source(
        runtime_replay_report_policy_source,
        "format_agent_runtime_replay_state_patch_report",
    )
    replay_guard_formatter = _function_source(
        runtime_replay_report_policy_source,
        "format_agent_runtime_replay_guard_report",
    )
    worker_drain_replay_formatter = _function_source(
        runtime_replay_report_policy_source,
        "format_agent_runtime_worker_drain_replay_report",
    )
    tool_graph_replay_formatter = _function_source(
        runtime_replay_report_policy_source,
        "format_agent_runtime_tool_graph_replay_report",
    )
    gm_summary_replay_formatter = _function_source(
        runtime_replay_report_policy_source,
        "format_agent_runtime_gm_summary_replay_report",
    )
    replay_plan_lifecycle_formatter = _function_source(
        runtime_replay_lifecycle_policy_source,
        "format_agent_runtime_replay_plan_lifecycle_report",
    )
    replay_intervention_formatter = _function_source(
        runtime_replay_lifecycle_policy_source,
        "format_agent_runtime_replay_intervention_report",
    )
    replay_geometry_formatter = _function_source(
        runtime_replay_lifecycle_policy_source,
        "format_agent_runtime_replay_geometry_report",
    )
    replay_runtime_event_formatter = _function_source(
        runtime_replay_event_policy_source,
        "format_agent_runtime_replay_runtime_event_report",
    )
    event_rows_formatter = _function_source(
        runtime_replay_event_policy_source,
        "format_agent_runtime_event_rows",
    )
    replay_report_formatter = _function_source(
        runtime_replay_report_policy_source,
        "format_agent_runtime_replay_report",
    )
    runtime_event_replay_summary = _function_source(
        core_source,
        "_runtime_event_replay_summary",
    )
    runtime_guard_replay_summary = _function_source(
        core_source,
        "_runtime_guard_replay_summary",
    )
    resource_readiness_replay_summary = _function_source(
        core_source,
        "_resource_readiness_replay_summary",
    )
    replay_failure_strategy_formatter = _function_source(
        runtime_replay_detail_policy_source,
        "format_agent_runtime_replay_failure_strategy_report",
    )
    replay_layout_formatter = _function_source(
        runtime_replay_detail_policy_source,
        "format_agent_runtime_replay_layout_report",
    )
    replay_final_adjustment_formatter = _function_source(
        runtime_replay_detail_policy_source,
        "format_agent_runtime_replay_final_adjustment_report",
    )
    replay_vlm_formatter = _function_source(
        runtime_replay_detail_policy_source,
        "format_agent_runtime_replay_vlm_report",
    )
    replay_review_advisory_formatter = _function_source(
        runtime_replay_detail_policy_source,
        "format_agent_runtime_replay_review_advisory_report",
    )
    replay_environment_formatter = _function_source(
        runtime_replay_resource_policy_source,
        "format_agent_runtime_replay_environment_report",
    )
    replay_readiness_formatter = _function_source(
        runtime_replay_resource_policy_source,
        "format_agent_runtime_replay_resource_readiness_report",
    )
    replay_sync_formatter = _function_source(
        runtime_replay_sync_policy_source,
        "format_agent_runtime_sync_replay_report",
    )
    replay_asset_transfer_formatter = _function_source(
        runtime_replay_transfer_policy_source,
        "format_agent_runtime_replay_asset_transfer_report",
    )
    replay_peer_sync_formatter = _function_source(
        runtime_replay_peer_sync_policy_source,
        "format_agent_runtime_replay_peer_sync_report",
    )
    sync_actor_rows_formatter = _function_source(
        runtime_sync_policy_source,
        "format_agent_runtime_sync_actor_rows",
    )
    sync_asset_rows_formatter = _function_source(
        runtime_sync_policy_source,
        "format_agent_runtime_sync_asset_rows",
    )
    sync_health_formatter = _function_source(
        runtime_sync_policy_source,
        "format_agent_runtime_sync_health_report",
    )
    sync_report_formatter = _function_source(
        runtime_sync_policy_source,
        "format_agent_runtime_sync_report",
    )
    gm_sync_replay_formatter = _function_source(
        runtime_sync_policy_source,
        "format_agent_runtime_gm_sync_replay_digest",
    )
    sync_asset_transfer_formatter = _function_source(
        runtime_sync_policy_source,
        "format_agent_runtime_asset_transfer_report",
    )
    gm_runtime_event_replay_formatter = _function_source(
        runtime_replay_event_policy_source,
        "format_agent_runtime_gm_runtime_event_replay_digest",
    )
    gm_sync_replay_forwarder = _function_source(
        worker_source,
        "_format_agent_runtime_gm_sync_replay_digest",
    )
    if not resource_flow_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_resource_flow_report not found")
    else:
        for token in (
            '"batches {batch_count}"',
            '"completed {completed_count}"',
            '"failed {failed_count}"',
            "image_ready_count",
            "model_ready_count",
            "import_ready_count",
            "import_failure_code_counts",
            "import-failures",
            "safe_label(code)",
        ):
            if token not in resource_flow_formatter:
                violations.append(f"LANChatAgentWorker resource flow formatter missing token: {token}")
    if not lanchat_sync_bridge:
        violations.append("LANChatAgentWorker._record_lanchat_sync_event_in_agent_runtime not found")
    else:
        for token in (
            'action="runtime_sync_event"',
            "_safe_lanchat_sync_bridge_reason",
            '"event": dict(result.get("sync_event") or {})',
            '"sync_state": dict(result.get("sync_status") or {})',
        ):
            if token not in lanchat_sync_bridge:
                violations.append(f"LANChatAgentWorker sync bridge missing safe Runtime token: {token}")
        if 'str(result.get("message")' in lanchat_sync_bridge:
            violations.append("LANChatAgentWorker sync bridge must not expose raw Runtime message as reason")
    if not lanchat_sync_bridge_reason:
        violations.append("LANChatAgentWorker._safe_lanchat_sync_bridge_reason not found")
    else:
        for token in (
            "runtime_sync_rejected",
            "provider",
            "prompt",
            "api_key",
            "https://",
            ".glb",
        ):
            if token not in lanchat_sync_bridge_reason:
                violations.append(f"LANChatAgentWorker sync bridge reason sanitizer missing token: {token}")
    if not report_health_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_report_health_report not found")
    else:
        for token in (
            "sync_failure_code_counts",
            "latest_sync_failure_code",
            "sync failures",
            "import_failure_code_counts",
            "import failures",
        ):
            if token not in report_health_formatter:
                violations.append(f"LANChatAgentWorker report health formatter missing sync failure diagnostic token: {token}")
    if not context_formatter:
        violations.append("runtime_report_policy.format_agent_runtime_context_report not found")
    else:
        for token in ("context_count", "context_type_counts", "speaker_type_counts", "latest_context", "safe_label"):
            if token not in context_formatter:
                violations.append(f"Runtime context formatter missing token: {token}")
    if not intervention_digest_formatter:
        violations.append("runtime_report_policy.format_agent_runtime_intervention_digest not found")
    else:
        for token in (
            "pending_count",
            "accepted_count",
            "deferred_count",
            "absorbable_pending_count",
            "non_absorbable_pending_count",
        ):
            if token not in intervention_digest_formatter:
                violations.append(f"Runtime intervention digest formatter missing token: {token}")
    for formatter, name, tokens in (
        (
            intervention_summary_formatter,
            "intervention summary",
            ("pending_count", "accepted_count", "deferred_count", "latest_pending"),
        ),
        (
            intervention_batch_summary_formatter,
            "intervention batch summary",
            ("batch_count", "status_counts", "latest_batches", "requested_items"),
        ),
    ):
        if not formatter:
            violations.append(f"runtime_report_policy {name} formatter not found")
        else:
            for token in tokens:
                if token not in formatter:
                    violations.append(f"Runtime {name} formatter missing token: {token}")
    if not message_delivery_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_message_delivery_report not found")
    else:
        for token in ("failure_code_counts", "latest_failure_code", "failure codes"):
            if token not in message_delivery_formatter:
                violations.append(
                    f"LANChatAgentWorker message delivery formatter missing failure diagnostic token: {token}"
                )
    if not scene_snapshot_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_scene_snapshot_report not found")
    else:
        for token in (
            "snapshot_count",
            "observed_actor_count",
            "observed_actor_total_count",
            "latest_source",
            "snapshots",
            "observed",
        ):
            if token not in scene_snapshot_formatter:
                violations.append(f"LANChatAgentWorker scene snapshot formatter missing token: {token}")
    if not resource_stage_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_resource_stage_report not found")
    else:
        for token in (
            "event_count",
            "by_phase",
            "requested_count",
            "failed_count",
            "latest_events",
            "image",
            "model",
        ):
            if token not in resource_stage_formatter:
                violations.append(f"LANChatAgentWorker resource stage formatter missing token: {token}")
    if not resource_formatter:
        violations.append("runtime_report_policy resource formatter not found")
    else:
        for token in ("default runtime adapters", "scene_snapshot", "safe_value", "adapter"):
            if token not in resource_formatter:
                violations.append(f"runtime_report_policy resource formatter missing token: {token}")
    if not resource_readiness_formatter:
        violations.append("runtime_report_policy resource readiness formatter not found")
    else:
        for token in ("channel_count", "requested_count", "enabled_count", "unavailable_channels"):
            if token not in resource_readiness_formatter:
                violations.append(
                    f"runtime_report_policy resource readiness formatter missing token: {token}"
                )
    if not import_stage_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_import_stage_report not found")
    else:
        for token in (
            "event_count",
            "requested_count",
            "imported_count",
            "failed_count",
            "latest_events",
            "imported",
        ):
            if token not in import_stage_formatter:
                violations.append(f"LANChatAgentWorker import stage formatter missing token: {token}")
    if not closure_formatter:
        violations.append("runtime_report_policy.format_agent_runtime_closure_report not found")
    else:
        for token in (
            "runtime_state_source",
            "engine_write_boundary_fact_count",
            "operation_count",
            "operation_total_count",
            "patch applied/conflict/invalid",
        ):
            if token not in closure_formatter:
                violations.append(f"Runtime closure formatter missing token: {token}")
    if not actor_import_boundary_formatter:
        violations.append("runtime_report_policy.format_agent_runtime_actor_import_boundary_report not found")
    else:
        for token in (
            "requested_count",
            "actor_count",
            "bridge_call_count",
            "runtime_state_only",
            "native pending F5",
        ):
            if token not in actor_import_boundary_formatter:
                violations.append(f"Runtime actor import boundary formatter missing token: {token}")
    if not geometry_fact_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_geometry_fact_report not found")
    else:
        for token in (
            "fact_count",
            "aabb_actor_count",
            "aabb_skipped_count",
            "overlap_issue_count",
            "fact_type_counts",
            "status_counts",
            "AABB actors",
            "overlap issues",
        ):
            if token not in geometry_fact_formatter:
                violations.append(f"LANChatAgentWorker geometry fact formatter missing token: {token}")
    if not command_formatter:
        violations.append("runtime_report_policy.format_agent_runtime_command_report not found")
    else:
        for token in ("command_count", "latest_commands", "safe_text", "provider", "prompt", "raw"):
            if token not in command_formatter:
                violations.append(f"Runtime command formatter missing token: {token}")
    if not review_proposal_formatter:
        violations.append("runtime_report_policy.format_agent_runtime_review_proposal_report not found")
    else:
        for token in ("proposal_count", "item_count", "status_counts", "waiting host confirmation"):
            if token not in review_proposal_formatter:
                violations.append(f"Runtime review proposal formatter missing token: {token}")
    if not review_confirmation_formatter:
        violations.append("runtime_report_policy.format_agent_runtime_review_confirmation_report not found")
    else:
        for token in ("confirmation_count", "decision_counts", "confirmation(s)"):
            if token not in review_confirmation_formatter:
                violations.append(f"Runtime review confirmation formatter missing token: {token}")
    if not review_formatter:
        violations.append("runtime_report_policy.format_agent_runtime_review_report not found")
    else:
        for token in ("review_count", "issue_count", "advisory_count", "status_counts", "checkpoint_counts"):
            if token not in review_formatter:
                violations.append(f"Runtime review formatter missing token: {token}")
    if not tool_queue_health_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_tool_queue_health_report not found")
    else:
        for token in (
            "queue_count",
            "queued_count",
            "running_count",
            "blocked_count",
            "terminal_count",
            "active_count",
            "queue_pressure",
            "pressure",
        ):
            if token not in tool_queue_health_formatter:
                violations.append(f"LANChatAgentWorker tool queue health formatter missing token: {token}")
    if not batch_tooling_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_batch_tooling_report not found")
    else:
        for token in (
            "fact_count",
            "created_batch_fact_count",
            "created_batch_count",
            "prioritized_item_count",
            "merged_intervention_fact_count",
            "merged_intervention_item_count",
            "absorbed_intervention_count",
            "latest_fact_types",
        ):
            if token not in batch_tooling_formatter:
                violations.append(f"LANChatAgentWorker batch tooling formatter missing token: {token}")
    if not batch_resource_lifecycle_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_batch_resource_lifecycle_report not found")
    else:
        for token in (
            '"events {resource_event_count}"',
            '"image {image_ready_count}/{image_failed_count}"',
            '"model {model_ready_count}/{model_failed_count}"',
            '"import {import_ready_count}/{import_failed_count}"',
            '"env {environment_ready_count}/{environment_failed_count}"',
        ):
            if token not in batch_resource_lifecycle_formatter:
                violations.append(
                    "LANChatAgentWorker batch resource lifecycle formatter missing token: "
                    f"{token}"
                )
    if not operation_replay_reply:
        violations.append("LANChatAgentWorker._handle_agent_runtime_operation_replay_query not found")
    else:
        for token in (
            "batch_resource_lifecycle_summary",
            "_format_agent_runtime_batch_resource_lifecycle_report",
            "runtime_command_summary",
            "_format_agent_runtime_replay_command_report",
            "tool_execution_summary",
            "_format_agent_runtime_replay_tool_execution_report",
            "tool_graph_queue_summary",
            "_format_agent_runtime_replay_tool_queue_report",
            "state_patch_summary",
            "_format_agent_runtime_replay_state_patch_report",
            "runtime_guard_replay_summary",
            "_format_agent_runtime_replay_guard_report",
            "scene_plan_lifecycle_summary",
            "_format_agent_runtime_replay_plan_lifecycle_report",
            "intervention_batch_replay_summary",
            "_format_agent_runtime_replay_intervention_report",
            "geometry_fact_replay_summary",
            "_format_agent_runtime_replay_geometry_report",
            "runtime_event_replay_summary",
            "_format_agent_runtime_replay_runtime_event_report",
            "tool_failure_strategy_summary",
            "_format_agent_runtime_replay_failure_strategy_report",
            "layout_adjustment_summary",
            "_format_agent_runtime_replay_layout_report",
            "final_adjustment_confirmation_replay_summary",
            "_format_agent_runtime_replay_final_adjustment_report",
            "vlm_checkpoint_summary",
            "_format_agent_runtime_replay_vlm_report",
            "environment_component_summary",
            "_format_agent_runtime_replay_environment_report",
            "resource_readiness_replay_summary",
            "_format_agent_runtime_replay_resource_readiness_report",
            "sync_summary",
            "_format_agent_runtime_sync_replay_report",
            "asset_transfer_replay_summary",
            "_format_agent_runtime_replay_asset_transfer_report",
            "peer_sync_replay_summary",
            "_format_agent_runtime_replay_peer_sync_report",
            "batch_resources",
            "commands",
            "tools",
            "queue",
            "state_patch",
            "guard",
            "plan_lifecycle",
            "interventions",
            "geometry",
            "runtime_events",
            "failure_strategy",
            "layout",
            "final_adjustment",
            "vlm",
            "environment",
            "resource_readiness",
            "sync",
            "asset_transfer",
            "peer_sync",
        ):
            if token not in operation_replay_reply:
                violations.append(f"LANChatAgentWorker operation replay reply missing token: {token}")
    if not runtime_report_reply:
        violations.append("LANChatAgentWorker._handle_agent_runtime_report_query not found")
    else:
        for token in (
            "operation_replay_summary",
            "sync_replay_summary",
            "_format_agent_runtime_sync_replay_report",
            "asset_transfer_replay_summary",
            "_format_agent_runtime_replay_asset_transfer_report",
            "peer_sync_replay_summary",
            "_format_agent_runtime_replay_peer_sync_report",
            "sync replay",
            "asset transfer replay",
            "peer sync replay",
        ):
            if token not in runtime_report_reply:
                violations.append(f"LANChatAgentWorker runtime report reply missing replay token: {token}")
        for token in (
            "batch_tooling_summary",
            "batch_tooling_text",
            "_format_agent_runtime_batch_tooling_report",
            "batch tooling",
        ):
            if token not in runtime_report_reply:
                violations.append(f"LANChatAgentWorker runtime report reply missing batch tooling token: {token}")
        for token in (
            "state_patch_summary",
            "state_patch_text",
            "_format_agent_runtime_replay_state_patch_report",
            "state patch",
            "tool_failure_strategy_summary",
            "failure_strategy_text",
            "_format_agent_runtime_replay_failure_strategy_report",
            "failure strategy",
            "runtime_guard_replay_summary",
            "runtime_guard_text",
            "_format_agent_runtime_replay_guard_report",
            "guard:",
            "scene_plan_lifecycle_summary",
            "plan_lifecycle_text",
            "_format_agent_runtime_replay_plan_lifecycle_report",
            "plan lifecycle",
        ):
            if token not in runtime_report_reply:
                violations.append(f"LANChatAgentWorker runtime report reply missing state/failure token: {token}")
        for token in (
            "tool_queue_health_summary",
            "tool_queue_health_text",
            "_format_agent_runtime_tool_queue_health_report",
            "runtime queue",
        ):
            if token not in runtime_report_reply:
                violations.append(f"LANChatAgentWorker runtime report reply missing queue health token: {token}")
        for token in (
            "tool_execution_digest",
            "tool_execution_text",
            "_format_agent_runtime_tool_execution_digest_report",
            "tool execution",
        ):
            if token not in runtime_report_reply:
                violations.append(f"LANChatAgentWorker runtime report reply missing tool execution token: {token}")
        for token in (
            "vlm_checkpoint_summary",
            "vlm_checkpoint_text",
            "_format_agent_runtime_replay_vlm_report",
            "vlm replay",
            "review_advisory_replay_summary",
            "review_advisory_replay_text",
            "_format_agent_runtime_replay_review_advisory_report",
            "review advisory replay",
            "final_adjustment_confirmation_replay_summary",
            "final_adjustment_replay_text",
            "_format_agent_runtime_replay_final_adjustment_report",
            "final adjustment replay",
        ):
            if token not in runtime_report_reply:
                violations.append(f"LANChatAgentWorker runtime report reply missing VLM/review replay token: {token}")
        for token in (
            "scene_design_contract_summary",
            "scene_contract_text",
            "_format_agent_runtime_scene_contract_report",
            "scene contract",
        ):
            if token not in runtime_report_reply:
                violations.append(f"LANChatAgentWorker runtime report reply missing scene contract token: {token}")
        for token in (
            "semantic_arbitration_summary",
            "semantic_arbitration_text",
            "_format_agent_runtime_semantic_arbitration_report",
            "semantic arbitration",
        ):
            if token not in runtime_report_reply:
                violations.append(f"LANChatAgentWorker runtime report reply missing semantic arbitration token: {token}")
        for token in (
            "scene_snapshot_summary",
            "scene_snapshot_text",
            "_format_agent_runtime_scene_snapshot_report",
            "scene snapshot",
            "resource_summary",
            "runtime_resource_text",
            "_format_agent_runtime_resource_stage_report",
            "runtime resources",
            "import_summary",
            "import_text",
            "_format_agent_runtime_import_stage_report",
            "import:",
        ):
            if token not in runtime_report_reply:
                violations.append(f"LANChatAgentWorker runtime report reply missing scene/resource/import token: {token}")
        for token in (
            "geometry_fact_summary",
            "geometry_text",
            "_format_agent_runtime_geometry_fact_report",
            "geometry facts",
        ):
            if token not in runtime_report_reply:
                violations.append(f"LANChatAgentWorker runtime report reply missing geometry fact token: {token}")
    if not replay_report_formatter:
        violations.append("runtime_replay_report_policy replay aggregate formatter not found")
    else:
        for token in (
            "runtime_event_replay_summary",
            "disclosure_skipped_count",
            "runtime_event_text",
        ):
            if token not in replay_report_formatter:
                violations.append(f"Runtime replay aggregate formatter missing runtime-event token: {token}")
    if not event_rows_formatter:
        violations.append("runtime_replay_event_policy event rows formatter not found")
    else:
        for token in ("events", "title", "message", "progress", "rows"):
            if token not in event_rows_formatter:
                violations.append(f"Runtime event rows formatter missing token: {token}")
    if not runtime_system_event_sender:
        violations.append("LANChatAgentWorker._send_agent_runtime_system_event not found")
    else:
        for token in (
            "runtime_event",
            "runtime_event_metadata",
            "json.dumps(metadata",
        ):
            if token not in runtime_system_event_sender:
                violations.append(f"LANChatAgentWorker runtime event sender missing metadata token: {token}")
    if not runtime_event_emitter:
        violations.append("LANChatAgentWorker._emit_agent_runtime_events_since not found")
    else:
        for token in (
            "fresh_events",
            "_should_auto_disclose_agent_runtime_event",
            "_format_agent_runtime_event_rows([event])",
            "_record_skipped_agent_runtime_event_disclosure",
        ):
            if token not in runtime_event_emitter:
                violations.append(f"LANChatAgentWorker runtime event emitter missing disclosure filter token: {token}")
    if not runtime_event_metadata_helper:
        violations.append("LANChatAgentWorker._safe_runtime_event_metadata not found")
    else:
        for token in (
            "runtime_event_id",
            "runtime_event_type",
            "runtime_plan_id",
            "runtime_batch_id",
            "runtime_stage",
            "runtime_audience",
            "runtime_level",
            "runtime_progress",
        ):
            if token not in runtime_event_metadata_helper:
                violations.append(f"LANChatAgentWorker runtime event metadata helper missing token: {token}")
    if not runtime_event_disclosure_guard:
        violations.append("LANChatAgentWorker._should_auto_disclose_agent_runtime_event not found")
    else:
        for token in ("host", "participants", "all"):
            if token not in runtime_event_disclosure_guard:
                violations.append(f"LANChatAgentWorker runtime event disclosure guard missing audience token: {token}")
    if not runtime_event_disclosure_skip:
        violations.append("LANChatAgentWorker._record_skipped_agent_runtime_event_disclosure not found")
    elif "runtime_system_event_disclosure_skipped" not in runtime_event_disclosure_skip:
        violations.append("LANChatAgentWorker runtime event disclosure skip audit missing event token")
    else:
        for token in ("runtime_plan_id", "batch_id"):
            if token not in runtime_event_disclosure_skip:
                violations.append(f"LANChatAgentWorker runtime event disclosure skip audit missing scope token: {token}")
    if not runtime_audit_recorder:
        violations.append("LANChatAgentWorker._record_runtime_audit_event not found")
    elif "runtime_plan_id" not in runtime_audit_recorder:
        violations.append("LANChatAgentWorker runtime audit recorder missing runtime_plan_id scope token")
    if not replay_command_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_replay_command_report not found")
    else:
        for token in (
            "cancelled_batch_total",
            "cancelled_graph_total",
            "resumed_graph_total",
            "retried_graph_total",
            "latest_command",
        ):
            if token not in replay_command_formatter:
                violations.append(f"LANChatAgentWorker replay command formatter missing token: {token}")
    if not replay_tool_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_replay_tool_execution_report not found")
    else:
        for token in (
            "started_count",
            "succeeded_count",
            "failed_count",
            "blocked_count",
            "retry_scheduled_count",
            "skipped_count",
            "latest_tool_event",
        ):
            if token not in replay_tool_formatter:
                violations.append(f"LANChatAgentWorker replay tool formatter missing token: {token}")
    if not replay_queue_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_replay_tool_queue_report not found")
    else:
        for token in (
            "queued_count",
            "dequeued_count",
            "completed_count",
            "rejected_count",
            "blocked_count",
            "missing_graph_count",
            "latest_queue_event",
        ):
            if token not in replay_queue_formatter:
                violations.append(f"LANChatAgentWorker replay queue formatter missing token: {token}")
    if not replay_state_patch_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_replay_state_patch_report not found")
    else:
        for token in (
            "version_stamped",
            "applied",
            "conflict",
            "invalid",
            "reconciled",
            "reconcile_failed",
            "latest_events",
        ):
            if token not in replay_state_patch_formatter:
                violations.append(f"LANChatAgentWorker replay state patch formatter missing token: {token}")
    if not replay_guard_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_replay_guard_report not found")
    else:
        for token in (
            "blocked_count",
            "high_risk_confirmation_required_count",
            "write_confirmation_required_count",
            "system_actor_write_blocked_count",
            "user_visible_blocked_event_count",
            "latest_block",
        ):
            if token not in replay_guard_formatter:
                violations.append(f"LANChatAgentWorker replay guard formatter missing token: {token}")
    if not worker_drain_replay_formatter:
        violations.append("runtime_replay_report_policy worker-drain formatter not found")
    else:
        for token in (
            "requested_count",
            "message_drained_count",
            "status_failed_count",
            "plan_resolve_failed_count",
            "latest_drain_event",
        ):
            if token not in worker_drain_replay_formatter:
                violations.append(f"Runtime worker-drain formatter missing token: {token}")
    if not tool_graph_replay_formatter:
        violations.append("runtime_replay_report_policy tool graph formatter not found")
    else:
        for token in ("started_count", "completed_count", "finalized_count", "queued_count", "blocked_count"):
            if token not in tool_graph_replay_formatter:
                violations.append(f"Runtime tool graph formatter missing token: {token}")
    if not gm_summary_replay_formatter:
        violations.append("runtime_replay_report_policy GM summary formatter not found")
    else:
        for token in (
            "exported_count",
            "failed_count",
            "scene_plan_count",
            "resource_readiness_publish_total",
            "resource_readiness_query_total",
        ):
            if token not in gm_summary_replay_formatter:
                violations.append(f"Runtime GM summary formatter missing token: {token}")
    for token in (
        "write-blocked 1",
        "unconfirmed 1",
        "risk medium:1",
        "latest write-confirmation-required risk:medium/write/unconfirmed",
    ):
        if token not in runtime_guard_test_source:
            violations.append(f"LANChat replay guard formatter regression missing token: {token}")
    if not replay_plan_lifecycle_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_replay_plan_lifecycle_report not found")
    else:
        for token in (
            "created_count",
            "confirmed_count",
            "state_persisted_count",
            "state_persist_failed_count",
            "status_persisted_count",
            "status_persist_failed_count",
            "extracted_count",
            "latest_plan_event",
        ):
            if token not in replay_plan_lifecycle_formatter:
                violations.append(f"LANChatAgentWorker replay plan lifecycle formatter missing token: {token}")
    if not replay_intervention_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_replay_intervention_report not found")
    else:
        for token in (
            "routed_count",
            "queued_count",
            "persisted_count",
            "persist_failed_count",
            "skipped_count",
            "enqueue_failed_count",
            "absorbed_count",
            "route_absorbable_count",
            "route_non_absorbable_count",
            "route_requested_item_count",
            "merge_event_count",
            "merged_item_count",
            "merge_absorbed_count",
            "latest_intervention_batch",
        ):
            if token not in replay_intervention_formatter:
                violations.append(f"LANChatAgentWorker replay intervention formatter missing token: {token}")
    if not replay_geometry_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_replay_geometry_report not found")
    else:
        for token in (
            "patch_event_count",
            "fact_count",
            "aabb_actor_count",
            "aabb_skipped_count",
            "overlap_issue_count",
            "status_counts",
            "fact_type_counts",
            "latest_geometry_event",
        ):
            if token not in replay_geometry_formatter:
                violations.append(f"LANChatAgentWorker replay geometry formatter missing token: {token}")
    if not engine_write_readiness_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_engine_write_readiness_report not found")
    else:
        for token in (
            "native_enabled_count",
            "runtime_state_only_count",
            "fallback_count",
            "disabled_count",
            "native_enabled_channels",
            "runtime_state_only_channels",
            "fallback_channels",
            "disabled_channels",
            "runtime-state",
        ):
            if token not in engine_write_readiness_formatter:
                violations.append(f"LANChatAgentWorker engine-write readiness formatter missing token: {token}")

    if not engine_write_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_engine_write_report not found")
    else:
        for token in (
            "status_export_count",
            "latest_status_export",
            "status-export",
            "readiness_mismatch_count",
            "readiness_mismatch_channels",
            "readiness-mismatch",
            "engine_write_bridge_error_code_counts",
            "bridge-failed",
            "engine_write_readiness_native_enabled_count",
            "engine_write_readiness_runtime_state_only_count",
            "engine_write_readiness_fallback_count",
            "engine_write_readiness_disabled_count",
            "engine_write_readiness_native_enabled_channels",
            "engine_write_readiness_runtime_state_only_channels",
            "engine_write_readiness_fallback_channels",
            "engine_write_readiness_disabled_channels",
            "channels ",
            "readiness ",
        ):
            if token not in engine_write_formatter:
                violations.append(f"LANChatAgentWorker engine-write formatter missing status-export token: {token}")
    if not engine_write_boundary_formatter:
        violations.append("runtime_report_policy engine-write boundary formatter not found")
    else:
        for token in (
            "boundary_fact_count",
            "write_source_counts",
            "bridge_call_count",
            "bridge_error_code_counts",
            "native verified",
        ):
            if token not in engine_write_boundary_formatter:
                violations.append(f"runtime_report_policy engine-write boundary formatter missing token: {token}")
    if not replay_runtime_event_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_replay_runtime_event_report not found")
    else:
        for token in (
            "emitted_count",
            "emit_failed_count",
            "disclosure_skipped_count",
            "event_type_counts",
            "latest_runtime_event",
            "latest_disclosure_skip",
            "skipped {skipped_count}",
            "environment_import_failure_code_counts",
            "env-import-failures",
            "engine_write_bridge_error_code_counts",
            "engine-write-failures",
            "engine_write_readiness_mismatch_count",
            "engine_write_readiness_mismatch_channels",
            "engine-write-mismatch",
        ):
            if token not in replay_runtime_event_formatter:
                violations.append(f"LANChatAgentWorker replay runtime event formatter missing token: {token}")
    if not runtime_event_replay_summary:
        violations.append("AgentRuntime._runtime_event_replay_summary not found")
    else:
        for token in (
            "runtime_system_event_disclosure_skipped",
            "disclosure_skipped_count",
            "latest_disclosure_skip",
            "layout_applied_delta_count",
            "layout_skipped_delta_count",
            "layout_transform_result_count",
            "layout_ground_snapped_count",
            "layout_overlap_resolved_count",
            "environment_import_failure_code_counts",
            "engine_write_boundary_fact_count",
            "engine_write_bridge_failed_count",
            "engine_write_bridge_error_code_counts",
            "engine_write_readiness_mismatch_count",
            "engine_write_readiness_mismatch_channels",
        ):
            if token not in runtime_event_replay_summary:
                violations.append(f"AgentRuntime runtime event replay summary missing token: {token}")
    if not runtime_guard_replay_summary:
        violations.append("AgentRuntime._runtime_guard_replay_summary not found")
    else:
        for token in (
            "risk_level_counts",
            "requires_write_blocked_count",
            "confirmed_blocked_count",
            "unconfirmed_blocked_count",
            'payload.get("risk_level")',
            'payload.get("requires_write")',
            'payload.get("confirmed")',
        ):
            if token not in runtime_guard_replay_summary:
                violations.append(f"AgentRuntime RuntimeGuard replay summary missing blocked-call audit token: {token}")
    if not replay_failure_strategy_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_replay_failure_strategy_report not found")
    else:
        for token in (
            "retry_scheduled_count",
            "dependency_skipped_count",
            "abandoned_late_result_count",
            "handler_failed_count",
            "invalid_result_count",
            "invalid_state_patch_count",
            "state_patch_conflict_count",
            "stopped_by_runtime_command_count",
            "latest_strategy_event",
        ):
            if token not in replay_failure_strategy_formatter:
                violations.append(f"LANChatAgentWorker replay failure strategy formatter missing token: {token}")
    if not replay_layout_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_replay_layout_report not found")
    else:
        for token in (
            "request_count",
            "request_failed_count",
            "confirmation_count",
            "confirmation_failed_count",
            "applied_count",
            "transform_success_count",
            "transform_failed_count",
            "ground_snapped_count",
            "overlap_resolved_count",
            "delta_count",
            "latest_graph_status",
        ):
            if token not in replay_layout_formatter:
                violations.append(f"LANChatAgentWorker replay layout formatter missing token: {token}")
    if not replay_final_adjustment_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_replay_final_adjustment_report not found")
    else:
        for token in (
            "confirmation_count",
            "confirmation_failed_count",
            "confirmation_skipped_count",
            "decision_counts",
            "latest_confirmation",
            "conflict_item_count",
        ):
            if token not in replay_final_adjustment_formatter:
                violations.append(
                    "LANChatAgentWorker replay final adjustment formatter missing token: "
                    f"{token}"
                )
    if not replay_vlm_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_replay_vlm_report not found")
    else:
        for token in (
            "checkpoint_count",
            "advisory_count",
            "status_counts",
            "checkpoint_counts",
            "latest_checkpoints",
        ):
            if token not in replay_vlm_formatter:
                violations.append(f"LANChatAgentWorker replay VLM formatter missing token: {token}")
    if not replay_environment_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_replay_environment_report not found")
    else:
        for token in (
            "ready_event_count",
            "failed_event_count",
            "import_event_count",
            "import_failed_event_count",
            "event_type_counts",
            "latest_event_type",
        ):
            if token not in replay_environment_formatter:
                violations.append(f"LANChatAgentWorker replay environment formatter missing token: {token}")
    if not replay_readiness_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_replay_resource_readiness_report not found")
    else:
        for token in (
            "status_query_count",
            "published_count",
            "publish_failed_count",
            "readiness_event_count",
            "publish_requested_total",
            "publish_enabled_total",
            "publish_unavailable_total",
            "publish_status_counts",
            "status_query_requested_total",
            "status_query_enabled_total",
            "status_query_unavailable_total",
            "status_query_status_counts",
            "status_counts",
            "latest_readiness_event",
            "def safe_label",
            "provider",
            "url",
            "safe_label(key)",
        ):
            if token not in replay_readiness_formatter:
                violations.append(f"LANChatAgentWorker replay resource readiness formatter missing token: {token}")
    if not resource_readiness_replay_summary:
        violations.append("AgentRuntime._resource_readiness_replay_summary not found")
    for token in (
        '"publish_requested_total"',
        '"publish_enabled_total"',
        '"publish_unavailable_total"',
        '"publish_status_counts"',
        '"latest_publish_event"',
        '"status_query_requested_total"',
        '"status_query_enabled_total"',
        '"status_query_unavailable_total"',
        '"status_query_status_counts"',
        '"latest_provider_status_query"',
    ):
        if token not in resource_readiness_replay_summary:
            violations.append(f"AgentRuntime._resource_readiness_replay_summary missing token: {token}")
    if not replay_sync_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_sync_replay_report not found")
    else:
        for token in (
            "recorded_count",
            "failed_count",
            "actor_transform_count",
            "transfer_progress_count",
            "latest_transfer_progress",
            "failure_code_counts",
            "latest_failure_code",
            "failure codes",
        ):
            if token not in replay_sync_formatter:
                violations.append(f"LANChatAgentWorker sync replay formatter missing token: {token}")
    if not replay_asset_transfer_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_replay_asset_transfer_report not found")
    else:
        for token in (
            "asset_event_count",
            "asset_transfer_started_count",
            "asset_transfer_progress_count",
            "asset_transfer_completed_count",
            "asset_transfer_failed_count",
            "peer_asset_ready_count",
            "latest_transfer_progress",
        ):
            if token not in replay_asset_transfer_formatter:
                violations.append(f"LANChatAgentWorker asset transfer replay formatter missing token: {token}")
        for forbidden in ("latest_asset_id", "latest_peer_id", "asset_path", "provider", "prompt", "url", "raw"):
            if forbidden in replay_asset_transfer_formatter:
                violations.append(
                    "LANChatAgentWorker asset transfer replay formatter must not expose internal token: "
                    f"{forbidden}"
                )
    if not replay_peer_sync_formatter:
        violations.append("LANChatAgentWorker._format_agent_runtime_replay_peer_sync_report not found")
    else:
        for token in (
            "peer_event_count",
            "peer_join_count",
            "peer_leave_count",
            "room_close_count",
            "sync_reconcile_count",
            "sync_reconcile_failed_count",
            "state_reconcile_count",
            "state_reconcile_failed_count",
        ):
            if token not in replay_peer_sync_formatter:
                violations.append(f"LANChatAgentWorker peer sync replay formatter missing token: {token}")
        for forbidden in ("latest_peer_id", "peer_id", "message_id", "provider", "prompt", "url", "raw"):
            if forbidden in replay_peer_sync_formatter:
                violations.append(
                    "LANChatAgentWorker peer sync replay formatter must not expose internal token: "
                    f"{forbidden}"
                )
    if not sync_actor_rows_formatter:
        violations.append("runtime_sync_policy.format_agent_runtime_sync_actor_rows not found")
    else:
        for token in ("rows[:5]", "actor_name", "actor_id", "lifecycle_status"):
            if token not in sync_actor_rows_formatter:
                violations.append(f"Runtime Sync actor formatter missing token: {token}")
    if not sync_asset_rows_formatter:
        violations.append("runtime_sync_policy.format_agent_runtime_sync_asset_rows not found")
    else:
        for token in ("rows[:5]", "asset_id", "transfer_status", "bytes_transferred", "total_bytes"):
            if token not in sync_asset_rows_formatter:
                violations.append(f"Runtime Sync asset formatter missing token: {token}")
    if not sync_health_formatter:
        violations.append("runtime_sync_policy.format_agent_runtime_sync_health_report not found")
    else:
        for token in ("needs_attention", "actor_create_count", "latest_active_actor_count", "room_close_count"):
            if token not in sync_health_formatter:
                violations.append(f"Runtime Sync health formatter missing token: {token}")
    if not sync_report_formatter:
        violations.append("runtime_sync_policy.format_agent_runtime_sync_report not found")
    else:
        for token in ("event_count", "actor_event_count", "asset_event_count", "latest_actors", "latest_assets"):
            if token not in sync_report_formatter:
                violations.append(f"Runtime Sync report formatter missing token: {token}")
    if not gm_sync_replay_formatter:
        violations.append("runtime_sync_policy.format_agent_runtime_gm_sync_replay_digest not found")
    else:
        for token in (
            "recorded_count",
            "asset_transfer_progress_count",
            "peer_join_count",
            "sync_reconcile_count",
            "failure_code_counts",
        ):
            if token not in gm_sync_replay_formatter:
                violations.append(f"Runtime GM Sync replay formatter missing token: {token}")
    if not sync_asset_transfer_formatter:
        violations.append("runtime_sync_policy.format_agent_runtime_asset_transfer_report not found")
    else:
        for token in ("asset_count", "ready_count", "overall_progress", "latest_assets"):
            if token not in sync_asset_transfer_formatter:
                violations.append(f"Runtime Sync transfer formatter missing token: {token}")
    if not gm_summary_reply:
        violations.append("LANChatAgentWorker._agent_runtime_gm_summary_reply not found")
    else:
        for token in (
            "resource_flow_digest",
            "_format_agent_runtime_resource_flow_report",
            "sync_replay_digest",
            "_format_agent_runtime_gm_sync_replay_digest",
            "runtime_event_replay_digest",
            "_format_agent_runtime_gm_runtime_event_replay_digest",
            "RuntimeEvent replay",
        ):
            if token not in gm_summary_reply:
                violations.append(f"LANChatAgentWorker GM summary reply missing Runtime digest token: {token}")
        if not gm_sync_replay_forwarder:
            violations.append("LANChatAgentWorker._format_agent_runtime_gm_sync_replay_digest not found")
        else:
            if "format_agent_runtime_gm_sync_replay_digest" not in gm_sync_replay_forwarder:
                violations.append("LANChatAgentWorker GM sync replay formatter missing policy forwarder")
        for token in (
            "batch_tooling_digest",
            "_format_agent_runtime_batch_tooling_report",
            "Batch tooling",
        ):
            if token not in gm_summary_reply:
                violations.append(f"LANChatAgentWorker GM summary reply missing batch tooling token: {token}")
        for token in (
            "state_patch_digest",
            "_format_agent_runtime_replay_state_patch_report",
            "StatePatch",
            "tool_failure_strategy_digest",
            "_format_agent_runtime_replay_failure_strategy_report",
            "Failure strategy",
            "runtime_guard_digest",
            "_format_agent_runtime_replay_guard_report",
            "RuntimeGuard",
            "scene_plan_lifecycle_digest",
            "_format_agent_runtime_replay_plan_lifecycle_report",
            "Plan lifecycle",
            "engine_write_digest",
            "_format_agent_runtime_engine_write_report",
            "Engine write",
            "engine_write_readiness_digest",
            "engine_write_readiness_text",
            "_format_agent_runtime_engine_write_readiness_report",
            "Engine write readiness",
            "message_delivery_digest",
            "_format_agent_runtime_message_delivery_report",
            "Message delivery",
        ):
            if token not in gm_summary_reply:
                violations.append(f"LANChatAgentWorker GM summary reply missing runtime health token: {token}")
        for token in (
            "tool_queue_health_digest",
            "_format_agent_runtime_tool_queue_health_report",
            "Runtime queue",
        ):
            if token not in gm_summary_reply:
                violations.append(f"LANChatAgentWorker GM summary reply missing queue health token: {token}")
        for token in (
            "tool_execution_digest",
            "tool_execution_text",
            "_format_agent_runtime_tool_execution_digest_report",
            "Tool execution",
        ):
            if token not in gm_summary_reply:
                violations.append(f"LANChatAgentWorker GM summary reply missing tool execution token: {token}")
        for token in (
            "vlm_checkpoint_digest",
            "review_advisory_replay_digest",
            "_format_agent_runtime_replay_vlm_report",
            "_format_agent_runtime_replay_review_advisory_report",
            "VLM replay",
            "Review advisory replay",
        ):
            if token not in gm_summary_reply:
                violations.append(f"LANChatAgentWorker GM summary reply missing VLM/review replay token: {token}")
        for token in (
            "scene_design_contract_digest",
            "scene_contract_text",
            "_format_agent_runtime_scene_contract_report",
            "Scene contract",
        ):
            if token not in gm_summary_reply:
                violations.append(f"LANChatAgentWorker GM summary reply missing scene contract token: {token}")
        for token in (
            "semantic_arbitration_digest",
            "semantic_arbitration_text",
            "_format_agent_runtime_semantic_arbitration_report",
            "Semantic arbitration",
        ):
            if token not in gm_summary_reply:
                violations.append(f"LANChatAgentWorker GM summary reply missing semantic arbitration token: {token}")
        for token in (
            "scene_snapshot_digest",
            "scene_snapshot_text",
            "_format_agent_runtime_scene_snapshot_report",
            "Scene snapshot",
            "resource_stage_digest",
            "runtime_resource_text",
            "_format_agent_runtime_resource_stage_report",
            "Runtime resources",
            "import_stage_digest",
            "import_text",
            "_format_agent_runtime_import_stage_report",
            "Import",
        ):
            if token not in gm_summary_reply:
                violations.append(f"LANChatAgentWorker GM summary reply missing scene/resource/import token: {token}")
        for token in (
            "geometry_fact_digest",
            "geometry_text",
            "_format_agent_runtime_geometry_fact_report",
            "Geometry facts",
        ):
            if token not in gm_summary_reply:
                violations.append(f"LANChatAgentWorker GM summary reply missing geometry fact token: {token}")
    if not gm_runtime_event_replay_formatter:
        violations.append("runtime_replay_event_policy GM runtime event digest not found")
    else:
        for token in (
            "emitted_count",
            "emit_failed_count",
            "disclosure_skipped_count",
            "latest_disclosure_skip",
            "latest-skip",
            "environment_import_failure_code_counts",
            "env-import-failures",
            "engine_write_bridge_error_code_counts",
            "engine-write-failures",
            "engine_write_readiness_mismatch_count",
            "engine_write_readiness_mismatch_channels",
            "engine-write-mismatch",
        ):
            if token not in gm_runtime_event_replay_formatter:
                violations.append(f"Runtime GM runtime event replay formatter missing token: {token}")
    if not runtime_status_reply:
        violations.append("LANChatAgentWorker._agent_runtime_status_reply not found")
    else:
        for token in (
            "batch_resource_flow_summary",
            "resource_flow_text",
            "_format_agent_runtime_resource_flow_report",
            "asset_transfer_replay_summary",
            "_format_agent_runtime_replay_asset_transfer_report",
            "peer_sync_replay_summary",
            "_format_agent_runtime_replay_peer_sync_report",
            "runtime_event_replay_summary",
            "_format_agent_runtime_replay_runtime_event_report",
            "RuntimeEvent replay",
            "gm_summary_replay_summary",
            "gm_summary_replay_text",
            "_format_agent_runtime_gm_summary_replay_report",
            "GM replay",
            "batch_execution_replay_summary",
            "tool_graph_queue_replay_summary",
            "_format_agent_runtime_tool_graph_replay_report",
            "ToolGraph replay",
        ):
            if token not in runtime_status_reply:
                violations.append(f"LANChatAgentWorker Runtime status reply missing resource flow token: {token}")
        for token in (
            "batch_tooling_summary",
            "batch_tooling_text",
            "_format_agent_runtime_batch_tooling_report",
            "Batch tooling",
        ):
            if token not in runtime_status_reply:
                violations.append(f"LANChatAgentWorker Runtime status reply missing batch tooling token: {token}")
        for token in (
            "state_patch_summary",
            "state_patch_text",
            "_format_agent_runtime_replay_state_patch_report",
            "StatePatch",
            "tool_failure_strategy_summary",
            "failure_strategy_text",
            "_format_agent_runtime_replay_failure_strategy_report",
            "Failure strategy",
            "runtime_guard_replay_summary",
            "runtime_guard_text",
            "_format_agent_runtime_replay_guard_report",
            "RuntimeGuard",
            "scene_plan_lifecycle_summary",
            "plan_lifecycle_text",
            "_format_agent_runtime_replay_plan_lifecycle_report",
            "Plan lifecycle",
        ):
            if token not in runtime_status_reply:
                violations.append(f"LANChatAgentWorker Runtime status reply missing state/failure token: {token}")
        for token in (
            "tool_queue_health_summary",
            "tool_queue_health_text",
            "_format_agent_runtime_tool_queue_health_report",
            "Runtime queue",
        ):
            if token not in runtime_status_reply:
                violations.append(f"LANChatAgentWorker Runtime status reply missing queue health token: {token}")
        for token in (
            "tool_execution_digest",
            "tool_execution_text",
            "_format_agent_runtime_tool_execution_digest_report",
            "Tool execution",
        ):
            if token not in runtime_status_reply:
                violations.append(f"LANChatAgentWorker Runtime status reply missing tool execution token: {token}")
        for token in (
            "vlm_checkpoint_summary",
            "vlm_checkpoint_text",
            "_format_agent_runtime_replay_vlm_report",
            "VLM replay",
            "review_advisory_replay_summary",
            "review_advisory_replay_text",
            "_format_agent_runtime_replay_review_advisory_report",
            "Review advisory replay",
        ):
            if token not in runtime_status_reply:
                violations.append(f"LANChatAgentWorker Runtime status reply missing VLM/review replay token: {token}")
        for token in (
            "scene_design_contract_summary",
            "scene_contract_text",
            "_format_agent_runtime_scene_contract_report",
        ):
            if token not in runtime_status_reply:
                violations.append(f"LANChatAgentWorker Runtime status reply missing scene contract token: {token}")
        for token in (
            "semantic_arbitration_summary",
            "semantic_arbitration_text",
            "_format_agent_runtime_semantic_arbitration_report",
        ):
            if token not in runtime_status_reply:
                violations.append(f"LANChatAgentWorker Runtime status reply missing semantic arbitration token: {token}")
        for token in (
            "scene_snapshot_summary",
            "scene_snapshot_text",
            "_format_agent_runtime_scene_snapshot_report",
            "resource_summary",
            "runtime_resource_text",
            "_format_agent_runtime_resource_stage_report",
            "engine_write_readiness_summary",
            "engine_write_readiness_text",
            "_format_agent_runtime_engine_write_readiness_report",
            "Engine write readiness",
            "import_summary",
            "import_text",
            "_format_agent_runtime_import_stage_report",
        ):
            if token not in runtime_status_reply:
                violations.append(f"LANChatAgentWorker Runtime status reply missing scene/resource/import token: {token}")
        for token in (
            "geometry_fact_summary",
            "geometry_text",
            "_format_agent_runtime_geometry_fact_report",
        ):
            if token not in runtime_status_reply:
                violations.append(f"LANChatAgentWorker Runtime status reply missing geometry fact token: {token}")

    if violations:
        print("[FAIL] static AgentRuntime flag boundary gate")
        for item in violations:
            _print_violation(item)
        return False
    print("[OK]  static AgentRuntime flag boundary gate")
    return True


def _runtime_state_apply_patch_boundary_gate() -> bool:
    print("[RUN] static RuntimeState apply_patch boundary gate")
    core_path = REPO_ROOT / AGENT_RUNTIME_CORE
    try:
        with tokenize.open(core_path) as handle:
            source = handle.read()
    except Exception as exc:
        print(f"[FAIL] static RuntimeState apply_patch boundary gate: cannot read {AGENT_RUNTIME_CORE}: {exc}")
        return False
    tree = ast.parse(source)
    parents: dict[ast.AST, ast.AST] = {}
    for parent in ast.walk(tree):
        for child in ast.iter_child_nodes(parent):
            parents[child] = parent

    def enclosing_function(node: ast.AST) -> str:
        current = parents.get(node)
        while current is not None:
            if isinstance(current, (ast.FunctionDef, ast.AsyncFunctionDef)):
                return current.name
            current = parents.get(current)
        return "<module>"

    violations: list[str] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        func = node.func
        if not isinstance(func, ast.Attribute) or func.attr != "apply_patch":
            continue
        owner = enclosing_function(node)
        if owner not in ALLOWED_RUNTIME_STATE_APPLY_PATCH_FUNCTIONS:
            violations.append(
                f"{AGENT_RUNTIME_CORE}:{getattr(node, 'lineno', '?')}: "
                f"RuntimeState.apply_patch call outside allowed executor boundary: {owner}"
            )
    if violations:
        print("[FAIL] static RuntimeState apply_patch boundary gate")
        for item in violations:
            _print_violation(item)
        return False
    print("[OK]  static RuntimeState apply_patch boundary gate")
    return True


def _extract_policy_command_set(source: str, name: str) -> set[str]:
    tree = ast.parse(source)
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign):
            continue
        if not any(isinstance(target, ast.Name) and target.id == name for target in node.targets):
            continue
        value = node.value
        if isinstance(value, ast.Call) and isinstance(value.func, ast.Name) and value.func.id == "frozenset":
            if not value.args:
                return set()
            value = value.args[0]
        if isinstance(value, (ast.Set, ast.List, ast.Tuple)):
            return {
                str(item.value).strip().lower()
                for item in value.elts
                if isinstance(item, ast.Constant) and isinstance(item.value, str)
            }
    return set()


def _extract_workflow_commands(path: Path) -> set[str]:
    try:
        source = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        source = path.read_text(encoding="utf-8-sig")
    tree = ast.parse(source)
    commands: set[str] = set()
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign):
            continue
        if not any(isinstance(target, ast.Name) and target.id == "WORKFLOW_COMMANDS" for target in node.targets):
            continue
        if not isinstance(node.value, ast.Dict):
            continue
        for key in node.value.keys:
            if isinstance(key, ast.Constant) and isinstance(key.value, str):
                value = key.value.strip().lower()
                if value:
                    commands.add(value if value.startswith("/") else f"/{value}")
    return commands


def _iter_workflow_command_files() -> list[Path]:
    files: list[Path] = []
    for root in WORKFLOW_COMMAND_SCAN_ROOTS:
        root_path = REPO_ROOT / root
        if root_path.is_file():
            files.append(root_path)
            continue
        if root_path.is_dir():
            for path in root_path.rglob("*.py"):
                rel_parts = path.relative_to(REPO_ROOT).parts
                if "tests" in rel_parts or path.name.startswith("test_"):
                    continue
                files.append(path)
    return sorted(set(files))


def _workflow_command_exposure_gate() -> bool:
    print("[RUN] static workflow command exposure gate")
    policy_path = REPO_ROOT / "editor/plugins/AITool/services/workflow_command_policy.py"
    register_path = REPO_ROOT / "editor/plugins/AITool/cai_extensions/register.py"
    try:
        policy_source = policy_path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        policy_source = policy_path.read_text(encoding="utf-8-sig")
    try:
        register_source = register_path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        register_source = register_path.read_text(encoding="utf-8-sig")
    deprecated = _extract_policy_command_set(policy_source, "DEPRECATED_USER_WORKFLOW_COMMANDS")
    internal = _extract_policy_command_set(policy_source, "INTERNAL_DEBUG_WORKFLOW_COMMANDS")

    violations: list[str] = []
    missing_deprecated = sorted(REQUIRED_DEPRECATED_WORKFLOW_COMMANDS - deprecated)
    if missing_deprecated:
        violations.append(
            "workflow_command_policy.py missing deprecated commands: " + ", ".join(missing_deprecated)
        )
    missing_internal = sorted(REQUIRED_INTERNAL_WORKFLOW_COMMANDS - internal)
    if missing_internal:
        violations.append(
            "workflow_command_policy.py missing internal/debug commands: " + ", ".join(missing_internal)
        )

    for path in _iter_workflow_command_files():
        commands = _extract_workflow_commands(path)
        if not commands:
            continue
        rel = _to_repo_path(path)
        for command in sorted(commands & REQUIRED_DEPRECATED_WORKFLOW_COMMANDS):
            if command not in deprecated:
                violations.append(f"{rel}: {command} appears in WORKFLOW_COMMANDS but is not deprecated")
        for command in sorted(commands & REQUIRED_INTERNAL_WORKFLOW_COMMANDS):
            if command not in internal:
                violations.append(f"{rel}: {command} appears in WORKFLOW_COMMANDS but is not internal/debug")

    required_policy_tokens = (
        'if exposure == "deprecated":',
        "return False",
        "should_execute_workflow_function",
        "install_workflow_function_policy",
        "get_with_policy",
        "has_with_policy",
        "list_function_ids_with_policy",
    )
    if not all(token in policy_source for token in required_policy_tokens):
        violations.append("workflow_command_policy.py is missing hidden workflow function execution guards")

    workflow_plugin_source = register_source[
        register_source.find("class CabbageWorkflowPlugin:"):
        register_source.find("class CabbageWorkflowSyncPlugin:")
    ]
    register_function = _function_source(workflow_plugin_source, "register")
    if not register_function:
        violations.append("cai_extensions/register.py CabbageExtension.register not found")
    else:
        required_order = (
            "registry = runtime.get_registry(\"workflow\")",
            "command_registry = runtime.get_registry(\"workflow_command\")",
            "install_workflow_command_policy(command_registry)",
            "install_workflow_function_policy(registry, command_registry)",
            "for module_name in self.flow_modules:",
            "registry.register(function_id, graph, overwrite=True)",
            "record_workflow_function_exposure(command_registry, command, function_id)",
            "if not should_register_workflow_command(command):",
            "command_registry.register(command, function_id, overwrite=True)",
        )
        last_index = -1
        for token in required_order:
            token_index = register_function.find(token)
            if token_index < 0:
                violations.append(f"cai_extensions/register.py register missing workflow policy token: {token}")
                continue
            if token_index < last_index:
                violations.append(f"cai_extensions/register.py register policy token out of order: {token}")
            last_index = token_index

    if violations:
        print("[FAIL] static workflow command exposure gate")
        for item in violations:
            _print_violation(item)
        return False
    print("[OK]  static workflow command exposure gate")
    return True


def _function_source(source: str, function_name: str) -> str:
    for indent in ("    ", ""):
        marker = f"{indent}def {function_name}("
        start = source.find(marker)
        if start < 0:
            continue
        next_candidates = [
            index
            for index in (
                source.find(f"\n{indent}def ", start + len(marker)),
                source.find(f"\n{indent}@", start + len(marker)),
            )
            if index >= 0
        ]
        next_start = min(next_candidates) if next_candidates else -1
        return source[start:] if next_start < 0 else source[start:next_start]
    return ""


def _runtime_report_fact_source_gate() -> bool:
    print("[RUN] static Runtime report fact-source gate")
    core_path = REPO_ROOT / AGENT_RUNTIME_CORE
    try:
        source = core_path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        source = core_path.read_text(encoding="utf-8-sig")
    phase1_test_path = REPO_ROOT / "editor/plugins/AITool/services/tests/test_agent_runtime_phase1.py"
    try:
        phase1_test_source = phase1_test_path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        phase1_test_source = phase1_test_path.read_text(encoding="utf-8-sig")
    generate_report = _function_source(source, "generate_report")
    operation_replay = _function_source(source, "operation_replay")
    compose_operation_replay = _function_source(source, "_compose_operation_replay")
    operation_replay_snapshot = _function_source(source, "_operation_replay_snapshot_via_tool_graph")
    record_operation_replay_snapshot = _function_source(source, "_record_operation_replay_snapshot_tool")
    operation_replay_snapshot_payload = _function_source(source, "_operation_replay_snapshot_audit_payload")
    record_operation_replay_summary = _function_source(source, "_record_operation_replay_summary_tool")
    operation_replay_summary_snapshot = _function_source(source, "_operation_replay_summary_via_tool_graph")
    operation_replay_summary_for_report = _function_source(source, "_operation_replay_summary_for_report")
    operation_replay_summary_payload = _function_source(source, "_operation_replay_snapshot_summary_payload")
    tool_manifest = _function_source(source, "tool_manifest")
    provider_status = _function_source(source, "provider_status")
    provider_status_snapshot = _function_source(source, "_provider_status_snapshot_via_tool_graph")
    gm_summary = _function_source(source, "gm_summary")
    gm_summary_snapshot = _function_source(source, "_gm_summary_snapshot_via_tool_graph")
    runtime_events_snapshot = _function_source(source, "_runtime_events_snapshot_via_tool_graph")
    sync_status_snapshot = _function_source(source, "_sync_status_snapshot_via_tool_graph")
    execute_scene_plan = _function_source(source, "execute_scene_plan")
    enqueue_scene_plan = _function_source(source, "enqueue_scene_plan")
    enqueue_planned_batches = _function_source(source, "enqueue_planned_batches")
    enqueue_pending_intervention_batch = _function_source(source, "enqueue_pending_intervention_batch")
    build_batch_execution_graph = _function_source(source, "_build_batch_execution_graph")
    handle_message = _function_source(source, "handle_message")
    apply_runtime_command = _function_source(source, "apply_runtime_command")
    status_summary = _function_source(source, "status_summary")
    tool_manifest = _function_source(source, "tool_manifest")
    status_summary_snapshot = _function_source(source, "_status_summary_snapshot_via_tool_graph")
    gm_summary_snapshot = _function_source(source, "_gm_summary_snapshot_via_tool_graph")
    runtime_events_snapshot = _function_source(source, "_runtime_events_snapshot_via_tool_graph")
    sync_status_snapshot = _function_source(source, "_sync_status_snapshot_via_tool_graph")
    provider_status_snapshot = _function_source(source, "_provider_status_snapshot_via_tool_graph")
    operation_replay_snapshot = _function_source(source, "_operation_replay_snapshot_via_tool_graph")
    record_status_summary_snapshot = _function_source(source, "_record_status_summary_snapshot_tool")
    snapshot_failure_payload = _function_source(source, "_snapshot_failure_audit_payload")
    batch_resource_lifecycle_replay = _function_source(source, "_batch_resource_lifecycle_replay_summary")
    intervention_batch_replay = _function_source(source, "_intervention_batch_replay_summary")
    gm_summary_replay = _function_source(source, "_gm_summary_replay_summary")
    final_adjustment_confirmation_replay = _function_source(
        source,
        "_final_adjustment_confirmation_replay_summary",
    )
    violations: list[str] = []

    if not generate_report:
        violations.append("AgentRuntime.generate_report not found")
    else:
        required_order = [
            "_operation_replay_summary_via_tool_graph",
            "_classification_summary_for_plan",
            '"operation_replay_summary": operation_replay_summary',
            '"user_report_generated"',
            "_persist_user_report",
            'event_type="report_ready"',
        ]
        positions: list[int] = []
        for token in required_order:
            pos = generate_report.find(token)
            if pos < 0:
                violations.append(f"AgentRuntime.generate_report missing required fact/report token: {token}")
            positions.append(pos)
        if all(pos >= 0 for pos in positions) and positions != sorted(positions):
            violations.append(
                "AgentRuntime.generate_report must derive OperationLog/RuntimeState summaries "
                "before logging, persisting, and emitting the user report"
            )
        if "_operation_replay_summary_for_report(" in generate_report:
            violations.append(
                "AgentRuntime.generate_report must use runtime.report.operation_replay_summary "
                "ToolCallGraph instead of directly composing OperationLog replay facts"
            )
        if "intervention_digest = self._intervention_digest_for_report" not in generate_report:
            violations.append("AgentRuntime.generate_report missing Runtime intervention digest token")
        if '"sync_health_digest": sync_health_digest' not in generate_report:
            violations.append("AgentRuntime.generate_report missing Runtime sync health digest token")
        if "_sync_health_digest_for_report(" not in generate_report:
            violations.append("AgentRuntime.generate_report must derive sync health from Runtime sync summaries")
        if '"runtime_guard_replay_summary": dict(operation_replay_summary.get("runtime_guard_replay_summary") or {})' not in generate_report:
            violations.append("AgentRuntime.generate_report missing RuntimeGuard replay summary token")
        for required in (
            '"requires_write_blocked_count": int(runtime_guard_replay_summary.get("requires_write_blocked_count") or 0)',
            '"confirmed_blocked_count": int(runtime_guard_replay_summary.get("confirmed_blocked_count") or 0)',
            '"unconfirmed_blocked_count": int(runtime_guard_replay_summary.get("unconfirmed_blocked_count") or 0)',
            '"risk_level_counts": {',
            '"risk_level": str(latest_guard_block.get("risk_level") or "")',
            '"requires_write": bool(latest_guard_block.get("requires_write"))',
            '"confirmed": bool(latest_guard_block.get("confirmed"))',
        ):
            if required not in source:
                violations.append(f"AgentRuntime GM runtime_guard_digest missing blocked-call audit token: {required}")
        if '"scene_plan_lifecycle_summary": dict(operation_replay_summary.get("scene_plan_lifecycle_summary") or {})' not in generate_report:
            violations.append("AgentRuntime.generate_report missing ScenePlan lifecycle summary token")
        if '"vlm_checkpoint_summary": dict(operation_replay_summary.get("vlm_checkpoint_summary") or {})' not in generate_report:
            violations.append("AgentRuntime.generate_report missing VLM checkpoint replay summary token")
        if '"review_advisory_replay_summary": dict(operation_replay_summary.get("review_advisory_summary") or {})' not in generate_report:
            violations.append("AgentRuntime.generate_report missing review advisory replay summary token")
        if (
            '"final_adjustment_confirmation_replay_summary": dict(' not in generate_report
            or 'operation_replay_summary.get("final_adjustment_confirmation_replay_summary") or {}'
            not in generate_report
        ):
            violations.append("AgentRuntime.generate_report missing final adjustment confirmation replay summary token")
        for required in (
            "scene_design_contract_summary = self._scene_design_contract_summary_for_plan",
            '"scene_design_contract_summary": scene_design_contract_summary',
            "semantic_arbitration_summary = self._semantic_arbitration_digest_for_report",
            '"semantic_arbitration_summary": semantic_arbitration_summary',
            "scene_snapshot_summary = self._scene_snapshot_summary_for_plan",
            '"scene_snapshot_summary": scene_snapshot_summary',
            "geometry_fact_summary = self._geometry_fact_summary_for_plan",
            '"geometry_fact_summary": geometry_fact_summary',
            "resource_summary = self._resource_summary_for_plan",
            '"resource_summary": resource_summary',
            "import_summary = self._import_summary_for_plan",
            '"import_summary": import_summary',
            "scene_entity_registry = self._scene_entity_registry_for_plan",
            '"scene_entity_registry": scene_entity_registry',
            "sync_readiness_summary = self._sync_readiness_summary",
            '"sync_readiness_summary": sync_readiness_summary',
            "scene_entity_registry=scene_entity_registry",
            "environment_component_summary = self._environment_component_summary_for_plan",
            '"environment_component_summary": environment_component_summary',
            "environment_component_summary=environment_component_summary",
            "provider_readiness_from_state = self._provider_readiness_from_state_room(room)",
            "engine_write_readiness_summary = self._engine_write_readiness_summary(provider_readiness_from_state)",
            "engine_write_adapter_summary = self._engine_write_adapter_summary",
            '"engine_write_readiness_summary": engine_write_readiness_summary',
            '"engine_write_adapter_summary": engine_write_adapter_summary',
            "tool_execution_digest = self._tool_execution_digest_for_report",
            '"tool_execution_digest": tool_execution_digest',
            '"layout_applied_delta_count": int(layout_adjustment_summary.get("applied_delta_count") or 0)',
            '"layout_skipped_delta_count": int(layout_adjustment_summary.get("skipped_delta_count") or 0)',
            '"layout_transform_result_count": int(layout_adjustment_summary.get("transform_result_count") or 0)',
            '"layout_ground_snapped_count": int(layout_adjustment_summary.get("ground_snapped_count") or 0)',
            '"layout_overlap_resolved_count": int(layout_adjustment_summary.get("overlap_resolved_count") or 0)',
            '"layout_transform_failure_code_counts": dict(',
            '"sync_failure_code_counts": dict(sorted(sync_failure_code_counts.items()))',
            '"latest_sync_failure_code": latest_sync_failure_code',
        ):
            if required not in generate_report:
                violations.append(f"AgentRuntime.generate_report missing runtime report token: {required}")
        for required in (
            '"engine_write_pending_f5_count": engine_write_pending_f5_count',
            '"engine_write_verified_count": engine_write_verified_count',
            '"engine_write_verification_status_counts": dict(',
            '"missing_transform_count": missing_transform_count',
            '"missing_aabb_count": missing_aabb_count',
            '"estimated_actor_bounds_count": estimated_actor_bounds_count',
            "environment_component_summary: Mapping[str, Any] | None = None",
            '"environment_import_failed_count": environment_import_failed_count',
            '"environment_import_failure_code_counts": dict(',
            '"environment_import_failed"',
        ):
            if required not in source:
                violations.append(f"AgentRuntime report health missing environment health token: {required}")
        for safe_event_payload_key in (
            '"layout_applied_delta_count"',
            '"layout_skipped_delta_count"',
            '"layout_transform_result_count"',
            '"layout_ground_snapped_count"',
            '"layout_overlap_resolved_count"',
            '"layout_transform_failure_code_counts"',
            '"engine_write_boundary_fact_count"',
            '"engine_write_bridge_call_count"',
            '"engine_write_bridge_success_count"',
            '"engine_write_bridge_failed_count"',
            '"engine_write_bridge_error_code_counts"',
            '"environment_failed_count"',
            '"environment_import_requested_count"',
            '"environment_imported_count"',
            '"environment_import_failed_count"',
            '"environment_import_failure_code_counts"',
            '"sync_failure_code_counts"',
            '"latest_sync_failure_code"',
        ):
            if safe_event_payload_key not in source:
                violations.append(f"AgentRuntime safe runtime event payload keys missing: {safe_event_payload_key}")
        sync_health_tool = _function_source(source, "_sync_health_digest_for_report")
        for required in (
            '"peer_join_count": peer_join_count',
            '"peer_leave_count": peer_leave_count',
            '"room_close_count": room_close_count',
            '"latest_peer_id": latest_peer_id',
            '"latest_peer_event_type": latest_peer_event_type',
            '"latest_room_status": latest_room_status',
            '"actor_create_count": actor_create_count',
            '"actor_transform_count": actor_transform_count',
            '"actor_delete_count": actor_delete_count',
            '"latest_active_actor_count": latest_active_actor_count',
            '"latest_deleted_actor_count": latest_deleted_actor_count',
        ):
            if required not in sync_health_tool:
                violations.append(f"AgentRuntime sync health digest missing actor sync token: {required}")
        report_summary_tool = _function_source(source, "_operation_replay_summary_via_tool_graph")
        if "runtime.report.operation_replay_summary" not in report_summary_tool:
            violations.append(
                "AgentRuntime._operation_replay_summary_via_tool_graph must execute "
                "runtime.report.operation_replay_summary"
            )

    if not tool_manifest:
        violations.append("AgentRuntime.tool_manifest not found")
    else:
        for required in (
            "runtime.tool_manifest.snapshot",
            "custom_report_facts",
            '"runtime_tool_manifest_queried"',
        ):
            if required not in tool_manifest:
                violations.append(f"AgentRuntime.tool_manifest missing Runtime manifest fact token: {required}")
        if "self.registry.manifest(" in tool_manifest or "self.registry.capability_summary(" in tool_manifest:
            violations.append(
                "AgentRuntime.tool_manifest must read ToolRegistry facts through "
                "runtime.tool_manifest.snapshot instead of direct registry access"
            )
        if "test_tool_manifest_snapshot_failure_records_safe_audit_payload" not in phase1_test_source:
            violations.append("AgentRuntime.tool_manifest snapshot failure audit missing regression test")
        for required_test_token in (
            'payload["guard_requires_write_blocked_count"]',
            'payload["guard_unconfirmed_blocked_count"]',
            'payload["guard_risk_level_counts"]',
            'replay_recorded_payload["guard_requires_write_blocked_count"]',
            'replay_recorded_payload["guard_risk_level_counts"]',
            'summary["runtime_guard_digest"]["requires_write_blocked_count"]',
            'gm_snapshot_payload["runtime_guard_requires_write_blocked_count"]',
            'gm_snapshot_payload["runtime_guard_risk_level_counts"]',
        ):
            if required_test_token not in phase1_test_source:
                violations.append(f"AgentRuntime operation replay RuntimeGuard audit regression missing token: {required_test_token}")
        for manifest_contract_token in (
            '"execution_contract": execution_contract',
            '"state_contract": "stateful" if stateful else "stateless"',
            '"confirmation_required": bool(self.requires_write or self.default_risk_level == RiskLevel.HIGH)',
            '"read_only_tool_count"',
            '"stateful_tool_count"',
        ):
            if manifest_contract_token not in source:
                violations.append(
                    f"ToolRegistry manifest missing Agent-native execution contract token: {manifest_contract_token}"
                )

    if not operation_replay:
        violations.append("AgentRuntime.operation_replay not found")
    else:
        for required in (
            "runtime_operation_replay_requested",
            "runtime_operation_replay_queried",
            "_operation_replay_snapshot_via_tool_graph",
            "payload=self._operation_replay_snapshot_audit_payload(replay)",
        ):
            if required not in operation_replay:
                violations.append(f"AgentRuntime.operation_replay missing Runtime replay fact token: {required}")
        if not operation_replay_snapshot or "runtime.operation_replay.snapshot" not in operation_replay_snapshot:
            violations.append(
                "AgentRuntime._operation_replay_snapshot_via_tool_graph must execute "
                "runtime.operation_replay.snapshot"
            )
        if operation_replay_snapshot:
            for required in (
                "payload=self._operation_replay_snapshot_audit_payload(replay_with_evidence)",
                "replay_with_evidence[\"snapshot_recorded\"] = True",
                "replay_with_evidence[\"snapshot_status\"] = str(graph.status or \"\")",
                "replay_with_evidence[\"snapshot_tool_status\"] = str(snapshot_call.status.value)",
                "replay_with_evidence[\"snapshot_state_version\"] = int(self.state.version)",
            ):
                if required not in operation_replay_snapshot:
                    violations.append(
                        "AgentRuntime._operation_replay_snapshot_via_tool_graph missing Runtime replay "
                        f"ToolCall evidence token: {required}"
                    )
        if not operation_replay_snapshot_payload:
            violations.append("AgentRuntime._operation_replay_snapshot_audit_payload not found")
        else:
            for required in (
                '"summary_type": "runtime_operation_replay"',
                '"entry_count": int(replay.get("entry_count") or 0)',
                '"event_counts": dict(sorted(event_counts.items()))',
                '"runtime_event_emitted_count": int(runtime_events.get("emitted_count") or 0)',
                '"runtime_event_emit_failed_count": int(runtime_events.get("emit_failed_count") or 0)',
                '"sync_recorded_count": int(sync_summary.get("recorded_count") or 0)',
                '"sync_failed_count": int(sync_summary.get("failed_count") or 0)',
                '"asset_transfer_failed_count": int(asset_transfer.get("asset_transfer_failed_count") or 0)',
                '"peer_event_count": int(peer_sync.get("peer_event_count") or 0)',
                '"engine_write_import_result_count": int(engine_write.get("import_result_count") or 0)',
                '"engine_write_transform_result_count": int(engine_write.get("transform_result_count") or 0)',
                '"report_health_status": AgentRuntime._safe_report_text',
                '"report_health_attention_required": bool(report_health.get("attention_required"))',
                '"runtime_fact_injection_count": int(runtime_fact_injection.get("injection_event_count") or 0)',
                '"runtime_fact_injection_field_count": int(',
                '"runtime_fact_injection_unique_field_count": len(',
                '"runtime_fact_injection_field_counts": {',
            ):
                if required not in operation_replay_snapshot_payload:
                    violations.append(
                        "AgentRuntime._operation_replay_snapshot_audit_payload missing audit token: "
                        f"{required}"
                    )
            if any(secret in operation_replay_snapshot_payload for secret in ("entries", "prompt", "provider", "asset_path")):
                violations.append(
                    "AgentRuntime._operation_replay_snapshot_audit_payload must stay safe and avoid "
                    "entries/prompt/provider/path fields"
                )
        for function_name, function_source in (
            ("_record_operation_replay_snapshot_tool", record_operation_replay_snapshot),
            ("_operation_replay_snapshot_via_tool_graph", operation_replay_snapshot),
        ):
            if not (
                "payload=self._operation_replay_snapshot_audit_payload(replay)" in function_source
                or "payload=self._operation_replay_snapshot_audit_payload(replay_with_evidence)" in function_source
                or "payload=audit_payload" in function_source
            ):
                violations.append(
                    f"AgentRuntime.{function_name} must record the shared operation replay snapshot audit payload"
                )
        if record_operation_replay_snapshot and "**audit_payload" not in record_operation_replay_snapshot:
            violations.append(
                "AgentRuntime._record_operation_replay_snapshot_tool must persist snapshot audit payload into custom_report_facts"
            )
        if not operation_replay_summary_payload:
            violations.append("AgentRuntime._operation_replay_snapshot_summary_payload not found")
        else:
            for required in (
                '"summary_type": "operation_replay_summary"',
                '"entry_count": int(summary.get("entry_count") or 0)',
                '"runtime_event_emitted_count": int(runtime_events.get("emitted_count") or 0)',
                '"runtime_event_emit_failed_count": int(runtime_events.get("emit_failed_count") or 0)',
                '"runtime_event_report_ready_count": int(runtime_events.get("report_ready_count") or 0)',
                '"sync_recorded_count": int(sync_replay.get("recorded_count") or 0)',
                '"sync_failed_count": int(sync_replay.get("failed_count") or 0)',
                '"sync_actor_transform_count": int(sync_replay.get("actor_transform_count") or 0)',
                '"asset_transfer_failed_count": int(asset_transfer.get("asset_transfer_failed_count") or 0)',
                '"asset_transfer_progress_count": int(asset_transfer.get("asset_transfer_progress_count") or 0)',
                '"peer_event_count": int(peer_sync.get("peer_event_count") or 0)',
                '"sync_reconcile_failed_count": int(peer_sync.get("sync_reconcile_failed_count") or 0)',
                '"gm_summary_exported_count": int(gm_replay.get("exported_count") or 0)',
                '"gm_summary_failed_count": int(gm_replay.get("failed_count") or 0)',
                '"guard_blocked_count": int(guard_replay.get("blocked_count") or 0)',
                '"state_patch_conflict_count": int(state_patch.get("conflict") or 0)',
                '"queue_blocked_count": int(queue_replay.get("blocked_count") or 0)',
                '"failure_retry_scheduled_count": int(failure_strategy.get("retry_scheduled_count") or 0)',
                '"engine_write_import_result_count": int(engine_write.get("import_result_count") or 0)',
                '"engine_write_bridge_failed_count": int(',
                '"runtime_fact_injection_count": int(runtime_fact_injection.get("injection_event_count") or 0)',
                '"runtime_fact_injection_field_count": int(',
                '"runtime_fact_injection_unique_field_count": len(',
                '"runtime_fact_injection_field_counts": {',
            ):
                if required not in operation_replay_summary_payload:
                    violations.append(
                        "AgentRuntime._operation_replay_snapshot_summary_payload missing audit token: "
                        f"{required}"
                    )
            if any(secret in operation_replay_summary_payload for secret in ("prompt", "provider", "asset_path")):
                violations.append(
                    "AgentRuntime._operation_replay_snapshot_summary_payload must stay safe and avoid "
                    "prompt/provider/path fields"
                )
        for function_name, function_source in (
            ("_record_operation_replay_summary_tool", record_operation_replay_summary),
            ("_operation_replay_summary_via_tool_graph", operation_replay_summary_snapshot),
        ):
            if not (
                "payload=self._operation_replay_snapshot_summary_payload(summary)" in function_source
                or "payload=audit_payload" in function_source
            ):
                violations.append(
                    f"AgentRuntime.{function_name} must record the shared operation replay summary audit payload"
                )
        if record_operation_replay_summary and "**audit_payload" not in record_operation_replay_summary:
            violations.append(
                "AgentRuntime._record_operation_replay_summary_tool must persist summary audit payload into custom_report_facts"
            )
        if not operation_replay_summary_for_report or "gm_summary_replay_summary" not in operation_replay_summary_for_report:
            violations.append(
                "AgentRuntime._operation_replay_summary_for_report must include gm_summary_replay_summary"
            )
        if (
            not operation_replay_summary_for_report
            or '"runtime_fact_injection_replay_summary": self._runtime_fact_injection_replay_summary(raw_entries)' not in operation_replay_summary_for_report
        ):
            violations.append(
                "AgentRuntime._operation_replay_summary_for_report must include runtime_fact_injection_replay_summary"
            )
        if (
            not compose_operation_replay
            or 'replay["runtime_fact_injection_replay_summary"] = self._runtime_fact_injection_replay_summary(raw_entries)' not in compose_operation_replay
        ):
            violations.append(
                "AgentRuntime._compose_operation_replay must include runtime_fact_injection_replay_summary"
            )
        if not gm_summary_replay:
            violations.append("AgentRuntime._gm_summary_replay_summary not found")
        else:
            for required in (
                "resource_readiness_publish_total",
                "resource_readiness_publish_failed_total",
                "resource_readiness_query_total",
                "resource_readiness_publish_requested_total",
                "resource_readiness_publish_enabled_total",
                "resource_readiness_publish_unavailable_total",
                "resource_readiness_publish_count",
                "resource_readiness_query_count",
                "engine_write_boundary_fact_total",
                "engine_write_bridge_call_total",
                "engine_write_bridge_success_total",
                "engine_write_bridge_failed_total",
                "engine_write_bridge_error_code_counts",
                "engine_write_bridge_failed_count",
                "sync_actor_transform_total",
                "sync_actor_delete_total",
                "sync_actor_transform_count",
                "sync_actor_delete_count",
            ):
                if required not in gm_summary_replay:
                    violations.append(
                        f"AgentRuntime._gm_summary_replay_summary missing resource readiness token: {required}"
                    )
        if '"geometry_fact_replay_summary": self._geometry_fact_replay_summary' not in operation_replay_summary_for_report:
            violations.append(
                "AgentRuntime._operation_replay_summary_for_report must include geometry_fact_replay_summary"
            )
        if not apply_runtime_command:
            violations.append("AgentRuntime.apply_runtime_command not found")
        else:
            required_order = (
                "self._persist_runtime_command_state(",
                "self.operation_log.append(",
                'f"runtime_{normalized}_command_applied"',
                "self.emit_runtime_event(",
            )
            positions = []
            for token in required_order:
                pos = apply_runtime_command.find(token)
                if pos < 0:
                    violations.append(f"AgentRuntime.apply_runtime_command missing command fact-order token: {token}")
                positions.append(pos)
            if all(pos >= 0 for pos in positions) and positions != sorted(positions):
                violations.append(
                    "AgentRuntime.apply_runtime_command must persist RuntimeState command facts "
                    "before logging replay events and emitting user-visible Runtime events"
                )
            for token in (
                "command_record = self._persist_runtime_command_state(",
                '"command_recorded": bool((command_record or {}).get("command_recorded"))',
                '"graph_status": str((command_record or {}).get("graph_status") or "")',
                '"tool_call_status": str((command_record or {}).get("tool_call_status") or "")',
                '"state_version": int((command_record or {}).get("state_version") or self.state.version)',
                'payload={"status": new_status, "command": normalized, **dict(command_record or {})}',
            ):
                if token not in apply_runtime_command:
                    violations.append(f"AgentRuntime.apply_runtime_command missing command ToolCall evidence token: {token}")
        runtime_command_replay = _function_source(source, "_runtime_command_replay_summary")
        for required in (
            '"cancelled_batch_total": cancelled_batch_total',
            '"cancelled_graph_total": cancelled_graph_total',
            '"resumed_graph_total": resumed_graph_total',
            '"retried_graph_total": retried_graph_total',
            '"status_transition_counts": dict(sorted(status_transition_counts.items()))',
        ):
            if required not in runtime_command_replay:
                violations.append(f"AgentRuntime runtime command replay missing queue-impact token: {required}")
        review_advisory_replay = _function_source(source, "_review_advisory_replay_summary")
        for required in (
            '"proposal_status_counts": dict(sorted(proposal_status_counts.items()))',
            '"pending_proposal_count": int(proposal_status_counts.get("proposed") or 0)',
            '"confirmed_proposal_count": int(proposal_status_counts.get("confirmed") or 0)',
            '"rejected_proposal_count": int(proposal_status_counts.get("rejected") or 0)',
            '"advisory_item_count": advisory_item_count',
        ):
            if required not in review_advisory_replay:
                violations.append(f"AgentRuntime review advisory replay missing proposal-status token: {required}")
        layout_adjustment_replay = _function_source(source, "_layout_adjustment_replay_summary")
        for required in (
            '"proposal_status_counts": dict(sorted(proposal_status_counts.items()))',
            '"pending_proposal_count": int(proposal_status_counts.get("proposed") or 0)',
            '"confirmed_proposal_count": int(proposal_status_counts.get("confirmed") or 0)',
            '"failed_proposal_count": int(proposal_status_counts.get("failed") or 0)',
            '"delta_count": delta_count',
            '"layout_transform_failure_code_counts": dict(',
        ):
            if required not in layout_adjustment_replay:
                violations.append(f"AgentRuntime layout adjustment replay missing proposal-status token: {required}")
        if not final_adjustment_confirmation_replay:
            violations.append("AgentRuntime._final_adjustment_confirmation_replay_summary not found")
        else:
            for required in (
                '"confirmation_count": confirmation_count',
                '"confirmation_failed_count": confirmation_failed_count',
                '"confirmation_skipped_count": confirmation_skipped_count',
                '"decision_counts": dict(sorted(decision_counts.items()))',
                '"latest_confirmation": latest_confirmation',
                'event == "final_adjustment_confirmation_recorded"',
                'event == "final_adjustment_confirmation_record_failed"',
            ):
                if required not in final_adjustment_confirmation_replay:
                    violations.append(
                        "AgentRuntime final adjustment confirmation replay missing token: "
                        f"{required}"
                    )
        for required in (
            '"final_adjustment_confirmation_replay_summary": (',
            "self._final_adjustment_confirmation_replay_summary(entries)",
        ):
            if required not in operation_replay_summary_for_report:
                violations.append(
                    "AgentRuntime._operation_replay_summary_for_report missing final adjustment replay token: "
                    f"{required}"
                )
        if (
            'replay["final_adjustment_confirmation_replay_summary"] = ('
            not in compose_operation_replay
            or "self._final_adjustment_confirmation_replay_summary(replay.get(\"entries\", []))"
            not in compose_operation_replay
        ):
            violations.append(
                "AgentRuntime._compose_operation_replay must include final_adjustment_confirmation_replay_summary"
            )
        asset_transfer_replay = _function_source(source, "_asset_transfer_replay_summary")
        for required in (
            '"asset_transfer_started_count": started_count',
            '"asset_transfer_progress_count": progress_count',
            '"asset_transfer_completed_count": completed_count',
            '"asset_transfer_failed_count": failed_count',
            '"peer_asset_ready_count": peer_ready_count',
            '"transfer_status_counts": dict(sorted(transfer_status_counts.items()))',
        ):
            if required not in asset_transfer_replay:
                violations.append(f"AgentRuntime asset transfer replay missing lifecycle token: {required}")
        if '"asset_transfer_replay_summary": asset_transfer_replay_summary' not in operation_replay_summary_for_report:
            violations.append(
                "AgentRuntime._operation_replay_summary_for_report must include asset_transfer_replay_summary"
            )
        peer_sync_replay = _function_source(source, "_peer_sync_replay_summary")
        for required in (
            '"peer_event_count": peer_event_count',
            '"peer_join_count": peer_join_count',
            '"peer_leave_count": peer_leave_count',
            '"sync_reconcile_count": sync_reconcile_count',
            '"sync_reconcile_failed_count": sync_reconcile_failed_count',
            '"state_reconcile_count": state_reconcile_count',
            '"state_reconcile_failed_count": state_reconcile_failed_count',
            '"latest_reconcile_event": latest_reconcile_event',
        ):
            if required not in peer_sync_replay:
                violations.append(f"AgentRuntime peer sync replay missing lifecycle/reconcile token: {required}")
        if '"peer_sync_replay_summary": self._peer_sync_replay_summary' not in operation_replay_summary_for_report:
            violations.append(
                "AgentRuntime._operation_replay_summary_for_report must include peer_sync_replay_summary"
            )
        if '"runtime_event_replay_summary": self._runtime_event_replay_summary' not in operation_replay_summary_for_report:
            violations.append(
                "AgentRuntime._operation_replay_summary_for_report must include runtime_event_replay_summary"
            )
        for required in (
            '"batch_resource_lifecycle_summary": self._batch_resource_lifecycle_replay_summary',
            '"batch_execution_summary": self._batch_execution_replay_summary',
        ):
            if required not in operation_replay_summary_for_report:
                violations.append(
                    f"AgentRuntime._operation_replay_summary_for_report missing batch lifecycle token: {required}"
                )
    if not batch_resource_lifecycle_replay:
        violations.append("AgentRuntime._batch_resource_lifecycle_replay_summary not found")
    else:
        for required in (
            '"image_ready_count": 0',
            '"model_ready_count": 0',
            '"import_ready_count": 0',
            '"environment_ready_count": 0',
            '"emit_failed_count": 0',
            '"batch_event_counts": {}',
            '"latest_resource_event": {}',
            '"image_resources_ready": ("image_ready_count", "image")',
            '"actors_imported": ("import_ready_count", "import")',
        ):
            if required not in batch_resource_lifecycle_replay:
                violations.append(f"AgentRuntime batch resource lifecycle replay missing token: {required}")
    if not intervention_batch_replay:
        violations.append("AgentRuntime._intervention_batch_replay_summary not found")
    else:
        for required in (
            '"route_absorbable_count": route_absorbable_count',
            '"route_non_absorbable_count": route_non_absorbable_count',
            '"route_requested_item_count": route_requested_item_count',
            '"merge_event_count": merge_event_count',
            '"merged_item_count": merged_item_count',
            '"merge_absorbed_count": merge_absorbed_count',
            'event == "batch_interventions_merged_via_tool_graph"',
        ):
            if required not in intervention_batch_replay:
                violations.append(f"AgentRuntime intervention batch replay missing route/merge token: {required}")
    if compose_operation_replay and (
        '"batch_resource_lifecycle_summary"] = self._batch_resource_lifecycle_replay_summary'
        not in compose_operation_replay
    ):
        violations.append("AgentRuntime._compose_operation_replay must expose batch_resource_lifecycle_summary")
    if compose_operation_replay and (
        '"geometry_fact_replay_summary"] = self._geometry_fact_replay_summary'
        not in compose_operation_replay
    ):
        violations.append("AgentRuntime._compose_operation_replay must expose geometry_fact_replay_summary")

    if not status_summary:
        violations.append("AgentRuntime.status_summary not found")
    else:
        for forbidden in ('"user_report_generated"', "_persist_user_report", 'event_type="report_ready"'):
            if forbidden in status_summary:
                violations.append(f"AgentRuntime.status_summary must stay read-only for reports: found {forbidden}")
        for required in (
            "runtime_status_queried",
            "_operation_log_snapshot_from_entries",
            "_status_summary_snapshot_via_tool_graph",
            "intervention_digest = self._intervention_digest_for_report",
            '"intervention_digest": intervention_digest',
            '"sync_health_digest": sync_health_digest',
            "_sync_health_digest_for_report(",
            "_batch_tooling_summary_for_plan(",
            '"batch_tooling_summary": batch_tooling_summary',
            "_tool_queue_health_summary_for_plan(",
            '"tool_queue_health_summary": tool_queue_health_summary',
            "_batch_resource_flow_summary_for_plan(",
            '"batch_resource_flow_summary": batch_resource_flow_summary',
            "runtime_event_replay_summary = self._runtime_event_replay_summary",
            '"runtime_event_replay_summary": runtime_event_replay_summary',
            "runtime_guard_replay_summary = self._runtime_guard_replay_summary",
            '"runtime_guard_replay_summary": runtime_guard_replay_summary',
            "scene_plan_lifecycle_summary = self._scene_plan_lifecycle_replay_summary",
            '"scene_plan_lifecycle_summary": scene_plan_lifecycle_summary',
            "vlm_checkpoint_summary = self._vlm_checkpoint_replay_summary",
            '"vlm_checkpoint_summary": vlm_checkpoint_summary',
            "review_advisory_replay_summary = self._review_advisory_replay_summary",
            '"review_advisory_replay_summary": review_advisory_replay_summary',
            "scene_design_contract_summary = self._scene_design_contract_summary_for_plan",
            '"scene_design_contract_summary": scene_design_contract_summary',
		            "semantic_arbitration_summary = self._semantic_arbitration_digest_for_report",
		            '"semantic_arbitration_summary": semantic_arbitration_summary',
            "scene_snapshot_summary = self._scene_snapshot_summary_for_plan",
            '"scene_snapshot_summary": scene_snapshot_summary',
            "geometry_fact_summary = self._geometry_fact_summary_for_plan",
            '"geometry_fact_summary": geometry_fact_summary',
            "resource_summary = self._resource_summary_for_plan",
            '"resource_summary": resource_summary',
            "import_summary = self._import_summary_for_plan",
            '"import_summary": import_summary',
		            "tool_execution_digest = self._tool_execution_digest_for_report",
		            '"tool_execution_digest": tool_execution_digest',
		        ):
            if required not in status_summary:
                violations.append(f"AgentRuntime.status_summary missing audit/read-summary token: {required}")
        if "self.registry.capability_summary(" in status_summary:
            violations.append(
                "AgentRuntime.status_summary must read ToolRegistry summary through "
                "runtime.tool_manifest.snapshot instead of direct registry access"
            )
        if not status_summary_snapshot or "runtime.status_summary.snapshot" not in status_summary_snapshot:
            violations.append(
                "AgentRuntime._status_summary_snapshot_via_tool_graph must execute "
                "runtime.status_summary.snapshot"
            )
        if status_summary_snapshot:
            for required in (
                "status_with_evidence[\"snapshot_recorded\"] = True",
                "status_with_evidence[\"snapshot_status\"] = str(graph.status or \"\")",
                "status_with_evidence[\"snapshot_tool_status\"] = str(snapshot_call.status.value)",
                "status_with_evidence[\"snapshot_state_version\"] = int(self.state.version)",
            ):
                if required not in status_summary_snapshot:
                    violations.append(
                        "AgentRuntime._status_summary_snapshot_via_tool_graph missing Runtime status "
                        f"ToolCall evidence token: {required}"
                    )
        status_snapshot_sources = {
            "AgentRuntime._status_summary_snapshot_via_tool_graph": status_summary_snapshot,
            "AgentRuntime._record_status_summary_snapshot_tool": record_status_summary_snapshot,
        }
        if not snapshot_failure_payload:
            violations.append("AgentRuntime._snapshot_failure_audit_payload not found")
        else:
            for required in (
                '"recorded": False',
                '"failure_code": "snapshot_record_failed"',
                '"reason": AgentRuntime._safe_report_text(reason or "unknown")[:160]',
            ):
                if required not in snapshot_failure_payload:
                    violations.append(f"AgentRuntime._snapshot_failure_audit_payload missing token: {required}")
            if any(secret in snapshot_failure_payload for secret in ("prompt", "provider", "asset_path")):
                violations.append(
                    "AgentRuntime._snapshot_failure_audit_payload must stay safe and avoid prompt/provider/path fields"
                )
        snapshot_failure_sources = {
            "AgentRuntime.tool_manifest": tool_manifest,
            "AgentRuntime._status_summary_snapshot_via_tool_graph": status_summary_snapshot,
            "AgentRuntime._gm_summary_snapshot_via_tool_graph": gm_summary_snapshot,
            "AgentRuntime._runtime_events_snapshot_via_tool_graph": runtime_events_snapshot,
            "AgentRuntime._sync_status_snapshot_via_tool_graph": sync_status_snapshot,
            "AgentRuntime._provider_status_snapshot_via_tool_graph": provider_status_snapshot,
            "AgentRuntime._operation_replay_snapshot_via_tool_graph": operation_replay_snapshot,
            "AgentRuntime._operation_replay_summary_via_tool_graph": operation_replay_summary_snapshot,
        }
        for snapshot_name, snapshot_source in snapshot_failure_sources.items():
            if 'snapshot_failed' in snapshot_source and 'payload=self._snapshot_failure_audit_payload(' not in snapshot_source:
                violations.append(f"{snapshot_name} must record safe snapshot failure payloads")
            if 'payload={"reason": reason' in snapshot_source or '"reason": reason}' in snapshot_source:
                violations.append(f"{snapshot_name} must not regress to reason-only snapshot failure payloads")
        for required in (
            "report_health_summary = dict(",
            '"report_health_status": AgentRuntime._safe_report_text(',
            '"report_health_attention_required": bool(',
            '"report_health_reasons": [',
            "engine_write_readiness_summary = dict(",
            '"engine_write_readiness_native_enabled_count": int(',
            '"engine_write_readiness_runtime_state_only_count": int(',
            '"engine_write_readiness_fallback_count": int(',
            '"engine_write_readiness_disabled_count": int(',
            '"engine_write_readiness_unavailable_count": int(',
            '"engine_write_readiness_native_enabled_channels": [',
            '"engine_write_readiness_runtime_state_only_channels": [',
            '"engine_write_readiness_fallback_channels": [',
            '"engine_write_readiness_disabled_channels": [',
            '"engine_write_readiness_unavailable_channels": [',
        ):
            for name, snippet in status_snapshot_sources.items():
                if required not in snippet:
                    violations.append(
                        f"{name} must preserve report health / engine-write "
                        f"audit payload: missing {required}"
                    )

    if not provider_status:
        violations.append("AgentRuntime.provider_status not found")
    else:
        for forbidden in ('"user_report_generated"', "_persist_user_report", 'event_type="report_ready"'):
            if forbidden in provider_status:
                violations.append(f"AgentRuntime.provider_status must stay read-only for reports: found {forbidden}")
        for required in (
            "runtime_provider_status_queried",
            "_provider_status_snapshot_via_tool_graph",
        ):
            if required not in provider_status:
                violations.append(f"AgentRuntime.provider_status missing Runtime provider fact token: {required}")
        if not provider_status_snapshot or "runtime.resource_status.snapshot" not in provider_status_snapshot:
            violations.append(
                "AgentRuntime._provider_status_snapshot_via_tool_graph must execute "
                "runtime.resource_status.snapshot"
            )
        if provider_status_snapshot:
            for required in (
                "provider_status_with_evidence[\"snapshot_recorded\"] = True",
                "provider_status_with_evidence[\"snapshot_status\"] = str(graph.status or \"\")",
                "provider_status_with_evidence[\"snapshot_tool_status\"] = str(snapshot_call.status.value)",
                "provider_status_with_evidence[\"snapshot_state_version\"] = int(self.state.version)",
            ):
                if required not in provider_status_snapshot:
                    violations.append(
                        "AgentRuntime._provider_status_snapshot_via_tool_graph missing Runtime provider "
                        f"ToolCall evidence token: {required}"
                    )
        for required in (
            "engine_write_readiness_summary = dict(",
            '"engine_write_readiness_native_enabled_count": int(',
            '"engine_write_readiness_runtime_state_only_count": int(',
            '"engine_write_readiness_fallback_count": int(',
            '"engine_write_readiness_disabled_count": int(',
            '"engine_write_readiness_unavailable_count": int(',
            '"engine_write_readiness_native_enabled_channels": [',
            '"engine_write_readiness_runtime_state_only_channels": [',
            '"engine_write_readiness_fallback_channels": [',
            '"engine_write_readiness_disabled_channels": [',
            '"engine_write_readiness_unavailable_channels": [',
        ):
            if required not in provider_status_snapshot:
                violations.append(
                    "AgentRuntime._provider_status_snapshot_via_tool_graph must preserve "
                    f"engine-write readiness snapshot payload: missing {required}"
                )

    if not gm_summary:
        violations.append("AgentRuntime.gm_summary not found")
    else:
        for forbidden in ('"user_report_generated"', "_persist_user_report", 'event_type="report_ready"'):
            if forbidden in gm_summary:
                violations.append(f"AgentRuntime.gm_summary must stay read-only for reports: found {forbidden}")
        for required in (
            "status_summary(",
            "_gm_summary_snapshot_via_tool_graph",
	            "runtime_gm_summary_exported",
	            "context_digest",
	            "agent_contributions",
	            "intervention_digest",
            "intervention_pending_count",
            "intervention_accepted_count",
            "intervention_deferred_count",
            "layout_proposal_count",
            "layout_applied_delta_count",
            "layout_skipped_delta_count",
            "layout_transform_result_count",
            "layout_ground_snapped_count",
            "layout_overlap_resolved_count",
            "layout_transform_failure_code_counts",
            "engine_write_boundary_digest",
            "engine_write_boundary_fact_count",
            "engine_write_bridge_call_count",
            "engine_write_bridge_success_count",
            "engine_write_bridge_failed_count",
            "engine_write_bridge_error_code_counts",
            "batch_tooling_summary",
            "batch_tooling_digest",
            "created_batch_count",
            "prioritized_item_count",
            "merged_intervention_item_count",
            "absorbed_intervention_count",
            "batch_resource_flow_summary",
            "resource_flow_digest",
            "resource_batch_count",
            "resource_failed_count",
            "resource_waiting_count",
            "resource_import_failure_code_counts",
            "report_import_failure_code_counts",
            "tool_queue_health_summary",
            "tool_queue_health_digest",
            "queue_pressure",
            "active_count",
            "blocked_count",
            "state_patch_summary",
            "state_patch_digest",
            "reconcile_pending_count",
            "tool_failure_strategy_summary",
            "tool_failure_strategy_digest",
            "retry_scheduled_count",
            "abandoned_late_result_count",
            "stopped_by_runtime_command_count",
            "runtime_guard_replay_summary",
            "runtime_guard_digest",
            "high_risk_confirmation_required_count",
            "write_confirmation_required_count",
            "system_actor_write_blocked_count",
            "user_visible_blocked_event_count",
            "scene_plan_lifecycle_summary",
            "scene_plan_lifecycle_digest",
            "created_count",
            "confirmed_count",
            "state_persist_failed_count",
            "status_persist_failed_count",
            "vlm_checkpoint_summary",
            "vlm_checkpoint_digest",
            "checkpoint_count",
            "checkpoint_counts",
            "review_advisory_replay_summary",
            "review_advisory_replay_digest",
            "proposal_created_count",
            "confirmation_count",
            "advisory_item_count",
            "scene_design_contract_summary",
            "scene_design_contract_digest",
            "scene_design_contract_available",
            "scene_design_contract_scene_type",
            "scene_design_contract_environment_type",
            "scene_type",
            "environment_type",
            "terrain_type",
            "boundary_type",
            "semantic_arbitration_summary",
            "semantic_arbitration_digest",
		            "semantic_arbitration_state",
		            "semantic_arbitration_requires_host_confirmation",
		            "arbitration_state",
		            "execution_readiness",
		            "requires_host_confirmation",
		            "needs_clarification",
            "scene_snapshot_summary",
            "scene_snapshot_digest",
            "scene_snapshot_count",
            "scene_observed_actor_count",
            "resource_summary",
            "resource_stage_digest",
            "resource_event_count",
            "import_summary",
            "import_stage_digest",
            "imported_actor_count",
            "import_failed_count",
            "geometry_fact_summary",
            "geometry_fact_digest",
            "geometry_fact_count",
            "geometry_overlap_issue_count",
            "aabb_actor_count",
            "overlap_issue_count",
		            "tool_execution_summary",
		            "tool_execution_digest",
		            "tool_execution_attention_required",
	            "tool_execution_failed_count",
	            "tool_execution_blocked_count",
	            "attention_required",
	            "attention_reasons",
	            "engine_write_summary",
            "engine_write_digest",
            "engine_write_readiness_summary",
            "engine_write_readiness_digest",
            "runtime_state_only_count",
            "fallback_count",
            "disabled_count",
            "import_result_count",
            "transform_result_count",
            "status_export_count",
            "latest_status_export",
            "engine_write_bridge_failed_count",
            "engine_write_bridge_error_code_counts",
            "message_delivery_summary",
            "message_delivery_digest",
            "requested_count",
            "succeeded_count",
            "failed_count",
            "sync_health_digest",
            "sync_health_status",
            "sync_failure_code_counts",
            "latest_sync_failure_code",
            "sync_replay_summary",
            "asset_transfer_replay_summary",
            "peer_sync_replay_summary",
            "runtime_event_replay_summary",
            "sync_replay_digest",
            "runtime_event_replay_digest",
            "resource_readiness_replay_summary",
            "resource_readiness_replay_digest",
            "resource_readiness_publish_count",
            "resource_readiness_query_count",
            "resource_readiness_publish_enabled_total",
            "status_query_status_counts",
            "disclosure_skipped_count",
            "asset_transfer_progress_count",
            "peer_asset_ready_count",
            "sync_reconcile_count",
        ):
            if required not in gm_summary:
                violations.append(f"AgentRuntime.gm_summary missing Runtime GM summary token: {required}")
        if not gm_summary_snapshot or "runtime.gm_summary.snapshot" not in gm_summary_snapshot:
            violations.append(
                "AgentRuntime._gm_summary_snapshot_via_tool_graph must execute "
                "runtime.gm_summary.snapshot"
            )
        if gm_summary_snapshot:
            for required in (
                "summary_with_evidence[\"snapshot_recorded\"] = True",
                "summary_with_evidence[\"snapshot_status\"] = str(graph.status or \"\")",
                "summary_with_evidence[\"snapshot_tool_status\"] = str(snapshot_call.status.value)",
                "summary_with_evidence[\"snapshot_state_version\"] = int(self.state.version)",
            ):
                if required not in gm_summary_snapshot:
                    violations.append(
                        "AgentRuntime._gm_summary_snapshot_via_tool_graph missing Runtime GM summary "
                        f"ToolCall evidence token: {required}"
                    )
        for required in (
            "context_digest = dict(",
            "intervention_digest = dict(",
            "layout_digest = dict(",
            "runtime_event_digest = dict(",
            "report_health_digest = dict(",
            "runtime_guard_digest = dict(",
            '"agent_contribution_count": len(list(context_digest.get("agent_contributions") or []))',
            '"latest_user_point_count": len(list(context_digest.get("latest_user_points") or []))',
            '"intervention_pending_count": int(intervention_digest.get("pending_count") or 0)',
            '"intervention_accepted_count": int(intervention_digest.get("accepted_count") or 0)',
            '"intervention_deferred_count": int(intervention_digest.get("deferred_count") or 0)',
            '"layout_proposal_count": int(layout_digest.get("proposal_count") or 0)',
            '"layout_applied_delta_count": int(layout_digest.get("applied_delta_count") or 0)',
            '"layout_skipped_delta_count": int(layout_digest.get("skipped_delta_count") or 0)',
            '"runtime_event_emitted_count": int(runtime_event_digest.get("emitted_count") or 0)',
            '"runtime_event_emit_failed_count": int(runtime_event_digest.get("emit_failed_count") or 0)',
            '"report_health_status": AgentRuntime._safe_report_text(report_health_digest.get("status"))[:48]',
            '"report_attention_required": bool(report_health_digest.get("attention_required"))',
            '"runtime_guard_blocked_count": int(runtime_guard_digest.get("blocked_count") or 0)',
            '"runtime_guard_requires_write_blocked_count": int(',
            'runtime_guard_digest.get("requires_write_blocked_count") or 0',
            '"runtime_guard_confirmed_blocked_count": int(',
            'runtime_guard_digest.get("confirmed_blocked_count") or 0',
            '"runtime_guard_unconfirmed_blocked_count": int(',
            'runtime_guard_digest.get("unconfirmed_blocked_count") or 0',
            '"runtime_guard_risk_level_counts": {',
        ):
            if required not in gm_summary_snapshot:
                violations.append(
                    "AgentRuntime._gm_summary_snapshot_via_tool_graph must preserve "
                    f"GM context/intervention/layout audit payload: missing {required}"
                )

    if not handle_message:
        violations.append("AgentRuntime.handle_message not found")
    else:
        if "_runtime_events_snapshot_via_tool_graph" not in handle_message:
            violations.append(
                "AgentRuntime.handle_message runtime_events action must snapshot "
                "user-visible event feed before returning it"
            )
        if not runtime_events_snapshot or "runtime.events.snapshot" not in runtime_events_snapshot:
            violations.append(
                "AgentRuntime._runtime_events_snapshot_via_tool_graph must execute "
                "runtime.events.snapshot"
            )
        if runtime_events_snapshot:
            for required in (
                "\"snapshot_recorded\": True",
                "\"snapshot_status\": str(graph.status or \"\")",
                "\"snapshot_tool_status\": str(snapshot_call.status.value)",
                "\"snapshot_state_version\": int(self.state.version)",
            ):
                if required not in runtime_events_snapshot:
                    violations.append(
                        "AgentRuntime._runtime_events_snapshot_via_tool_graph missing Runtime events "
                        f"ToolCall evidence token: {required}"
                    )
        for required in (
            "events_snapshot = self._runtime_events_snapshot_via_tool_graph(",
            "events = list(events_snapshot.get(\"runtime_events\") or [])",
            "\"snapshot_recorded\": bool(events_snapshot.get(\"snapshot_recorded\"))",
            "\"snapshot_status\": str(events_snapshot.get(\"snapshot_status\") or \"\")",
            "\"snapshot_tool_status\": str(events_snapshot.get(\"snapshot_tool_status\") or \"\")",
            "\"snapshot_state_version\": int(events_snapshot.get(\"snapshot_state_version\") or self.state.version)",
        ):
            if required not in handle_message:
                violations.append(f"AgentRuntime.handle_message runtime_events missing snapshot evidence token: {required}")
        for required in (
            "_runtime_event_snapshot_summary(",
            '"event_type_counts": dict(sorted(event_type_counts.items()))',
            '"level_counts": dict(sorted(level_counts.items()))',
            '"audience_counts": dict(sorted(audience_counts.items()))',
            '"progress_event_count": progress_event_count',
            '"warning_count": int(level_counts.get("warning") or 0)',
            '"error_count": int(level_counts.get("error") or 0)',
            '"latest_event_type": latest_event_type',
            "event_summary = self._runtime_event_snapshot_summary(",
            "**event_summary",
        ):
            if required not in source:
                violations.append(
                    "AgentRuntime runtime events snapshot must preserve "
                    f"safe event audit summary: missing {required}"
                )
        if "_sync_status_snapshot_via_tool_graph" not in handle_message:
            violations.append(
                "AgentRuntime.handle_message sync_status action must snapshot "
                "sync status before returning it"
            )
        for required in (
            'engine_write_summary = dict(provider_status.get("engine_write_summary") or {})',
            'engine_write_readiness_summary = dict(',
            '"engine_write_readiness_summary": engine_write_readiness_summary',
            '"engine_write_summary": engine_write_summary',
            '"runtime_engine_write_status_exported"',
            '"engine_write_boundary_fact_count": int(',
            '"engine_write_import_boundary_count": int(',
            '"engine_write_environment_import_boundary_count": int(',
            '"engine_write_transform_boundary_count": int(',
            '"engine_write_delete_boundary_count": int(',
            '"engine_write_bridge_call_count": int(',
            '"engine_write_bridge_success_count": int(',
            '"engine_write_bridge_failed_count": int(',
            '"engine_write_bridge_error_code_counts": dict(',
            '"engine_write_readiness_native_enabled_count": int(',
            '"engine_write_readiness_runtime_state_only_count": int(',
            '"engine_write_readiness_fallback_count": int(',
            '"engine_write_readiness_disabled_count": int(',
            '"engine_write_readiness_native_enabled_channels": [',
            '"engine_write_readiness_runtime_state_only_channels": [',
            '"engine_write_readiness_fallback_channels": [',
            '"engine_write_readiness_disabled_channels": [',
        ):
            if required not in handle_message:
                violations.append(f"AgentRuntime.handle_message engine_write_status missing replay token: {required}")
        for required in (
            '"asset_transfer_replay_summary": asset_transfer_replay',
            '"asset_transfer_started_count": int(asset_transfer_replay.get("asset_transfer_started_count") or 0)',
            '"asset_transfer_progress_count": int(asset_transfer_replay.get("asset_transfer_progress_count") or 0)',
            '"asset_transfer_completed_count": int(asset_transfer_replay.get("asset_transfer_completed_count") or 0)',
            '"asset_transfer_failed_count": int(asset_transfer_replay.get("asset_transfer_failed_count") or 0)',
            '"peer_asset_ready_count": int(asset_transfer_replay.get("peer_asset_ready_count") or 0)',
            '"actor_transform_count": int(sync_replay.get("actor_transform_count") or 0)',
            '"actor_delete_count": int(sync_replay.get("actor_delete_count") or 0)',
            '"peer_sync_replay_summary": peer_sync_replay',
            '"peer_event_count": int(peer_sync_replay.get("peer_event_count") or 0)',
            '"sync_reconcile_count": int(peer_sync_replay.get("sync_reconcile_count") or 0)',
            '"state_reconcile_count": int(peer_sync_replay.get("state_reconcile_count") or 0)',
        ):
            if required not in handle_message:
                violations.append(f"AgentRuntime.handle_message sync_status missing transfer/peer token: {required}")
        if not sync_status_snapshot or "runtime.sync_status.snapshot" not in sync_status_snapshot:
            violations.append(
                "AgentRuntime._sync_status_snapshot_via_tool_graph must execute "
                "runtime.sync_status.snapshot"
            )
        if sync_status_snapshot:
            for required in (
                "sync_status_with_evidence[\"snapshot_recorded\"] = True",
                "sync_status_with_evidence[\"snapshot_status\"] = str(graph.status or \"\")",
                "sync_status_with_evidence[\"snapshot_tool_status\"] = str(snapshot_call.status.value)",
                "sync_status_with_evidence[\"snapshot_state_version\"] = int(self.state.version)",
            ):
                if required not in sync_status_snapshot:
                    violations.append(
                        "AgentRuntime._sync_status_snapshot_via_tool_graph missing Runtime sync status "
                        f"ToolCall evidence token: {required}"
                    )
        for required in (
            'asset_transfer_replay = dict(status.get("asset_transfer_replay_summary") or {})',
            '"asset_transfer_started_count": int(',
            '"asset_transfer_progress_count": int(',
            '"asset_transfer_completed_count": int(',
            '"asset_transfer_failed_count": int(',
            '"peer_asset_ready_count": int(asset_transfer_replay.get("peer_asset_ready_count") or 0)',
            'peer_sync_replay = dict(status.get("peer_sync_replay_summary") or {})',
            '"peer_event_count": int(peer_sync_replay.get("peer_event_count") or 0)',
            '"sync_reconcile_count": int(peer_sync_replay.get("sync_reconcile_count") or 0)',
            '"state_reconcile_count": int(peer_sync_replay.get("state_reconcile_count") or 0)',
            '"sync_event_type_counts": AgentRuntime._safe_status_count_map(',
            '"asset_transfer_status_counts": AgentRuntime._safe_status_count_map(',
            '"asset_transfer_event_type_counts": AgentRuntime._safe_status_count_map(',
            '"peer_sync_event_type_counts": AgentRuntime._safe_status_count_map(',
            '"latest_transfer_status": AgentRuntime._safe_report_text(',
            '"latest_transfer_progress": int(',
            '"latest_peer_event_type": AgentRuntime._safe_report_text(',
        ):
            if required not in sync_status_snapshot:
                violations.append(f"AgentRuntime sync status snapshot missing transfer/peer token: {required}")
        for required in (
            "safe_snapshot = {",
            "_safe_graph_summary_for_user",
            "_safe_graphs_for_user",
            "_safe_queue_result_for_user",
            "_safe_drain_result_for_user",
            '"graph": {"status": graph_status}',
            '"snapshot": safe_snapshot',
            '"graph": {"status": graph_status}',
            '"graph": {"status": ""}',
        ):
            if required not in handle_message:
                violations.append(f"AgentRuntime.handle_message missing user-safe graph return token: {required}")
        forbidden_handle_returns = (
            '"snapshot": snapshot',
            '"graph": graph',
            '"graph": adjustment.get("graph"',
            '"graph": result.get("graph"',
            '"drain": drain_result',
            '"queued": queued',
            '"graphs": queued["graphs"]',
            '"graphs": execution["graphs"]',
        )
        for forbidden in forbidden_handle_returns:
            if forbidden in handle_message:
                violations.append(f"AgentRuntime.handle_message must not return raw graph payloads: found {forbidden}")
        for required in (
            "execution = self.execute_planned_batches(",
            '"action": normalized_action',
            '"batches": execution["batches"]',
            '"report": execution["report"]',
            '"message": (',
        ):
            if required not in handle_message:
                violations.append(
                    f"AgentRuntime.handle_message confirm/execute path missing planned-batch Runtime token: {required}"
                )
        if "self.execute_scene_plan(" in handle_message:
            violations.append(
                "AgentRuntime.handle_message confirm/execute path must not bypass planned BatchPlan execution via execute_scene_plan"
            )

    if not execute_scene_plan:
        violations.append("AgentRuntime.execute_scene_plan not found")
    else:
        for required in (
            "include_debug_graph_nodes: bool = False",
            "graph_result = {",
            '"node_count": len(graph.nodes) if graph else 0',
            "if include_debug_graph_nodes:",
            'graph_result["nodes"] = {key: call.as_dict() for key, call in graph.nodes.items()} if graph else {}',
            '"queued": self._safe_queue_result_for_user(queued)',
            '"drain": self._safe_drain_result_for_user(drain_result)',
        ):
            if required not in execute_scene_plan:
                violations.append(f"AgentRuntime.execute_scene_plan missing safe graph return token: {required}")
        if execute_scene_plan.find("if include_debug_graph_nodes:") > execute_scene_plan.find('graph_result["nodes"] = {key: call.as_dict()'):
            violations.append(
                "AgentRuntime.execute_scene_plan must only return graph nodes after include_debug_graph_nodes guard"
            )
    for name, body in (
        ("enqueue_scene_plan", enqueue_scene_plan),
        ("enqueue_planned_batches", enqueue_planned_batches),
        ("enqueue_pending_intervention_batch", enqueue_pending_intervention_batch),
    ):
        if not body:
            violations.append(f"AgentRuntime.{name} not found")
            continue
        if "_build_batch_execution_graph(" not in body:
            violations.append(f"AgentRuntime.{name} must build formal batch execution graphs")
        if "_build_mock_graph(" in body:
            violations.append(f"AgentRuntime.{name} must not call legacy mock graph builder")
    if "def _build_mock_graph(" in source:
        violations.append("AgentRuntime must not keep legacy _build_mock_graph compatibility wrapper")
    if not build_batch_execution_graph:
        violations.append("AgentRuntime._build_batch_execution_graph not found")
    else:
        for required in (
            'tool_name="scene.extract_objects"',
            '"plan_id": plan.plan_id',
            '"batch_id": batch.batch_id',
        ):
            if required not in build_batch_execution_graph:
                violations.append(
                    f"AgentRuntime._build_batch_execution_graph missing plan/batch boundary token: {required}"
                )
        if '"plan_id": batch.batch_id' in build_batch_execution_graph:
            violations.append(
                "AgentRuntime._build_batch_execution_graph must not pass batch_id as scene.extract_objects plan_id"
            )
        for required in (
            'tool_name="runtime.placement.propose"',
            '"batch_id": batch.batch_id',
        ):
            if required not in build_batch_execution_graph:
                violations.append(
                    f"AgentRuntime._build_batch_execution_graph missing placement batch-boundary token: {required}"
                )
        for required in (
            'tool_name="runtime.asset.plan"',
            '"batch_id": batch.batch_id',
        ):
            if required not in build_batch_execution_graph:
                violations.append(
                    f"AgentRuntime._build_batch_execution_graph missing asset request batch-boundary token: {required}"
                )
        for required in (
            'tool_name="runtime.environment.import_components"',
            'tool_name="runtime.actor.import_batch"',
            'tool_name="runtime.review.summarize_batch"',
        ):
            if required not in build_batch_execution_graph:
                violations.append(
                    f"AgentRuntime._build_batch_execution_graph missing F5 minimal Runtime write/review node: {required}"
                )
    for forbidden in (
        "def _register_default_mock_tools(",
        "def _mock_import_actor(",
        '"mock.import_actor"',
        "'mock.import_actor'",
    ):
        if forbidden in source:
            violations.append(f"AgentRuntime default Runtime tool path must not expose mock import token: {forbidden}")

    if violations:
        print("[FAIL] static Runtime report fact-source gate")
        for item in violations:
            _print_violation(item)
        return False
    print("[OK]  static Runtime report fact-source gate")
    return True


def _runtime_validator_contract_gate() -> bool:
    print("[RUN] static Runtime validator contract gate")
    core_path = REPO_ROOT / AGENT_RUNTIME_CORE
    try:
        source = core_path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        source = core_path.read_text(encoding="utf-8-sig")
    tools_path = REPO_ROOT / AGENT_RUNTIME_TOOLS
    try:
        tools_source = tools_path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        tools_source = tools_path.read_text(encoding="utf-8-sig")
    adapters_path = REPO_ROOT / AGENT_RUNTIME_ADAPTERS
    try:
        adapters_source = adapters_path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        adapters_source = adapters_path.read_text(encoding="utf-8-sig")
    report_policy_path = REPO_ROOT / "editor/plugins/AITool/services/runtime_report_policy.py"
    try:
        report_policy_source = report_policy_path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        report_policy_source = report_policy_path.read_text(encoding="utf-8-sig")
    worker_path = REPO_ROOT / LANCHAT_AGENT_WORKER
    try:
        worker_source = worker_path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        worker_source = worker_path.read_text(encoding="utf-8-sig")
    test_path = REPO_ROOT / LANCHAT_RUNTIME_GUARD_TESTS
    try:
        test_source = test_path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        test_source = test_path.read_text(encoding="utf-8-sig")
    phase1_test_path = REPO_ROOT / AGENT_RUNTIME_PHASE1_TESTS
    try:
        phase1_test_source = phase1_test_path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        phase1_test_source = phase1_test_path.read_text(encoding="utf-8-sig")
    report_health_formatter = _function_source(
        report_policy_source,
        "format_agent_runtime_report_health_report",
    )
    violations: list[str] = []

    for validator in sorted(REQUIRED_RUNTIME_VALIDATORS):
        if f"class {validator}" not in source:
            violations.append(f"missing required Runtime schema validator: {validator}")
    for required_tool_contract in (
        '"runtime.asset.plan"',
        '"runtime.placement.propose"',
    ):
        if required_tool_contract not in tools_source:
            violations.append(
                f"Runtime batch execution tool contract missing token: {required_tool_contract}"
            )
    if tools_source.count('required_args=("room_id", "batch_id", "model_items")') < 2:
        violations.append(
            "Runtime batch execution asset/placement tools must both require room_id, batch_id, and model_items"
        )
    for required_resource_fact_token in (
        "def _resource_phase_fact(",
        "plan_id: str = \"\"",
        '"plan_id": safe_plan_id',
        '"source": "runtime_resource_phase_fact"',
    ):
        if required_resource_fact_token not in tools_source:
            violations.append(
                f"Runtime resource phase fact contract missing token: {required_resource_fact_token}"
            )

    execute = _function_source(source, "execute")
    apply_patch = _function_source(source, "apply_patch")
    handle_message = _function_source(source, "handle_message")
    generate_report = _function_source(source, "generate_report")
    status_summary = _function_source(source, "status_summary")
    runtime_execution_reply = _function_source(
        report_policy_source,
        "format_agent_runtime_execution_reply",
    )
    runtime_intervention_reply = _function_source(
        report_policy_source,
        "format_agent_runtime_intervention_reply",
    )
    runtime_layout_reply = _function_source(
        report_policy_source,
        "format_agent_runtime_layout_confirmation_reply",
    )
    runtime_layout_report = _function_source(
        report_policy_source,
        "format_agent_runtime_layout_report",
    )
    persist_graph = _function_source(source, "_persist_graph")
    persist_user_report = _function_source(source, "_persist_user_report")
    persist_user_report_tool = _function_source(source, "_persist_user_report_tool")
    record_audit_event_tool = _function_source(source, "_record_audit_event_tool")
    execute_planning_context_persist_graph = _function_source(source, "_execute_planning_context_persist_graph")
    persist_planning_context_tool = _function_source(source, "_persist_planning_context_tool")
    record_agent_context_message = _function_source(source, "record_agent_context_message")
    record_user_context_message = _function_source(source, "record_user_context_message")
    apply_layout_delta_tool = _function_source(source, "_apply_layout_delta_tool")
    engine_write_replay_summary = _function_source(source, "_engine_write_replay_summary")
    report_record_validator_start = source.find("class ReportRecordValidator")
    report_record_allowed_fields_end = source.find("_BLOCKED_FIELDS", report_record_validator_start)
    report_record_allowed_fields = (
        source[report_record_validator_start:report_record_allowed_fields_end]
        if report_record_validator_start >= 0 and report_record_allowed_fields_end > report_record_validator_start
        else ""
    )

    required_execute_tokens = (
        "ToolCallGraphValidator.validate(graph, self.registry)",
        "self.guard.authorize(call, definition)",
        "ToolCallValidator._validate_runtime_fact_arg_tree",
        "ToolResultValidator.validate_for_tool",
    )
    for token in required_execute_tokens:
        if token not in execute:
            violations.append(f"ToolCallGraphExecutor.execute missing validation/guard token: {token}")
    for token in (
        "guard_payload = {",
        '"guard_reason": str(reason or "")',
        '"risk_level": effective_risk.value',
        '"requires_write": requires_write',
        '"confirmed": bool(call.confirmed)',
        "payload=guard_payload",
        "guard_payload=guard_payload",
        'payload={"status": "blocked", **dict(guard_payload or {})}',
    ):
        if token not in source:
            violations.append(f"ToolCallGraphExecutor blocked-call audit missing guard payload token: {token}")
    if "ToolCallGraphValidator.safe_graph_fact" not in persist_graph:
        violations.append("ToolCallGraphExecutor._persist_graph must persist only safe ToolCallGraph facts")

    required_apply_patch_tokens = (
        "StatePatchValidator.validate",
        "ScenePlanValidator.validate_plans",
        "BatchPlanValidator.validate_plans",
        "ToolCallGraphValidator.validate_graph_facts",
        "ToolCallGraphValidator.validate_queue_items",
        "AdjustmentProposalValidator.validate",
        "ReviewAdvisoryProposalValidator.validate",
        "ReportRecordValidator.validate_reports",
    )
    for token in required_apply_patch_tokens:
        if token not in apply_patch and token not in source:
            violations.append(f"RuntimeState.apply_patch path missing validator token: {token}")

    if "_persist_user_report(" not in generate_report:
        violations.append("AgentRuntime.generate_report must persist reports through the Runtime tool path")
    for required in (
        "_batch_tooling_summary_for_plan(",
        '"batch_tooling_summary": batch_tooling_summary',
        "_tool_queue_health_summary_for_plan(",
        '"tool_queue_health_summary": tool_queue_health_summary',
        "_batch_resource_flow_summary_for_plan(",
        '"batch_resource_flow_summary": batch_resource_flow_summary',
        "_geometry_fact_summary_for_plan(",
        '"geometry_fact_summary": geometry_fact_summary',
        '"geometry_fact_replay_summary": dict(',
    ):
        if required not in generate_report:
            violations.append(f"AgentRuntime.generate_report missing Runtime queue/batch summary token: {required}")
    batch_resource_flow_summary = _function_source(source, "_batch_resource_flow_summary_for_plan")
    import_summary = _function_source(source, "_import_summary_for_plan")
    emit_resource_stage_events = _function_source(source, "_emit_resource_stage_events_for_graph")
    if '"ready_count" in import_fact' not in batch_resource_flow_summary:
        violations.append("AgentRuntime batch resource flow must preserve explicit import ready_count=0")
    if 'import_fact.get("ready_count") or import_fact.get("actor_count")' in batch_resource_flow_summary:
        violations.append("AgentRuntime batch resource flow must not coerce ready_count=0 to actor_count")
    for token in (
        "aggregate_import_failure_code_counts",
        '"import_failure_code_counts": dict(sorted(aggregate_import_failure_code_counts.items()))',
    ):
        if token not in import_summary:
            violations.append(f"AgentRuntime import summary missing import failure-code token: {token}")
    if '"import_failure_code_counts": {"actor_import_provider_failed": 2}' not in phase1_test_source:
        violations.append("AgentRuntime import summary failure-code regression assertion missing")
    for token in (
        "_actor_import_failure_code_counts_for_batch(",
        '"import_failure_code_counts"',
    ):
        if token not in emit_resource_stage_events:
            violations.append(f"AgentRuntime actor import event missing failure-code payload token: {token}")
    if "_safe_user_visible_failure_code(" not in source:
        violations.append("AgentRuntime actor import event missing safe user-visible failure-code helper")
    if '"import_failure_code_counts": {"actor_import_adapter_failed": 2}' not in phase1_test_source:
        violations.append("AgentRuntime actor import failed event must expose safe adapter failure-code regression")
    if '"import_failure_code_counts": {"cpp_actor_import_failed": 1}' not in phase1_test_source:
        violations.append("AgentRuntime actor import partial event failure-code regression assertion missing")
    if not engine_write_replay_summary:
        violations.append("AgentRuntime._engine_write_replay_summary not found")
    else:
        for token in (
            "status_export_count",
            "latest_status_export",
            "readiness_mismatch_count",
            "readiness_mismatch_channels",
            "runtime_engine_write_status_exported",
            "engine_write_import_boundary_count",
            "engine_write_environment_import_boundary_count",
            "engine_write_transform_boundary_count",
            "engine_write_delete_boundary_count",
            "engine_write_bridge_error_code_counts",
        ):
            if token not in engine_write_replay_summary:
                violations.append(f"AgentRuntime._engine_write_replay_summary missing status-export token: {token}")
    report_health_summary_function = _function_source(source, "_report_health_summary")
    if not report_health_summary_function:
        violations.append("AgentRuntime._report_health_summary not found")
    else:
        for token in (
            "engine_write_summary: Mapping[str, Any] | None = None",
            "engine_write_state = dict(engine_write_summary or {})",
            "engine_write_readiness_mismatch_count",
            "engine_write_readiness_mismatch_channels",
            "engine_write_readiness_mismatch",
            'status = "needs_attention"',
        ):
            if token not in report_health_summary_function:
                violations.append(f"AgentRuntime._report_health_summary missing engine-write mismatch token: {token}")
    for token in (
        "engine_write_readiness_mismatch_count",
        "engine_write_readiness_mismatch_channels",
        "engine-write mismatch",
    ):
        if token not in report_health_formatter:
            violations.append(f"LANChat report health formatter missing engine-write mismatch token: {token}")
    for token in (
        'engine_replay["status_export_count"]',
        'engine_replay["latest_status_export"]',
        '"runtime_engine_write_status_exported"',
    ):
        if token not in phase1_test_source:
            violations.append(f"AgentRuntime engine-write status export replay regression missing token: {token}")
    for token in (
        '"engine_write_readiness_native_enabled_count"',
        '"engine_write_readiness_runtime_state_only_count"',
        '"engine_write_readiness_native_enabled_channels"',
        '"engine_write_readiness_runtime_state_only_channels"',
        'readiness native:1',
        'channels native actor-import',
        'readiness-mismatch 1(layout-transform)',
        'engine-write mismatch 1(layout-transform)',
        'report_health["status"], "needs_attention"',
        'engine_write_readiness_mismatch", report_health["reasons"]',
    ):
        if token not in test_source:
            violations.append(f"LANChat engine-write status export readiness regression missing token: {token}")
    if "runtime.user_report.persist" not in persist_user_report or "executor.execute(graph" not in persist_user_report:
        violations.append("AgentRuntime._persist_user_report must use ToolCallGraphExecutor")
    for token in (
        "safe_apply_reason = \"completed\" if applied else \"RuntimeState persistence failed\"",
        '"failure_code": "" if applied else "user_report_state_persist_failed"',
        '"reason": "" if applied else safe_apply_reason',
    ):
        if token not in persist_user_report:
            violations.append(f"AgentRuntime._persist_user_report missing safe failure audit token: {token}")
    for token in (
        'failed_payload["failure_code"]',
        'failed_payload["reason"]',
        'self.assertNotIn("provider", str(failed_payload).lower())',
    ):
        if token not in phase1_test_source:
            violations.append(f"AgentRuntime user report persist failure regression missing token: {token}")
    if "ReportRecordValidator.validate(report)" not in persist_user_report_tool:
        violations.append("runtime.user_report.persist must validate ReportRecord before StatePatch persistence")
    for token in (
        "runtime.audit_event.record",
        "self._record_audit_event_tool",
        'required_args=("room_id", "event_name", "payload")',
    ):
        if token not in source:
            violations.append(f"AgentRuntime audit event tool registration missing token: {token}")
    for token in (
        'tool_name="runtime.audit_event.record"',
        "ToolCallGraphExecutor(",
        "executor.execute(graph",
        "tool_graph_id",
        "tool_call_status",
    ):
        if token not in handle_message:
            violations.append(f"AgentRuntime runtime_audit_event branch must go through ToolCallGraph: {token}")
    for token in (
        '"intent"',
        '"route"',
        '"target_agent"',
    ):
        if token not in handle_message:
            violations.append(f"AgentRuntime runtime_audit_event branch must preserve safe semantic audit field: {token}")
    for token in (
        "self.operation_log.append(",
        "OperationLog._safe_payload",
        "entry.event",
        "operation_log_index",
    ):
        if token not in record_audit_event_tool:
            violations.append(f"runtime.audit_event.record tool missing safe OperationLog write token: {token}")
    for token in (
        '"intent"',
        '"route"',
        '"target_agent"',
    ):
        if token not in record_audit_event_tool:
            violations.append(f"runtime.audit_event.record tool must preserve safe semantic audit field: {token}")
    if "test_handle_message_runtime_audit_event_records_safe_operation_log_without_creating_plan" not in phase1_test_source:
        violations.append("AgentRuntime audit event ToolCallGraph path missing regression test")
    if "runtime.audit_event.record" not in phase1_test_source or "tool_graph_id" not in phase1_test_source:
        violations.append("AgentRuntime audit event regression must assert tool graph execution")
    for token in (
        "runtime.planning_context.persist",
        "self._persist_planning_context_tool",
        'required_args=("room_id", "changes", "context_event")',
        'produces_state=("active_plan_id", "scene_plans", "planning_context_events")',
    ):
        if token not in source:
            violations.append(f"AgentRuntime planning context tool registration missing token: {token}")
    for token in (
        'tool_name="runtime.planning_context.persist"',
        "ToolCallGraphExecutor(",
        "executor.execute(graph",
        "context_call.status == ToolCallStatus.SUCCEEDED",
    ):
        if token not in execute_planning_context_persist_graph:
            violations.append(f"AgentRuntime planning context persist must go through ToolCallGraph: {token}")
    for token in (
        "PlanningContextEventValidator.validate(context_event)",
        "StatePatchValidator.validate(state_patch, self.state.room(room))",
        "state_patch=state_patch",
    ):
        if token not in persist_planning_context_tool:
            violations.append(f"runtime.planning_context.persist tool missing validator/token: {token}")
    for token in (
        "_persist_planning_context_changes(",
        "context_type=\"agent_reply\" if plan_id else \"room_agent_reply\"",
        "reply_to=reply_to",
    ):
        if token not in record_agent_context_message:
            violations.append(f"AgentRuntime.record_agent_context_message missing context handoff token: {token}")
    for token in (
        "_persist_planning_context_event(",
        'context_type = "user_discussion" if plan_id else "room_chat"',
        "speaker_type=\"user\"",
    ):
        if token not in record_user_context_message:
            violations.append(f"AgentRuntime.record_user_context_message missing context handoff token: {token}")
    if "runtime.planning_context.persist" not in phase1_test_source:
        violations.append("AgentRuntime planning context ToolCallGraph path missing regression assertion")
    if '"final_adjustment_confirmation_replay_summary"' not in report_record_allowed_fields:
        violations.append(
            "ReportRecordValidator must allow final_adjustment_confirmation_replay_summary as a persisted user-report field"
        )
    for reply_token in (
        "graph_status_text",
        "health_text",
        "state",
    ):
        if reply_token not in runtime_execution_reply:
            violations.append(f"LANChatAgentWorker Runtime execution reply missing token: {reply_token}")
    if "待 worker drain 执行" not in runtime_execution_reply:
        violations.append("Runtime execution reply policy missing queued-state wording")
    for intervention_reply_token in (
        "patch_type",
        "status",
        "items",
    ):
        if intervention_reply_token not in runtime_intervention_reply:
            violations.append(
                f"LANChatAgentWorker Runtime intervention reply missing token: {intervention_reply_token}"
            )
    if "AgentRuntime execution result" in runtime_intervention_reply:
        violations.append("LANChatAgentWorker Runtime intervention reply must not collapse patch facts into a generic result")
    for layout_reply_token in (
        "ToolCallGraph",
        "graph",
        "applied_deltas",
        "skipped_deltas",
        "engine_transform_results",
        "ground_snapped",
        "overlap_resolved",
    ):
        if layout_reply_token not in runtime_layout_reply:
            violations.append(
                f"LANChatAgentWorker Runtime layout confirmation reply missing token: {layout_reply_token}"
            )
    if "AgentRuntime 鎵ц缁撴灉锛氬凡搴旂敤" in runtime_layout_reply:
        violations.append("LANChatAgentWorker Runtime layout confirmation reply must not collapse graph/proposal facts")
    if "mojibake" in runtime_layout_reply:
        violations.append("LANChatAgentWorker Runtime layout confirmation reply must not preserve mojibake text")
    for layout_report_token in (
        "applied_delta_count",
        "skipped_delta_count",
        "transform_result_count",
        "ground_snapped_count",
        "overlap_resolved_count",
        "layout_transform_failure_code_counts",
        "transform-failures",
        "ground-snapped",
        "overlap-resolved",
    ):
        if layout_report_token not in runtime_layout_report:
            violations.append(f"LANChatAgentWorker Runtime layout report missing token: {layout_report_token}")
    if "mojibake" in runtime_layout_report:
        violations.append("LANChatAgentWorker Runtime layout report must not preserve mojibake text")
    for status_token in (
        '"resource_readiness_replay_summary"',
        '"resource_readiness_publish_count"',
        '"resource_readiness_publish_failed_count"',
        '"resource_readiness_query_count"',
        '"resource_readiness_publish_requested_total"',
        '"resource_readiness_publish_enabled_total"',
        '"resource_readiness_publish_unavailable_total"',
        '"gm_summary_replay_summary"',
        '"gm_summary_exported_count"',
        '"gm_summary_failed_count"',
        '"gm_summary_resource_readiness_publish_total"',
        '"gm_summary_resource_readiness_query_total"',
        '"batch_execution_replay_summary"',
        '"tool_graph_queue_replay_summary"',
        '"batch_execution_started_count"',
        '"batch_execution_completed_count"',
        '"batch_execution_finalized_count"',
        '"tool_graph_queue_queued_count"',
        '"tool_graph_queue_dequeued_count"',
        '"tool_graph_queue_rejected_count"',
        '"tool_graph_queue_blocked_count"',
        '"resource_phase_failure_code_counts"',
        '"import_failure_code_counts"',
        '"sync_failure_code_counts"',
        '"latest_sync_failure_code"',
        '"layout_proposal_count"',
        '"layout_applied_delta_count"',
        '"layout_skipped_delta_count"',
        '"layout_transform_result_count"',
        '"layout_ground_snapped_count"',
        '"layout_overlap_resolved_count"',
        '"layout_transform_failure_code_counts"',
        '"engine_write_boundary_fact_count"',
        '"engine_write_bridge_call_count"',
        '"engine_write_bridge_success_count"',
        '"engine_write_bridge_failed_count"',
        '"engine_write_bridge_error_code_counts"',
    ):
        if status_token not in status_summary:
            violations.append(f"AgentRuntime.status_summary runtime_status_queried payload missing token: {status_token}")
    for token in (
        "_layout_support_type(actor)",
        "_shift_actor_aabb(",
        "_snap_actor_bottom_to_ground_if_supported(",
        "runtime_layout_transform_result",
        '"custom_report_facts": {layout_fact_key: layout_fact}',
        "_layout_transform_sync_changes(",
        "runtime_layout_transform",
    ):
        if token not in apply_layout_delta_tool:
            violations.append(f"runtime.layout.apply_delta missing selective grounding token: {token}")
    for helper in (
        "def _batch_tooling_summary_for_plan(",
        "def _batch_resource_flow_summary_for_plan(",
        "def _tool_queue_health_summary_for_plan(",
        "def _geometry_fact_summary_for_plan(",
        "def _geometry_fact_replay_summary(",
        "def _summarize_geometry_facts_for_replay(",
        "def _layout_support_type(",
        "def _shift_actor_aabb(",
        "def _snap_actor_bottom_to_ground_if_supported(",
        "def _layout_transform_sync_changes(",
    ):
        if helper not in source:
            violations.append(f"AgentRuntime missing Agent-native Runtime helper: {helper}")
    for test_name in (
        "test_runtime_cpp_bridge_success_payload_is_narrow_and_sanitized",
        "test_runtime_cpp_bridge_failure_message_is_sanitized",
        "test_runtime_cpp_bridge_missing_gate_method_is_stable",
        "test_runtime_tool_manifest_exposes_engine_plane_tools_without_internals",
    ):
        if test_name not in test_source:
            violations.append(f"RuntimeCppBridge boundary missing regression test: {test_name}")
    for bridge_token in (
        "boundary_fact",
        "bridge_call_count",
        "bridge_success_count",
        "bridge_failed_count",
        "bridge_error_code_counts",
        "_merge_bridge_boundary_facts",
    ):
        if bridge_token not in adapters_source:
            violations.append(f"RuntimeCppBridge adapter missing boundary fact token: {bridge_token}")
    for bridge_token in (
        "bridge_call_count",
        "bridge_success_count",
        "bridge_failed_count",
        "bridge_method_counts",
        "bridge_error_code_counts",
    ):
        if bridge_token not in source or bridge_token not in tools_source or bridge_token not in test_source:
            violations.append(f"Engine write boundary missing bridge token across Runtime layers: {bridge_token}")
    for import_failure_token in (
        "missing_ready_model_resource",
        "cpp_actor_import_failed",
        "actor_import_invalid_result",
        "cpp_environment_component_import_failed",
        "missing_transform_target",
        "cpp_actor_transform_failed",
        "missing_delete_target",
        "cpp_actor_delete_failed",
    ):
        if import_failure_token not in adapters_source:
            violations.append(f"Runtime engine write provider missing safe failure code: {import_failure_token}")
    safe_engine_result_rows = _function_source(source, "_safe_engine_result_rows")
    if '"failure_code"' not in safe_engine_result_rows:
        violations.append("ToolCallGraphExecutor engine-write replay summary must preserve safe failure_code")
    if "def _safe_environment_import_results(" not in tools_source or '"failure_code"' not in tools_source:
        violations.append("Runtime environment import result facts must preserve safe failure_code")
    for token in (
        "def _failed_component_patch_result(",
        '"custom_import_facts"',
        "_environment_import_result_fact(",
        "_environment_import_boundary_fact(",
    ):
        if token not in tools_source:
            violations.append(f"Runtime failed environment import path missing replay fact token: {token}")
    for test_name in (
        "test_engine_actor_import_provider_failure_codes_are_safe",
        "test_engine_environment_component_import_provider_failure_code_is_safe",
        "test_runtime_environment_import_tool_uses_provider_and_persists_sanitized_components",
        "test_runtime_environment_import_failure_preserves_provider_failure_code_fact",
        "test_engine_actor_delete_provider_failure_code_is_safe",
        "test_engine_layout_transform_provider_respects_status_and_success_failure",
        "test_engine_layout_transform_provider_keeps_partial_success_when_one_actor_fails",
        "test_engine_write_replay_summary_sanitizes_raw_engine_results",
    ):
        if test_name not in phase1_test_source:
            violations.append(f"Runtime engine write failure-code boundary missing regression test: {test_name}")
    for required_tool in (
        "runtime.environment.import_components",
        "runtime.actor.import_batch",
        "runtime.layout.apply_delta",
        "runtime.actor.mark_deleted",
    ):
        if required_tool not in test_source:
            violations.append(f"Runtime tool manifest regression missing engine-plane tool: {required_tool}")
    for test_name in (
        "test_runtime_guard_blocks_unconfirmed_high_risk_tool",
        "test_runtime_guard_blocks_unconfirmed_low_risk_write_tool",
        "test_runtime_guard_uses_tool_definition_requires_write_even_when_call_omits_it",
        "test_runtime_guard_blocks_confirmed_system_actor_write_by_actor_id",
        "test_runtime_guard_blocks_nested_system_actor_write_reference",
        "test_runtime_guard_system_actor_ref_matches_room_and_terrain_without_false_sky_prefix",
        "test_tool_definition_default_high_risk_requires_confirmation",
    ):
        if test_name not in phase1_test_source:
            violations.append(f"RuntimeGuard boundary missing regression test: {test_name}")
    for token in (
        'blocked_log.payload["guard_reason"]',
        'emitted[-1].payload["guard_reason"]',
        "expected_guard_payload",
        '"requires_write": True',
        '"confirmed": False',
    ):
        if token not in phase1_test_source:
            violations.append(f"RuntimeGuard blocked-call audit regression missing token: {token}")
    for test_name in REQUIRED_STATE_PATCH_CONFLICT_TESTS:
        if test_name not in phase1_test_source:
            violations.append(f"RuntimeState StatePatch conflict/reconcile missing regression test: {test_name}")
    for tool_name in (
        "runtime.queue.plan_enqueue_items",
        "runtime.geometry.compute_aabb",
        "runtime.geometry.check_overlap",
        "runtime.geometry.snap_to_ground_selective",
    ):
        if tool_name not in tools_source:
            violations.append(f"AgentRuntime required Runtime tool missing: {tool_name}")
    for token in (
        "def _failed_resource_entries(",
        '"failure_code": safe_source',
        '"failure_code_counts"',
        "image_resource_unavailable",
        "model_resource_unavailable",
        "missing_ready_model_resource",
        'plan_status = "failed"',
        'plan_status = "partial"',
        '"runtime.actor.import_batch"',
        'produces_state=("actors", "custom_import_facts")',
        "def _actor_import_result_fact(",
        '"failure_code": failure_code',
        "actor import skipped and failed resource fact recorded",
        "actor import failed and result fact recorded",
        "runtime_actor_import_result",
        ':actor_import_result',
    ):
        if token not in tools_source:
            violations.append(f"AgentRuntime resource provider result handling missing token: {token}")
    safe_actor_import_results = tools_source[
        tools_source.find("def _safe_actor_import_results("):
        tools_source.find("def _actor_import_boundary_fact(", tools_source.find("def _safe_actor_import_results("))
    ]
    if '"failure_code"' not in safe_actor_import_results:
        violations.append("Runtime actor import result facts must preserve safe failure_code")
    image_allowed_fields = source[
        source.find("_IMAGE_ALLOWED_FIELDS = {"):
        source.find("_MODEL_ALLOWED_FIELDS = {", source.find("_IMAGE_ALLOWED_FIELDS = {"))
    ]
    if '"failure_code"' not in image_allowed_fields:
        violations.append("Runtime image resource plans must preserve safe failure_code")
    batch_resource_summary = source[
        source.find("def _batch_resource_flow_summary_for_plan("):
        source.find("def _scene_snapshot_summary_for_plan(", source.find("def _batch_resource_flow_summary_for_plan("))
    ]
    for token in (
        "def failure_code_counts(",
        "def import_failure_code_counts(",
        '"image_failure_code_counts"',
        '"model_failure_code_counts"',
        '"import_failure_code_counts"',
        '"import_failure_code_counts": dict(',
    ):
        if token not in batch_resource_summary:
            violations.append(f"Runtime batch resource flow must preserve resource failure-code diagnostics: {token}")
    report_health_summary = source[
        source.find("def _report_health_summary("):
        source.find("def _batch_ids_for_plan(", source.find("def _report_health_summary("))
    ]
    for token in (
        "resource_phase_failure_code_counts",
        "failure_code_counts = dict(row.get(\"failure_code_counts\") or {})",
        "import_failure_code_counts",
        'dict(batch_flow.get("import_failure_code_counts") or {})',
        "environment_import_failure_code_counts",
        'dict(environment_state.get("import_failure_code_counts") or {})',
    ):
        if token not in report_health_summary:
            violations.append(f"Runtime report health must preserve resource failure-code diagnostics: {token}")
    for token in (
        '"resource_phase_failure_code_counts": dict(',
        '"import_failure_code_counts": dict(',
        '"environment_import_failure_code_counts": dict(',
        '"engine_write_readiness_mismatch_count": engine_write_readiness_mismatch_count',
        '"engine_write_readiness_mismatch_channels": engine_write_readiness_mismatch_channels',
    ):
        if token not in source:
            violations.append(f"Runtime report_ready payload must expose safe resource failure-code diagnostics: {token}")
    runtime_event_validator_source = source[
        source.find("class RuntimeEventValidator:"):
        source.find("class SyncEventValidator:", source.find("class RuntimeEventValidator:"))
    ]
    agent_runtime_event_payload_keys = source[
        source.find("_SAFE_RUNTIME_EVENT_PAYLOAD_KEYS = {"):
        source.find("def _build_provider_summary(", source.find("_SAFE_RUNTIME_EVENT_PAYLOAD_KEYS = {"))
    ]
    for token in (
        '"engine_write_readiness_mismatch_count"',
        '"engine_write_readiness_mismatch_channels"',
    ):
        if token not in runtime_event_validator_source:
            violations.append(f"RuntimeEventValidator safe payload allowlist missing token: {token}")
        if token not in agent_runtime_event_payload_keys:
            violations.append(f"AgentRuntime user-visible event payload allowlist missing token: {token}")
    operation_log_safe_payload = source[
        source.find("def _safe_payload(payload: Mapping[str, Any])"):
        source.find("blocked_keys = {", source.find("def _safe_payload(payload: Mapping[str, Any])"))
    ]
    for token in (
        '"fields"',
        '"field_count"',
        '"field_names"',
    ):
        if token not in operation_log_safe_payload:
            violations.append(f"OperationLog._safe_payload missing runtime fact injection token: {token}")
        if token not in runtime_event_validator_source:
            violations.append(f"RuntimeEventValidator safe payload allowlist missing runtime fact injection token: {token}")
    for token in (
        'normalized_key in {"field_names", "fields"}',
        'RuntimeEventValidator.safe_text(item) if isinstance(item, str) else item',
    ):
        if token not in runtime_event_validator_source:
            violations.append(f"RuntimeEventValidator safe payload missing runtime fact injection list sanitizer: {token}")
    for token in (
        '"field_count": len(injected)',
        '"field_names": injected',
    ):
        if token not in execute:
            violations.append(f"ToolCallGraphExecutor runtime fact injection audit missing token: {token}")
    for token in (
        '"tool_call_runtime_facts_injected"',
        '"runtime_fact_injection_count"',
        '"runtime_fact_injection_field_counts"',
        '"runtime_fact_injection_replay_summary"',
    ):
        if token not in phase1_test_source:
            violations.append(f"AgentRuntime runtime fact injection safe replay regression missing token: {token}")
    for token in (
        '"operation_log_event"',
        '"operation_log_index"',
    ):
        if token not in operation_log_safe_payload:
            violations.append(f"OperationLog._safe_payload missing report provenance token: {token}")
        if token not in runtime_event_validator_source:
            violations.append(f"RuntimeEventValidator safe payload allowlist missing report provenance token: {token}")
    for token in (
        'replay_payload = replay["entries"][-1]["payload"]',
        'replay_payload["operation_log_event"]',
        'replay_payload["operation_log_index"]',
    ):
        if token not in phase1_test_source:
            violations.append(f"AgentRuntime user report persist replay regression missing token: {token}")
    for name, section in (
        ("OperationLog safe payload allowlist", operation_log_safe_payload),
        ("RuntimeEventValidator", runtime_event_validator_source),
        ("AgentRuntime user-visible event payload allowlist", agent_runtime_event_payload_keys),
    ):
        for token in (
            '"guard_reason"',
            '"risk_level"',
            '"requires_write"',
            '"confirmed"',
        ):
            if token not in section:
                violations.append(f"{name} must allow RuntimeGuard blocked-call audit field: {token}")
    for name, section in (
        ("RuntimeEventValidator", runtime_event_validator_source),
        ("AgentRuntime user-visible event payload allowlist", agent_runtime_event_payload_keys),
    ):
        for token in (
            '"resource_phase_failure_code_counts"',
            '"import_failure_code_counts"',
            '"environment_import_failure_code_counts"',
            '"sync_event_count"',
            '"sync_actor_transform_count"',
        ):
            if token not in section:
                violations.append(f"{name} must allow safe resource failure-code diagnostics: {token}")
    message_delivery_summary = source[
        source.find("def _message_delivery_replay_summary("):
        source.find("def _planning_context_replay_summary(", source.find("def _message_delivery_replay_summary("))
    ]
    for token in (
        '"failure_code_counts"',
        '"latest_failure_code"',
        'payload.get("failure_code") or payload.get("error_code") or payload.get("reason")',
    ):
        if token not in message_delivery_summary:
            violations.append(f"Runtime message delivery replay must preserve safe failure diagnostics: {token}")
    message_delivery_digest = source[
        source.find("message_delivery_digest = {"):
        source.find("latest_resource_batches =", source.find("message_delivery_digest = {"))
    ]
    for token in ('"failure_code_counts"', '"latest_failure_code"'):
        if token not in message_delivery_digest:
            violations.append(f"Runtime message delivery digest must expose safe failure diagnostics: {token}")
    sync_replay_summary = source[
        source.find("def _sync_replay_summary("):
        source.find("def _asset_transfer_replay_summary(", source.find("def _sync_replay_summary("))
    ]
    for token in (
        '"failure_code_counts"',
        '"latest_failure_code"',
        'payload.get("failure_code")',
    ):
        if token not in sync_replay_summary:
            violations.append(f"Runtime sync replay must preserve safe failure diagnostics: {token}")
    sync_summary_for_plan = source[
        source.find("def _sync_summary_for_plan("):
        source.find("def _asset_transfer_summary_for_plan(", source.find("def _sync_summary_for_plan("))
    ]
    for token in ('"actor_transform_count"', '"actor_delete_count"', '"event_type_counts"'):
        if token not in sync_summary_for_plan:
            violations.append(f"Runtime sync summary must expose actor transform/delete diagnostics: {token}")
    sync_health_digest = source[
        source.find("def _sync_health_digest_for_report("):
        source.find("def _report_health_summary(", source.find("def _sync_health_digest_for_report("))
    ]
    for token in ('"sync_failure_code_counts"', '"latest_sync_failure_code"'):
        if token not in sync_health_digest:
            violations.append(f"Runtime sync health digest must expose safe failure diagnostics: {token}")
    report_health_summary = source[
        source.find("def _report_health_summary("):
        source.find("def _batch_ids_for_plan(", source.find("def _report_health_summary("))
    ]
    for token in (
        '"sync_failure_code_counts"',
        '"latest_sync_failure_code"',
        '"sync_actor_transform_count"',
        '"sync_actor_delete_count"',
    ):
        if token not in report_health_summary:
            violations.append(f"Runtime report health summary must preserve safe sync diagnostics: {token}")
    for token in (
        '"sync_failure_code_counts"',
        '"latest_sync_failure_code"',
        '"sync_event_count"',
        '"sync_actor_transform_count"',
        '"sync_actor_delete_count"',
    ):
        if token not in runtime_event_validator_source:
            violations.append(f"RuntimeEventValidator must allow safe sync diagnostics: {token}")
    if "without calling providers" in tools_source:
        violations.append("AgentRuntime resource tool manifest must not claim providers are never called")
    for token in (
        "def _resource_provider_failure_tool_result(",
        "failed_resources = _failed_resource_entries(",
        "_resource_phase_fact_key(batch_id, kind)",
        "_resource_phase_fact(",
    ):
        if token not in tools_source:
            violations.append(f"AgentRuntime provider failure must record resource phase facts: {token}")
    for token in (
        "actor_import provider failed; recorded failed import fact",
        "actor_import_provider_failed",
        "environment_import_result",
        "_environment_import_result_fact(",
    ):
        if token not in tools_source:
            violations.append(f"AgentRuntime import provider failure must record import result facts: {token}")
    for token in (
        "def _plan_queue_items_via_tool_graph(",
        "runtime.queue.plan_enqueue_items",
        "queue_item_plan_tool_failed",
    ):
        if token not in source:
            violations.append(f"AgentRuntime enqueue_planned_batches missing queue planning token: {token}")
    for token in (
        "def drain_next_tool_graph(",
        "runtime.queue.select_next_graph",
        "def _persist_tool_graph_state(",
        "runtime.queue.record_graph_state",
        "def _mark_tool_graph_queue_item(",
        "runtime.queue.mark_graph_status",
    ):
        if token not in source:
            violations.append(f"AgentRuntime ToolCallGraph queue executor missing queue ToolCall token: {token}")
    for token in (
        "skipped_count = 0",
        "skipped_count += 1",
        '"skipped_count": skipped_count',
        "skipped_count=skipped_count",
        '"skipped_count": max(0, int(skipped_count or 0))',
    ):
        if token not in source:
            violations.append(f"ToolCallGraphExecutor stopped-by-command audit missing skipped-count token: {token}")
    for token in (
        'paused["command"]["command_recorded"]',
        'paused["command"]["tool_call_status"]',
        'latest_runtime_event["payload"]["command_recorded"]',
        'latest_runtime_event["payload"]["tool_call_status"]',
    ):
        if token not in phase1_test_source:
            violations.append(f"Runtime command ToolCall evidence regression missing token: {token}")
    for token in (
        'replay["snapshot_recorded"]',
        'replay["snapshot_status"]',
        'replay["snapshot_tool_status"]',
        'replay["snapshot_state_version"]',
        'replay_without_snapshot_evidence.pop(key, None)',
    ):
        if token not in phase1_test_source:
            violations.append(f"Runtime operation replay ToolCall evidence regression missing token: {token}")
    for token in (
        'status["snapshot_recorded"]',
        'status["snapshot_status"]',
        'status["snapshot_tool_status"]',
        'status["snapshot_state_version"]',
        'status_without_snapshot_evidence.pop(key, None)',
        'summary["snapshot_recorded"]',
        'summary["snapshot_status"]',
        'summary["snapshot_tool_status"]',
        'summary["snapshot_state_version"]',
        'summary_without_snapshot_evidence.pop(key, None)',
    ):
        if token not in phase1_test_source:
            violations.append(f"Runtime status/GM summary ToolCall evidence regression missing token: {token}")
    for token in (
        'result["snapshot_recorded"]',
        'result["snapshot_status"]',
        'result["snapshot_tool_status"]',
        'result["snapshot_state_version"]',
        'sync_status_without_snapshot_evidence.pop(key, None)',
        'provider_status_without_snapshot_evidence.pop(key, None)',
    ):
        if token not in phase1_test_source:
            violations.append(f"Runtime sync/provider ToolCall evidence regression missing token: {token}")
    for token in (
        'result["snapshot_recorded"]',
        'result["snapshot_status"]',
        'result["snapshot_tool_status"]',
        'result["snapshot_state_version"]',
        'self.assertNotIn("snapshot_recorded", events_fact)',
        'self.assertNotIn("snapshot_state_version", events_fact)',
    ):
        if token not in phase1_test_source:
            violations.append(f"Runtime events ToolCall evidence regression missing token: {token}")
    if "test_tool_graph_executor_stops_before_next_tool_when_plan_is_paused" not in phase1_test_source:
        violations.append("ToolCallGraph stopped-by-command skipped-count audit missing regression test")
    else:
        for token in (
            'stopped_log.payload["skipped_count"]',
            'emitted_logs[-1].payload["skipped_count"]',
            '{"status": "paused", "skipped_count": 1}',
        ):
            if token not in phase1_test_source:
                violations.append(f"ToolCallGraph stopped-by-command regression missing token: {token}")
    forbidden_runtime_tool_manifest_tokens = (
        "legacy.scene_compose",
        "legacy.progressive_compose",
        "legacy.workflow_orchestrator",
        "SceneComposer.compose",
        "ProgressiveWorkflow",
        "run_progressive_workflow",
    )
    forbidden_runtime_tool_manifest_phrases = (
        "mock.import_actor",
        "mock actor import",
        "mock import",
    )
    try:
        tools_tree = ast.parse(tools_source)
    except SyntaxError as exc:
        violations.append(f"agent_runtime/tools.py could not be parsed for manifest safety: {exc}")
        tools_tree = None
    if tools_tree is not None:
        for node in ast.walk(tools_tree):
            if not isinstance(node, ast.Call):
                continue
            func = node.func
            if not isinstance(func, ast.Attribute) or func.attr != "register":
                continue
            tool_name = ""
            if node.args and isinstance(node.args[0], ast.Constant):
                tool_name = str(node.args[0].value or "")
            description = ""
            for keyword in node.keywords:
                if keyword.arg == "description" and isinstance(keyword.value, ast.Constant):
                    description = str(keyword.value.value or "")
                    break
            manifest_text = f"{tool_name} {description}"
            for token in forbidden_runtime_tool_manifest_tokens:
                if token in manifest_text:
                    violations.append(
                        "AgentRuntime tool registry exposes legacy main-control token "
                        f"{token!r} in manifest entry {tool_name!r}"
                    )
            lowered_manifest_text = manifest_text.lower()
            for phrase in forbidden_runtime_tool_manifest_phrases:
                if phrase in lowered_manifest_text:
                    violations.append(
                        "AgentRuntime tool registry exposes mock import phrase "
                        f"{phrase!r} in manifest entry {tool_name!r}"
                    )
    for test_name in (
        "test_queue_enqueue_item_planning_tool_records_safe_drafts_without_persisting_queue",
        "test_drain_next_tool_graph_executes_queued_graph_as_runtime_worker_slice",
        "test_tool_registry_manifest_exposes_safe_capability_metadata",
        "test_tool_definition_rejects_legacy_workflow_main_control_tools",
        "test_tool_registry_manifest_does_not_expose_legacy_workflow_main_control_tools",
        "test_empty_resource_provider_result_records_failed_resource_facts",
        "test_empty_model_resource_provider_result_records_failed_resource_facts",
        "test_runtime_provider_failure_fails_graph_and_records_failed_resource_facts",
        "test_model_resource_provider_failure_emits_safe_runtime_event",
        "test_actor_import_provider_failure_emits_safe_runtime_event",
        "test_runtime_environment_import_failure_does_not_count_planned_components_as_imported",
    ):
        if test_name not in phase1_test_source:
            violations.append(f"legacy main-control manifest boundary missing regression test: {test_name}")
    for test_name in REQUIRED_PHASE6_GEOMETRY_TOOL_TESTS:
        if test_name not in phase1_test_source:
            violations.append(f"Phase 6 geometry tool missing regression test: {test_name}")

    if violations:
        print("[FAIL] static Runtime validator contract gate")
        for item in violations:
            _print_violation(item)
        return False
    print("[OK]  static Runtime validator contract gate")
    return True


def _scene_substrate_guardrail_gate() -> bool:
    print("[RUN] static scene substrate/layout guardrail gate")
    classifier_path = REPO_ROOT / SCENE_ELEMENT_CLASSIFIER
    phase1_test_path = REPO_ROOT / AGENT_RUNTIME_PHASE1_TESTS
    try:
        classifier_source = classifier_path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        classifier_source = classifier_path.read_text(encoding="utf-8-sig")
    try:
        phase1_test_source = phase1_test_path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        phase1_test_source = phase1_test_path.read_text(encoding="utf-8-sig")

    violations: list[str] = []
    classifier_contract_tokens = (
        "_SUBSTRATE_TERMS",
        "_LAYOUT_TERMS",
        '"grassland"',
        '"grass"',
        '"sky"',
        '"forest"',
        '"terrain"',
        '"ground"',
        '"ceiling"',
        '"entrance"',
        '"path"',
        '"main street"',
        '"boundary"',
        "folded = clean.lower()",
        "str(term).lower() == folded",
        "str(term).lower() in folded",
    )
    for token in classifier_contract_tokens:
        if token not in classifier_source:
            violations.append(f"SceneElementClassifier substrate guardrail missing token: {token}")

    phase1_regression_tokens = (
        "test_substrate_terms_are_classified_but_not_imported_as_actors",
        "test_layout_terms_are_classified_but_not_imported_as_actors",
        "Create a forest camp with sky, forest, grass, wooden table, and tent",
        "Create a market with entrance, main street, boundary, wooden table, and tent",
        'plan.concrete_object_items = ["wooden table", "tent"]',
        '["entrance", "main street", "boundary", "wooden table", "tent"]',
        'self.assertEqual(state["model_item_lists"][batch_id], ["wooden table", "tent"])',
        'self.assertEqual(set(state["image_resource_plans"][batch_id]), {"wooden table", "tent"})',
        'self.assertEqual(set(state["model_resource_plans"][batch_id]), {"wooden table", "tent"})',
        '{"forest", "sky", "grass"}.issubset',
        '{"entrance", "main street", "boundary"}.issubset',
        'route["target_pipeline"] == "layout_structure"',
        'self.assertEqual(status["classification_summary"]["model_items"], ["wooden table", "tent"])',
        'self.assertEqual(node["args"]["model_items"], ["wooden table", "tent"])',
    )
    for token in phase1_regression_tokens:
        if token not in phase1_test_source:
            violations.append(f"AgentRuntime substrate routing regression missing token: {token}")

    if violations:
        print("[FAIL] static scene substrate/layout guardrail gate")
        for item in violations:
            _print_violation(item)
        return False
    print("[OK]  static scene substrate/layout guardrail gate")
    return True


def main() -> int:
    checks: list[tuple[str, list[str]]] = []

    for path in PYTHON_TESTS:
        checks.append((path, [sys.executable, path]))

    for path in NODE_TESTS:
        checks.append((path, ["node", path]))

    failed = 0
    for label, command in checks:
        if not _run(label, command):
            failed += 1

    if not _syntax_check(PY_COMPILE_TARGETS):
        failed += 1

    if not _direct_scene_compose_entry_gate():
        failed += 1

    if not _direct_engine_write_entry_gate():
        failed += 1

    if not _runtime_adapter_engine_write_boundary_gate():
        failed += 1

    if not _master_agent_legacy_compose_route_gate():
        failed += 1

    if not _direct_progressive_workflow_entry_gate():
        failed += 1

    if not _direct_generation_scheduler_entry_gate():
        failed += 1

    if not _direct_host_action_executor_entry_gate():
        failed += 1

    if not _host_action_executor_policy_gate():
        failed += 1

    if not _legacy_agent_coordinator_policy_gate():
        failed += 1

    if not _legacy_role_agent_scene_write_policy_gate():
        failed += 1

    if not _agent_runtime_flag_boundary_gate():
        failed += 1

    if not _runtime_state_apply_patch_boundary_gate():
        failed += 1

    if not _workflow_command_exposure_gate():
        failed += 1

    if not _runtime_report_fact_source_gate():
        failed += 1

    if not _scene_substrate_guardrail_gate():
        failed += 1

    if not _runtime_validator_contract_gate():
        failed += 1

    if failed:
        print(f"[SUMMARY] {failed} non-native check(s) failed.")
        return 1

    print("[SUMMARY] All current Agent-native non-native checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

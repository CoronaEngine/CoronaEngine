"""The retired JSON manifest must not come back.

Web already talks to C++ through the named typed V8 contract, and the JSON
manifest (`kEditorApiMethods` + `EditorApiRegistry` + `NativeApiRegistry`) only
serves callers that are themselves gone.  This machine has no C++ toolchain, so
these structural guards are the executable specification for the deletion
programme: each test names one family that must disappear, and the Web surface it
must leave alone.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
API = ROOT / "src/systems/ui/editor_api/cef_editor_api.cpp"
REGISTRY = ROOT / "src/systems/ui/cef/cef_editor_native_api_registry.cpp"
HANDLERS = ROOT / "src/systems/ui/cef/cef_editor_native_api_handlers.cpp"
V8 = ROOT / "src/systems/ui/cef/cef_v8_binding.cpp"
PY_API = ROOT / "embedded_python/api/editor_api.py"


def _text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def _manifest_rows(text: str, module: str) -> list[str]:
    return [
        line.strip()
        for line in text.splitlines()
        if "EDITOR_API_METHOD_SCHEMA" in line and f"{module}," in line
    ]


# ---------------------------------------------------------------------------
# Network: the whole manifest family is dead.  `execute_network_operation` (the
# Web path) is the only survivor and it lives elsewhere.
# ---------------------------------------------------------------------------
NETWORK_V8_METHODS = [
    "startSession", "stopSession", "getSessionInfo", "getDiscoveredPeers",
    "clearDiscoveredPeers", "searchLan", "connectToPeer", "setProjectRoot",
]


def test_network_manifest_rows_are_gone():
    rows = _manifest_rows(_text(API), "Network")
    assert not rows, f"{len(rows)} Network manifest rows survived: {rows[:3]}"


def test_network_handler_module_is_gone():
    api = _text(API)
    assert "register_network_api_handlers" not in api
    handlers = _text(HANDLERS)
    assert "register_network_api_handlers" not in handlers
    assert "register_network_api_handlers" not in _text(REGISTRY)


def test_network_legacy_executor_is_gone():
    assert "execute_legacy_network_operation" not in _text(HANDLERS)


def test_network_python_manifest_namespace_is_gone():
    py = _text(PY_API)
    assert "_NetworkApi" not in py
    assert "get_network_adapter" not in py


def test_network_web_contract_is_untouched():
    v8 = _text(V8)
    for name in NETWORK_V8_METHODS:
        assert f'network->SetValue("{name}"' in v8, f"typed V8 lost corona.network.{name}"
    assert "execute_network_operation" in _text(HANDLERS), "Web network implementation disappeared"


# ---------------------------------------------------------------------------
# Zero-caller rows.  Nothing in the repository outside the manifest itself
# named these: no Python service, no script runtime, no integration script.
# ---------------------------------------------------------------------------
ZERO_CALLER_ROWS = [
    ("ResourceSearch", "fuzzy_search"),
    ("ResourceSearch", "get_stats"),
    ("ResourceSearch", "image_search"),
    ("ResourceSearch", "list_types"),
    ("ResourceSearch", "mark_index_dirty"),
    ("ResourceSearch", "prepare_index"),
    ("ResourceSearch", "rebuild_index"),
    ("SceneTools", "actor_play_audio"),
    ("SceneTools", "actor_stop_audio"),
    ("SceneTools", "get_actor_ik_chains"),
    ("SceneTools", "get_actor_skeleton_leaves"),
    ("SceneTools", "get_model_nodes"),
    ("SceneTools", "list_scene_tree"),
    ("SceneTools", "pick_actor"),
    ("SceneTools", "play_audio"),
    ("SceneTools", "rename_actor"),
    ("SceneTools", "select_screenshot_path"),
    ("SceneTools", "set_actor_ik_chains"),
    ("SceneTools", "set_model_node_state"),
    ("SceneTools", "stop_audio"),
]


def test_zero_caller_manifest_rows_are_gone():
    api = _text(API)
    survivors = [f"{m}.{fn}" for m, fn in ZERO_CALLER_ROWS if f"({m}, {fn}," in api]
    assert not survivors, f"rows with no caller survived: {survivors}"


def test_resource_search_has_no_manifest_entry_left():
    rows = _manifest_rows(_text(API), "ResourceSearch")
    assert not rows, f"ResourceSearch still has manifest rows: {rows[:3]}"

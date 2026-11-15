"""The ScriptRuntime contract is the table plus its bindings, and nothing else.

Names are declared once, in `editor_operation_table.h`. `cef_py_bind.cpp` binds the
`script` rows under `CoronaEngine.<group>.<name>` with real positional arguments; this
guard proves the two halves agree, that every binding names a row, and that the retired
JSON-string protocol (`args_json`, `{success, error_code}` envelopes) is gone.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / "src/systems/ui/cef/editor_operation_table.h"
BINDINGS = ROOT / "src/systems/ui/cef/cef_py_bind.cpp"
ENTRY_HEADER = ROOT / "src/systems/ui/cef/script_operation_entry.h"
ADAPTER = ROOT / "embedded_python/script_runtime/native_scene_adapter.py"

TABLE_ROW = re.compile(
    r'\{"(\w+)",\s*"(\w+)",\s*(SceneMethod::\w+|std::nullopt),\s*'
    r'(true|false),\s*(true|false),\s*(\d+),'
)
BOUND = re.compile(r'bind_script\("(\w+)",\s*"(\w+)"')


def table_rows():
    text = TABLE.read_text(encoding="utf-8-sig")
    start = text.index("kEditorOperations[] = {")
    end = text.index("};", start)
    return [
        {"group": group, "name": name, "web": web == "true", "script": script == "true"}
        for group, name, _method, web, script, _arity in TABLE_ROW.findall(text[start:end])
    ]


def bindings():
    return BOUND.findall(BINDINGS.read_text(encoding="utf-8-sig"))


def test_every_script_row_is_bound_exactly_once():
    expected = [(row["group"], row["name"]) for row in table_rows() if row["script"]]
    actual = bindings()
    duplicates = sorted({pair for pair in actual if actual.count(pair) > 1})
    assert not duplicates, f"bound more than once: {duplicates}"
    missing = sorted(set(expected) - set(actual))
    extra = sorted(set(actual) - set(expected))
    assert not missing, f"script rows without a binding: {missing}"
    assert not extra, f"bindings without a script row: {extra}"
    assert len(expected) == 18, f"the script contract is 18 operations, found {len(expected)}"


def test_bindings_group_by_namespace():
    groups = {group for group, _ in bindings()}
    assert groups == {"scene", "audio", "viewport"}, f"unexpected namespaces: {sorted(groups)}"


def test_every_bound_operation_has_an_entry_point():
    declared = set(re.findall(r"NativeResult (script_entry_\w+)\(",
                              ENTRY_HEADER.read_text(encoding="utf-8-sig")))
    used = set(re.findall(r"(script_entry_\w+)", BINDINGS.read_text(encoding="utf-8-sig")))
    assert not (used - declared), f"bindings call undeclared entries: {sorted(used - declared)}"
    assert len(declared) == 18, f"expected 18 typed entries, found {len(declared)}"


def test_no_json_string_transport_survives():
    source = BINDINGS.read_text(encoding="utf-8-sig")
    for retired in ('nb::arg("args_json")', "args_json", '"error_code"', '"success"'):
        assert retired not in source, f"the JSON-string protocol came back: {retired}"


def test_the_binding_checks_the_table_at_init():
    source = BINDINGS.read_text(encoding="utf-8-sig")
    assert "find_editor_operation" in source, (
        "bind_script must look the row up, so a renamed table row fails the module load")


def test_the_adapter_is_a_thin_export_of_the_three_namespaces():
    """The Python adapter must not model its own API: it re-exports CoronaEngine's."""
    source = ADAPTER.read_text(encoding="utf-8-sig")
    for retired in ("_OPERATIONS", "invoke_native_scene", "ScriptRuntimeSceneAdapter",
                    "ScriptRuntimeSceneToolsAdapter", "ScriptRuntimeViewportAdapter"):
        assert retired not in source, f"the adapter kept its own surface: {retired}"
    exposed = set(re.findall(r"CoronaEngine\.(\w+)", source))
    assert exposed == {"scene", "audio", "viewport"}, f"unexpected namespaces: {sorted(exposed)}"

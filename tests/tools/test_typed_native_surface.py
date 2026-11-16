"""The typed native surface is the only contract Web has.

With the JSON manifest retired there is exactly one way for the renderer to reach
C++: the `window.corona` object installed by `attach_corona_v8_binding`, plus its
single `bridge.call` entry to Python. These guards pin that surface exactly, so
a namespace cannot quietly grow a second transport or lose a method.

Every name is installed from `editor_operation_table.h` now - the operation rows carry the
enum that owns them, and the binding publishes `name` verbatim (snake_case, like the wire
keys). The scene rows still have a second half in the binding (`kSceneReaders`, the argument
readers), which a row cannot name because a reader takes `CefV8ValueList` and cannot live in
the CEF-free header.
"""
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from native_surface import EXPECTED_SURFACE
import operation_table

ROOT = Path(__file__).resolve().parents[2]
V8 = ROOT / "src/systems/ui/cef/cef_v8_binding.cpp"
FRONTEND_SRC = ROOT / "CoronaEditor/editor/Frontend/src"


def _binding_source() -> str:
    return V8.read_text(encoding="utf-8-sig")


def _reader_methods() -> list[str]:
    """The SceneMethods `kSceneReaders` knows how to decode arguments for."""
    source = _binding_source()
    start = source.index("kSceneReaders[] = {")
    end = source.index("};", start)
    return re.findall(r"\{SceneMethod::(\w+),", source[start:end])


def _table_surface() -> dict[str, set[str]]:
    """Names the table declares: the spelling the binding publishes."""
    surface: dict[str, set[str]] = {}
    for row in operation_table.web_rows():
        surface.setdefault(row["group"], set()).add(row["name"])
    return surface


def test_the_table_publishes_exactly_the_pinned_surface():
    # native_surface.EXPECTED_SURFACE is written by hand so that it stays an independent
    # check: if this were derived from the table too, a wrong row would redefine the
    # contract instead of failing here.
    assert _table_surface() == EXPECTED_SURFACE


def test_every_scene_operation_has_an_argument_reader():
    # Without this, a row installed from the table but missing from kSceneReaders answers
    # INVALID_ARGUMENT at run time, and nothing in the build says so. Only the scene rows
    # have a reader here: the other namespaces' readers are chosen by the method's own type
    # (`invoke(NetworkMethod)`, `invoke(WindowMethod)`, ...), so the compiler checks them.
    readers = _reader_methods()
    duplicates = sorted({name for name in readers if readers.count(name) > 1})
    assert not duplicates, f"methods with more than one reader: {duplicates}"
    missing = [f"{row['group']}.{row['name']} -> SceneMethod::{row['method']}"
               for row in operation_table.web_rows()
               if row["kind"] == "scene" and row["method"] not in readers]
    assert not missing, f"scene operations without an argument reader: {missing}"


def _literal_surface() -> dict[str, set[str]]:
    """Names still installed with a SetValue pair in the binding. Must be empty."""
    surface: dict[str, set[str]] = {}
    for namespace, name in re.findall(
            r'(\w+)->SetValue\("([^"]+)", CefV8Value::CreateFunction', _binding_source()):
        surface.setdefault(namespace, set()).add(name)
    return surface


def _installed_surface() -> dict[str, set[str]]:
    surface = _literal_surface()
    for namespace, names in EXPECTED_SURFACE.items():
        assert not (surface.get(namespace, set()) & names), (
            f"{namespace} installs {sorted(surface.get(namespace, set()) & names)} twice")
        surface.setdefault(namespace, set()).update(names)
    return surface


def test_window_corona_surface_is_exactly_the_named_contract():
    assert _installed_surface() == EXPECTED_SURFACE


def test_no_name_is_installed_outside_the_operation_table():
    # A SetValue("createPanel", CreateFunction(...)) pair is a name the table does not own:
    # it cannot be derived, renamed, or found by any other guard.
    literal = _literal_surface()
    assert not literal, f"names installed by hand instead of from the table: {literal}"


def test_the_namespaces_are_created_before_the_table_fills_them():
    source = _binding_source()
    for namespace in EXPECTED_SURFACE:
        assert re.search(r'\{"%s", CefV8Value::CreateObject' % namespace, source), (
            f"the {namespace} namespace is not created by the install loop")
    assert "kEditorOperations" in source, "the binding must install the shared table"


def test_the_binding_publishes_the_row_name_verbatim():
    # A loop that re-derived the spelling (camelCase, a prefix, anything) would publish a
    # name the table does not declare, and the SetValue scan cannot see it: there are no
    # literals left to compare. Pin the assignment and that the key and the function name are
    # the same value, so the two cannot drift apart.
    source = _binding_source()
    assert "const std::string js_name(spec.name);" in source, (
        "the install loop stopped publishing the row name")
    assert not re.search(r"camel_case\s*\(", source), (
        "the binding must not derive a spelling again")
    assert re.search(r"SetValue\(\s*js_name,\s*CefV8Value::CreateFunction\(\s*js_name,", source), (
        "the published key and the function name must be the one spelling")


def test_corona_is_the_only_global_installed():
    source = _binding_source()
    assert source.count("GetGlobal()->SetValue(\"corona\"") == 1


def test_web_to_python_bridge_stays_one_function_wide():
    assert _installed_surface()["bridge"] == {"call"}


def test_no_json_transport_survives():
    for path in (ROOT / "src").rglob("*"):
        if path.suffix not in {".cpp", ".h"} or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        for retired in ("cefQuery", "CefMessageRouter", "OnQuery"):
            assert retired not in text, f"{retired} came back in {path.relative_to(ROOT)}"


def test_frontend_reaches_native_only_through_the_facade():
    offenders = []
    for path in FRONTEND_SRC.rglob("*"):
        if path.suffix not in {".js", ".vue", ".mjs"} or not path.is_file():
            continue
        if path.name == "editorApi.js":
            continue
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        if "window.corona" in text:
            offenders.append(str(path.relative_to(ROOT)))
    assert not offenders, f"native calls outside the facade: {offenders}"


# The promise rejection vocabulary, split by who owns each code.
#
# The five infrastructure codes are this binding's own: it invents them, and the frontend
# switches on them. PROJECT_CONTEXT_CHANGED is the scene layer's symbolic code for "the
# request named an archive or scene the editor no longer owns"; the binding reuses it
# rather than flattening it, and operation replies carry their own symbolic codes through
# verbatim (see the comment above dispatch_async). The guard below therefore checks the
# codes this file *originates*, which is where a typo or a new invention would hide.
INFRASTRUCTURE_CODES = {
    "PERMISSION_DENIED",   # event binding attempted from another V8 context
    "CONTEXT_CHANGED",     # the page's context was replaced mid-call
    "INVALID_ARGUMENT",    # the argument reader refused the call
    "NATIVE_UNAVAILABLE",  # the editor owner thread did not take the post
    "NATIVE_FAILURE",      # the operation threw
}
SCENE_LAYER_CODES = {
    "PROJECT_CONTEXT_CHANGED",  # the scene layer's stale-archive answer, passed through
}
REJECTION_CODES = INFRASTRUCTURE_CODES | SCENE_LAYER_CODES


def _reported_codes() -> set[str]:
    source = _binding_source()
    codes = set(re.findall(r'reject\([^;]*?"([A-Z][A-Z_]+)"', source))
    codes |= set(re.findall(r'RejectPromise\(\s*"([A-Z][A-Z_]+)"', source))
    codes |= set(re.findall(r'error_code\s*=\s*"([A-Z][A-Z_]+)"', source))
    return codes


def test_promise_rejections_use_the_documented_vocabulary():
    reported = _reported_codes()
    assert reported, "no rejection codes found - did the async path change shape?"
    unknown = sorted(reported - REJECTION_CODES)
    assert not unknown, (
        f"undocumented rejection codes: {unknown}. The frontend switches on these; "
        "adding one is a contract change")


def _scene_layer_codes() -> set[str]:
    """Codes the scene layer originates outside this binding.

    Operation replies carry their own symbolic codes through verbatim, so a
    pass-through code never appears as a literal in the binding. Scanning the
    layer keeps the documented pass-through vocabulary honest without a second
    hand-maintained list.
    """
    codes = set()
    for path in (ROOT / "src/systems/ui/cef").glob("*.cpp"):
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        codes |= set(re.findall(r'fail\(\s*"([A-Z][A-Z_]+)"', text))
        codes |= set(re.findall(r'error_code\s*=\s*"([A-Z][A-Z_]+)"', text))
        codes |= set(re.findall(r'\{\s*"([A-Z][A-Z_]+)"\s*,\s*"', text))
    return codes


def test_every_documented_code_is_still_used():
    unused = sorted(INFRASTRUCTURE_CODES - _reported_codes())
    assert not unused, f"documented but no longer produced: {unused} - remove it from the contract"
    missing = sorted(SCENE_LAYER_CODES - _scene_layer_codes())
    assert not missing, f"documented but no layer produces it: {missing} - update the contract"

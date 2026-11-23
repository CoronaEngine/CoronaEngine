"""Rules that keep the typed Web surface honest, without pinning its size or its names."""
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

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


def test_every_scene_operation_has_an_argument_reader():
    readers = _reader_methods()
    duplicates = sorted({name for name in readers if readers.count(name) > 1})
    assert not duplicates, f"methods with more than one reader: {duplicates}"
    missing = [f"{row['group']}.{row['name']} -> SceneMethod::{row['method']}"
               for row in operation_table.web_rows()
               if row["kind"] == "scene" and row["method"] not in readers]
    assert not missing, f"scene operations without an argument reader: {missing}"


def test_web_to_python_bridge_stays_one_function_wide():
    assert _table_surface()["bridge"] == {"call"}, (
        "the Web -> Python bridge grew a second function")


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

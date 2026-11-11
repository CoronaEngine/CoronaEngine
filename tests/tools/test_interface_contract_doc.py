"""The interface document stays a map of the real contract.

docs/interface-contract.md is the single entry point for the interface layer. A map that
lists a name the code does not have - or misses one it does - is worse than no map, so the
names are checked against the same sources the build enforces.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
DOC = ROOT / "docs/interface-contract.md"
HEADER = ROOT / "src/systems/ui/cef/script_runtime_api.h"
SURFACE = ROOT / "tests/tools/test_typed_native_surface.py"


def _doc() -> str:
    return DOC.read_text(encoding="utf-8-sig")


def _operations_in_source() -> set[str]:
    text = HEADER.read_text(encoding="utf-8-sig")
    return set(re.findall(r'\{"(script_scene_\w+)",\s*"', text))


def _namespaces_in_source() -> set[str]:
    text = SURFACE.read_text(encoding="utf-8-sig")
    block = text[text.index("EXPECTED_SURFACE = {"):text.index("\n}\n", text.index("EXPECTED_SURFACE"))]
    return set(re.findall(r'^\s{4}"(\w+)":', block, re.M))


def test_the_document_exists_and_is_the_entry_point():
    assert DOC.exists(), "docs/interface-contract.md is the entry point for the interface layer"
    text = _doc()
    for topic in ("Web 面", "ScriptRuntime 面", "失败形状", "保护这份契约的守卫"):
        assert topic in text, f"the document no longer covers {topic!r}"


def test_every_script_operation_is_listed_once():
    text = _doc()
    listed = re.findall(r"^\| `(\w+)` \| `\[", text, re.M)
    expected = {name.replace("script_scene_", "") for name in _operations_in_source()}
    assert len(listed) == len(set(listed)), f"duplicated rows: {sorted(listed)}"
    missing = sorted(expected - set(listed))
    extra = sorted(set(listed) - expected)
    assert not missing and not extra, f"document and contract disagree: missing {missing}, extra {extra}"


def test_every_web_namespace_is_listed():
    text = _doc()
    listed = set(re.findall(r"^\| `(\w+)` \| ", text, re.M))
    expected = _namespaces_in_source()
    missing = sorted(expected - listed)
    assert not missing, f"the document does not describe these namespaces: {missing}"


def test_the_failure_shapes_match_what_the_guards_enforce():
    text = _doc()
    for code in ("PERMISSION_DENIED", "CONTEXT_CHANGED", "INVALID_ARGUMENT",
                 "NATIVE_UNAVAILABLE", "NATIVE_FAILURE", "PROJECT_CONTEXT_CHANGED"):
        assert code in text, f"the document stops documenting {code}"
    for number in ("409", "403", "404", "400"):
        assert number in text, f"the document stops documenting the {number} class"
    assert "data.code" in text, "the document has to say where the symbolic code lives"


def test_every_guard_it_names_exists():
    text = _doc()
    for target in re.findall(r"\]\(\.\./([\w./-]+)\)", text):
        assert (ROOT / target).exists(), f"the document points at a missing guard: {target}"

def _web_methods_in_source() -> dict[str, set[str]]:
    text = SURFACE.read_text(encoding="utf-8-sig")
    block = text[text.index("EXPECTED_SURFACE = {"):text.index("\n}\n", text.index("EXPECTED_SURFACE"))]
    entries = re.findall(r'^\s{4}"(\w+)": \{(.*?)\},?$', block, re.M | re.S)
    return {namespace: set(re.findall(r'"(\w+)"', body)) for namespace, body in entries}


def test_every_web_method_is_listed():
    """Namespaces alone are not an entry point: the reader wants the method names."""
    documented = _doc()
    missing = []
    for namespace, methods in _web_methods_in_source().items():
        for method in methods:
            if f"`{method}`" not in documented:
                missing.append(f"{namespace}.{method}")
    assert not missing, f"the document does not name these Web methods: {sorted(missing)}"

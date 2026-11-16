"""The interface document stays a map of the real contract.

docs/interface-contract.md is the single entry point for the interface layer. A map that
lists a name the code does not have - or misses one it does - is worse than no map, so the
names are checked against the same sources the build enforces.
"""
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import operation_table
import native_surface

ROOT = Path(__file__).resolve().parents[2]
DOC = ROOT / "docs/interface-contract.md"


def _doc() -> str:
    return DOC.read_text(encoding="utf-8-sig")


def _operations_in_source() -> set[str]:
    return {row["name"] for row in operation_table.script_rows()}


def _namespaces_in_source() -> set[str]:
    return set(native_surface.EXPECTED_SURFACE)


def test_the_document_exists_and_is_the_entry_point():
    assert DOC.exists(), "docs/interface-contract.md is the entry point for the interface layer"
    text = _doc()
    for topic in ("Web 面", "失败形状", "保护这份契约的守卫"):
        assert topic in text, f"the document no longer covers {topic!r}"


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

def test_every_guard_it_names_exists():
    text = _doc()
    for target in re.findall(r"\]\(\.\./([\w./-]+)\)", text):
        assert (ROOT / target).exists(), f"the document points at a missing guard: {target}"

def _web_methods_in_source() -> dict[str, set[str]]:
    return dict(native_surface.EXPECTED_SURFACE)


def test_every_web_method_is_listed():
    """Namespaces alone are not an entry point: the reader wants the method names."""
    documented = _doc()
    missing = []
    for namespace, methods in _web_methods_in_source().items():
        for method in methods:
            if f"`{method}`" not in documented:
                missing.append(f"{namespace}.{method}")
    assert not missing, f"the document does not name these Web methods: {sorted(missing)}"


def test_the_document_matches_the_generator():
    """The checked-in document must be what the generator produces right now."""
    import generate_interface_contract as generator

    expected = generator.build()
    actual = _doc()
    assert actual == expected, (
        "docs/interface-contract.md is stale; run "
        "`python tests/tools/generate_interface_contract.py`")

def test_every_guard_file_is_registered_in_the_table():
    """A guard that is not in the table is a guard nobody reading the doc knows about.

    The table is generated from ``GUARDS`` in the generator, so this compares the
    directory with that list: adding a test file means registering it (or moving it
    somewhere that is not a guard).
    """
    generator = (ROOT / "tests/tools/generate_interface_contract.py").read_text(encoding="utf-8")
    registered = set(re.findall(r'\("(test_\w+\.py)"', generator))
    present = {path.name for path in (ROOT / "tests/tools").glob("test_*.py")}
    assert present == registered, (
        f"guards missing from the contract table: {sorted(present - registered)}; "
        f"registered but absent: {sorted(registered - present)}")

def test_every_bridge_service_is_listed():
    """The allowlist is code; the document has to show it, or nobody knows what Web may reach."""
    text = _doc()
    editor = (ROOT / "embedded_python/runtime/editor_host.py").read_text(encoding="utf-8-sig")
    allowed = re.findall(r"ALLOWED_SERVICES = frozenset\(\{([^}]*)\}\)", editor)
    assert allowed, "ALLOWED_SERVICES moved or changed shape"
    for name in re.findall(r'"(\w+)"', allowed[0]):
        assert f"| `{name}` |" in text, f"bridge service {name} is not in the document"
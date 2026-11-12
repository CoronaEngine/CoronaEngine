"""Every rejection a Web caller can see is declared in one place.

`docs/interface-contract.md` used to name five infrastructure codes while the
interface layer emitted two dozen. That is why this guard exists: it scans the
sources that can reject a Promise, and fails when one of them invents a code
that `rejection_codes.py` does not declare. The generated contract prints the
same list, so the document cannot drift from the code again.
"""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import rejection_codes


def test_sources_only_emit_declared_codes():
    emitted = rejection_codes.emitted_codes()
    undeclared = sorted(set(emitted) - rejection_codes.ALL)
    assert not undeclared, (
        f"these rejection codes are emitted but not declared in rejection_codes.py: "
        f"{ {{code: sorted(emitted[code]) for code in undeclared}} }")


def test_every_declared_code_is_still_emitted():
    emitted = set(rejection_codes.emitted_codes())
    unused = sorted(rejection_codes.ALL - emitted)
    assert not unused, f"declared but no longer emitted: {unused} - remove them from the vocabulary"


def test_the_groups_do_not_overlap():
    groups = {
        "infrastructure": rejection_codes.INFRASTRUCTURE,
        "window": rejection_codes.WINDOW,
        "scene": rejection_codes.SCENE,
        "bridge": rejection_codes.BRIDGE,
    }
    seen: dict[str, str] = {}
    for name, codes in groups.items():
        for code in codes:
            assert code not in seen, f"{code} is declared in both {seen[code]} and {name}"
            seen[code] = name


def test_stable_codes_are_declared_and_documented():
    assert rejection_codes.STABLE, "the stable tier must not be empty"
    assert rejection_codes.STABLE <= rejection_codes.ALL
    assert rejection_codes.STABLE | rejection_codes.DETAIL == rejection_codes.ALL
    assert not rejection_codes.STABLE & rejection_codes.DETAIL
    document = (rejection_codes.ROOT / "docs" / "interface-contract.md").read_text(encoding="utf-8")
    for code in rejection_codes.STABLE:
        assert f"`{code}`" in document, f"the contract does not name the stable code {code}"


def test_the_frontend_only_branches_on_stable_codes():
    # Codes the frontend compares against must be stable: a detail code may be renamed
    # or merged, and a branch on it would silently stop matching. Codes outside the
    # native vocabulary (the AI service's own lowercase codes, KeyEvent codes) are not
    # this layer's contract and are ignored.
    import re

    frontend = rejection_codes.ROOT / "CoronaEditor" / "editor" / "Frontend" / "src"
    assert frontend.is_dir(), "the editor checkout is missing; run dev.py editor-sync"
    pattern = re.compile(r"\.code\s*===\s*['\"]([A-Z][A-Z0-9_]*)['\"]")
    branched = set()
    for path in list(frontend.rglob("*.js")) + list(frontend.rglob("*.vue")):
        for match in pattern.finditer(path.read_text(encoding="utf-8", errors="replace")):
            branched.add(match.group(1))
    unstable = sorted((branched & rejection_codes.ALL) - rejection_codes.STABLE)
    assert not unstable, f"the frontend branches on detail codes: {unstable}"


def test_infrastructure_codes_are_the_five_the_binding_owns():
    # These five are this binding's own inventions; the frontend switches on them.
    assert rejection_codes.INFRASTRUCTURE == {
        "PERMISSION_DENIED", "CONTEXT_CHANGED", "INVALID_ARGUMENT",
        "NATIVE_UNAVAILABLE", "NATIVE_FAILURE",
    }

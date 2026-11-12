"""Integration smokes must call the native surface that exists.

The scripts under ``tests/integration`` are manual: nothing in CI runs them, so
when the native surface moves a script can keep calling methods that were
deleted and nobody notices. This guard reads the same ``EXPECTED_SURFACE`` the
Web contract is pinned to and fails when a script calls anything else.

``RETIRED_PRE_BRIDGE`` is the temporary hold list for scripts that still drive
the surface from before the Web -> Python bridge migration. It is empty: every
script now goes through ``corona.bridge.call`` for the file, document and script
domains. The mechanism stays, so a script that goes stale again fails here, and
a script placed on the list must be removed once it is migrated.
"""

from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import test_typed_native_surface as surface

ROOT = Path(__file__).resolve().parents[2]
INTEGRATION = ROOT / "tests/integration"

# Strings that look like `corona.<ns>.<member>` but are storage keys or page
# objects, not native namespaces.
NON_RPC_ROOTS = {"bridge", "cabbageAssistantContext", "nativeViewportCursor", "viewportUiCalibration"}

RETIRED_PRE_BRIDGE: set[str] = set()

# A direct call (`corona.scene.readScene(`) and the string form the `v8()` helpers use
# (`v8("corona.scene.readScene", ...)`).
# `typeof corona.ns.method` followed by an assertion that it is a function.
TYPEOF_FUNCTION = re.compile(r"typeof corona\.(\w+)\??\.(\w+)[^;\n]{0,80}?'function'")

CALL_PATTERNS = (
    re.compile(r"corona\.(\w+)\.(\w+)\s*\("),
    re.compile(r"""["']corona\.(\w+)\.(\w+)["']"""),
)


def stale_calls() -> dict[str, set[str]]:
    stale: dict[str, set[str]] = {}
    for path in sorted(INTEGRATION.rglob("*")):
        if path.suffix not in {".py", ".mjs"}:
            continue
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        for pattern in CALL_PATTERNS:
            for match in pattern.finditer(text):
                namespace, method = match.group(1), match.group(2)
                if namespace in NON_RPC_ROOTS:
                    continue
                if method not in surface.EXPECTED_SURFACE.get(namespace, set()):
                    stale.setdefault(path.name, set()).add(f"{namespace}.{method}")
    return stale


def test_typeof_assertions_only_describe_live_methods():
    """`typeof corona.ns.method === 'function'` is a claim the method exists.

    The mirror claim (`=== 'undefined'`) is how a retired method stays retired,
    so only the positive form is checked. A stale positive assertion passes the
    call scan, because `typeof` is not a call.
    """
    stale: dict[str, set[str]] = {}
    for path in sorted(INTEGRATION.rglob("*.mjs")):
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        for match in TYPEOF_FUNCTION.finditer(text):
            namespace, method = match.group(1), match.group(2)
            if namespace in NON_RPC_ROOTS:
                continue
            if method not in surface.EXPECTED_SURFACE.get(namespace, set()):
                stale.setdefault(path.name, set()).add(f"{namespace}.{method}")
    assert not stale, (
        "these scripts assert a function that the surface does not have: "
        f"{ {{name: sorted(methods) for name, methods in stale.items()}} }")


def test_integration_scripts_only_call_the_live_surface():
    stale = stale_calls()
    unexpected = {
        name: sorted(methods) for name, methods in stale.items() if name not in RETIRED_PRE_BRIDGE
    }
    assert not unexpected, (
        "these integration scripts call a surface that no longer exists: "
        f"{unexpected}. Update them to the current binding or the bridge.")
    fixed = sorted(RETIRED_PRE_BRIDGE - set(stale))
    assert not fixed, (
        f"these scripts no longer call a retired surface, so remove them from RETIRED_PRE_BRIDGE: {fixed}")


def test_every_retired_script_has_a_reason_to_be_there():
    stale = stale_calls()
    unexplained = sorted(RETIRED_PRE_BRIDGE - set(stale))
    assert not unexplained, (
        f"RETIRED_PRE_BRIDGE lists scripts that are not stale any more: {unexplained}")

"""Integration smokes must call the native surface that exists.

The scripts under ``tests/integration`` are manual: nothing in CI runs them, so
when the native surface moves a script can keep calling methods that were
deleted and nobody notices. This guard reads the same ``EXPECTED_SURFACE`` the
Web contract is pinned to and fails when a script calls anything else.

``RETIRED_PRE_BRIDGE`` lists the scripts that still drive the surface from
before the Web -> Python bridge migration (they call ``corona.scripts.*``,
``corona.projectFiles.*``, ``corona.projectSettings.*`` and
``corona.files.*`` instead of going through ``corona.bridge.call``). The list
is pinned so the debt stays visible and can only shrink: migrate one script and
its entry must go, while a newly stale script fails the guard.
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

RETIRED_PRE_BRIDGE = {
    "blockly_actor_file_smoke.py",
    "python_archive_smoke.py",
    "scene_pull_smoke.py",
    "web_graph_target_smoke.py",
}

# A direct call (`corona.scene.readScene(`) and the string form the `v8()` helpers use
# (`v8("corona.scene.readScene", ...)`).
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

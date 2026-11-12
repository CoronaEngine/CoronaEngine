"""The frontend may only call facade methods that exist.

``CoronaEditor`` is a separate checkout pinned by ``build_tools/source_workspaces.py``.
``src/api/editorApi.js`` is the one module that knows the native surface; every
other module calls through it. A stale call site (a method renamed or removed on
this side) fails at runtime in a browser, which no test here would notice.

This guard loads the facade with node and scans ``src`` for
``editorApi.<namespace>.<method>`` calls. It runs only when the editor checkout
and node are both available; the checkout is a build input, not a repo file.
"""

import json
import shutil
from pathlib import Path
import re
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
EDITOR = ROOT / "CoronaEditor" / "editor" / "Frontend"
SOURCE = EDITOR / "src"
FACADE = SOURCE / "api" / "editorApi.js"

CALL = re.compile(r"editorApi\.(\w+)\.(\w+)\s*\(")


def _skip_reasons() -> list[str]:
    reasons = []
    if not FACADE.is_file():
        reasons.append(f"{FACADE} is missing (run `python build_tools/dev.py editor-sync`)")
    if shutil.which("node") is None:
        reasons.append("node is not on PATH")
    return reasons


def load_facade() -> dict[str, list[str]]:
    for reason in _skip_reasons():
        pytest.skip(reason)
    script = (
        "import('" + FACADE.as_uri() + "').then(m => {"
        "const out = {};"
        "for (const [ns, value] of Object.entries(m.editorApi || {})) out[ns] = Object.keys(value).sort();"
        "console.log(JSON.stringify(out));});"
    )
    result = subprocess.run(["node", "--input-type=module", "-e", script],
                            capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout.strip().splitlines()[-1])


def test_the_facade_loads_without_a_browser():
    facade = load_facade()
    assert {"window", "scene", "project", "bridge"} <= set(facade), sorted(facade)
    assert "close" in facade["window"] and "createPanel" in facade["window"]


def test_only_the_facade_reaches_the_native_object():
    """`window.corona` is the native surface; one module owns it.

    Every other module goes through `editorApi`, which is what makes the facade
    guard meaningful and what lets the surface move without a codebase-wide
    search. A second door would quietly reintroduce the coupling this layer
    spent the campaign removing.
    """
    for reason in _skip_reasons():
        pytest.skip(reason)
    facade_path = FACADE.resolve()
    offenders: dict[str, list[int]] = {}
    for path in sorted(SOURCE.rglob("*")):
        if path.suffix not in {".js", ".vue"} or not path.is_file():
            continue
        if path.resolve() == facade_path:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        lines = [number for number, line in enumerate(text.splitlines(), 1) if "window.corona" in line]
        if lines:
            offenders[path.relative_to(EDITOR).as_posix()] = lines
    assert not offenders, (
        "these modules touch the native object directly; go through editorApi instead: "
        f"{offenders}")


def test_no_module_calls_a_missing_facade_method():
    facade = load_facade()
    stale: dict[str, set[str]] = {}
    for path in sorted(SOURCE.rglob("*")):
        if path.suffix not in {".js", ".vue"} or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in CALL.finditer(text):
            namespace, method = match.group(1), match.group(2)
            if method in facade.get(namespace, []):
                continue
            stale.setdefault(path.relative_to(EDITOR).as_posix(), set()).add(f"{namespace}.{method}")
    assert not stale, (
        "these modules call a facade method that no longer exists: "
        f"{ {{path: sorted(methods) for path, methods in stale.items()}} }")

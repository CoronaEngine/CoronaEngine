"""Every window method either checks the caller or is a documented exception.

Permission rules used to live only in the case bodies of cef_window_operations.cpp, so a new
window method could be added with no check at all and nothing would notice. This guard scans
the switch and requires each method to carry a caller check (`main_workspace`, or the camera
lease) unless it is one of the exceptions below, which have to be justified here.

The exceptions:
- CreatePanel: any window may create a panel; the reply carries its identity.
- CreateCameraView: authorized by the camera lease (`authority`), not by the main window,
  because SceneTools runs in its own dock context.
- ConfigureViewport: the request can only address the caller's own surface.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "src" / "systems" / "ui" / "cef" / "cef_window_operations.h"
SOURCE = ROOT / "src" / "systems" / "ui" / "cef" / "cef_window_operations.cpp"

EXCEPTIONS = {"CreatePanel", "CreateCameraView", "ConfigureViewport"}


def window_methods():
    header = HEADER.read_text(encoding="utf-8")
    enum = re.search(r"enum class WindowMethod\s*\{([^}]*)\}", header, re.S)
    assert enum, "WindowMethod enum not found"
    return [name.strip() for name in enum.group(1).split(",") if name.strip()]


def case_blocks():
    source = SOURCE.read_text(encoding="utf-8")
    positions = []
    for method in window_methods():
        match = re.search(r"case WindowMethod::" + method + r"\s*:", source)
        assert match, f"WindowMethod::{method} has no case label"
        positions.append((match.start(), method))
    positions.sort()
    blocks = {}
    for index, (start, method) in enumerate(positions):
        end = positions[index + 1][0] if index + 1 < len(positions) else len(source)
        blocks[method] = source[start:end]
    return blocks


def test_every_window_method_has_a_case_label():
    assert set(case_blocks()) == set(window_methods())


def test_every_window_method_checks_its_caller_or_is_a_documented_exception():
    unguarded = []
    for method, block in case_blocks().items():
        if method in EXCEPTIONS:
            continue
        if "main_workspace" not in block and "authority" not in block:
            unguarded.append(method)
    assert not unguarded, (
        f"these window methods run without a caller check: {unguarded}; "
        f"add a permission branch or justify an exception in this guard")


def test_the_exception_list_is_the_documented_one():
    # Shrinking this set means a method gained a check; growing it needs a reason above.
    assert EXCEPTIONS == {"CreatePanel", "CreateCameraView", "ConfigureViewport"}

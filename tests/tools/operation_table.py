"""The Web operation table, parsed once.

Every guard used to carry its own copy of this parser. It now reads the one table:
`{group, name, method, summary}` rows, all of them published to the Web surface (the
ScriptRuntime projection was removed with the script runtime).

A row's method is qualified by the enum that owns it - `SceneMethod::ReadScene`,
`WindowMethod::CreatePanel`, `NativeEventKind::Closed` - because the surface spans eight
method enums plus the two window event subscriptions.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / "src/systems/ui/cef/editor_operation_table.h"

ROW = re.compile(r'\{"(\w+)",\s*"(\w+)",\s*(\w+)::(\w+),\s*"((?:[^"\\]|\\.)*)"\}')


def method_kind(enum: str) -> str:
    """`SceneMethod` -> `scene`, `NativeEventKind` -> `event`: the guard-facing name."""
    return "event" if enum == "NativeEventKind" else enum.removesuffix("Method").lower()


def rows():
    """Every row, in table order, as plain dicts."""
    text = TABLE.read_text(encoding="utf-8-sig")
    start = text.index("kEditorOperations[] = {")
    end = text.index("};", start)
    parsed = [
        {"group": group, "name": name, "enum": enum, "kind": method_kind(enum),
         "method": method, "summary": summary, "web": True, "script": False}
        for group, name, enum, method, summary in ROW.findall(text[start:end])
    ]
    assert parsed, "the operation table did not parse - did its format change?"
    return parsed


def web_rows():
    return rows()


def script_rows():
    return []


def script_keys():
    return []

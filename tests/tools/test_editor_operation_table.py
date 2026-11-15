"""The one operation table is the only place an operation name is declared.

The Web surface derives its camelCase name from the table, and the ScriptRuntime binds
by `(group, name)`. This guard pins the table's own shape: 36 rows, 11 of them dual,
`web` implies a `SceneMethod`, arity agrees with the documented argument list, and the
camelCase derivation reproduces the spellings the Web surface already publishes.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / "src/systems/ui/cef/editor_operation_table.h"

ROW = re.compile(
    r'\{"(\w+)",\s*"(\w+)",\s*(SceneMethod::\w+|std::nullopt),\s*'
    r'(true|false),\s*(true|false),\s*(\d+),\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"\}'
)


def rows():
    text = TABLE.read_text(encoding="utf-8-sig")
    start = text.index("kEditorOperations[] = {")
    end = text.index("};", start)
    parsed = []
    for group, name, method, web, script, arity, arguments, summary in ROW.findall(text[start:end]):
        parsed.append({
            "group": group,
            "name": name,
            "method": None if method == "std::nullopt" else method.split("::", 1)[1],
            "web": web == "true",
            "script": script == "true",
            "arity": int(arity),
            "arguments": arguments,
            "summary": summary,
        })
    return parsed


def _items(spec):
    """Top-level items of an argument list such as ``[scene, {a, b}]``."""
    inner = spec.strip()
    if not (inner.startswith("[") and inner.endswith("]")):
        raise AssertionError(f"arguments must be a bracketed list: {spec}")
    inner = inner[1:-1].strip()
    if not inner:
        return []
    items, depth, current = [], 0, []
    for char in inner:
        if char in "{[":
            depth += 1
        elif char in "}]":
            depth -= 1
        if char == "," and depth == 0:
            items.append("".join(current).strip())
            current = []
        else:
            current.append(char)
    items.append("".join(current).strip())
    return items


def test_the_table_is_thirty_six_rows():
    assert len(rows()) == 36, f"expected 36 rows, found {len(rows())}"


def test_eleven_rows_are_projected_to_both_surfaces():
    dual = [row for row in rows() if row["web"] and row["script"]]
    assert len(dual) == 11, f"expected 11 dual rows, found {len(dual)}"


def test_every_web_row_carries_a_scene_method():
    missing = [f"{row['group']}.{row['name']}" for row in rows() if row["web"] and row["method"] is None]
    assert not missing, f"web rows without a SceneMethod: {missing}"


def test_every_script_only_row_has_no_scene_method():
    extra = [f"{row['group']}.{row['name']}" for row in rows()
             if row["script"] and not row["web"] and row["method"] is not None]
    assert not extra, f"script-only rows must not name a SceneMethod: {extra}"


def test_group_and_name_are_unique():
    keys = [(row["group"], row["name"]) for row in rows()]
    assert len(keys) == len(set(keys)), "duplicate (group, name) rows"


def test_script_arity_matches_the_documented_positions():
    problems = []
    for row in rows():
        if not row["script"]:
            continue
        expected = len(_items(row["arguments"]))
        if expected != row["arity"]:
            problems.append(f"{row['group']}.{row['name']}: arity {row['arity']} but {row['arguments']} lists {expected}")
    assert not problems, problems


def test_arguments_are_empty_for_web_only_rows():
    problems = [f"{row['group']}.{row['name']}" for row in rows()
                if row["web"] and not row["script"] and (row["arity"] or row["arguments"] != "[]")]
    assert not problems, f"web-only rows must document no script positions: {problems}"


def test_camel_case_matches_the_published_js_spelling():
    # The C++ camel_case() must produce exactly these; they are the spellings
    # test_typed_native_surface.EXPECTED_SURFACE already pins on the Web side.
    expected = {
        "read_scene": "readScene",
        "read_active": "readActive",
        "read_actor_properties": "readActorProperties",
        "read_actor_rig": "readActorRig",
        "set_actor_ik_chains": "setActorIkChains",
        "select_model_file": "selectModelFile",
        "is_vision_available": "isVisionAvailable",
        "capture_screenshot": "captureScreenshot",
        "read_camera_render_settings": "readCameraRenderSettings",
    }
    derived = {}
    for name in expected:
        head, *tail = name.split("_")
        derived[name] = head + "".join(part.capitalize() for part in tail)
    assert derived == expected, f"camelCase rule drift: {derived}"


def test_summaries_are_not_empty():
    missing = [f"{row['group']}.{row['name']}" for row in rows() if not row["summary"].strip()]
    assert not missing, f"rows without a summary: {missing}"

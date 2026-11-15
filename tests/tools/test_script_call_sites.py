"""Every script call site must name an operation the table publishes.

`block_program.py` is the engine-owned runtime library the generated Blockly programs use;
`file_api.py` is the script host's own helper. Both reach the editor through
`api.editor_api`, whose namespaces are the C++ ones. This guard keeps them honest: a call
site that names a method the table does not publish is a typo that would only surface at run
time, inside a user's project.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / "src/systems/ui/cef/editor_operation_table.h"
CALL_SITES = [
    ROOT / "embedded_python/script_runtime/file_api.py",
    ROOT / "embedded_python/frontend_assets/block_program.py",
    ROOT / "embedded_python/api/editor_api.py",
]

TABLE_ROW = re.compile(
    r'\{"(\w+)",\s*"(\w+)",\s*(SceneMethod::\w+|std::nullopt),\s*'
    r'(true|false),\s*(true|false),\s*(\d+),'
)
CALL = re.compile(r"\.(scene|audio|viewport)\.(\w+)\s*\(")
# `scene_api = get_script_runtime_editor_api().scene` then `scene_api.read_active()`.
ASSIGN = re.compile(
    r"(\w+)\s*=\s*(?:get_script_runtime_editor_api\(\)|editor_api\(\))\.(scene|audio|viewport)")
CALL_ON_VAR = re.compile(r"\b(\w+)\.(\w+)\s*\(")
# Only the contract's verb prefixes are checked: an unrelated `.get(...)` on a proxy that
# happens to be called `scene` is not a call site.
VERB = re.compile(r"^(?:read_|set_|update_|create_|delete_|pause_|resume_|play$|stop$)")
LEGACY = ("scene_tools", "script_scene_", "_OPERATIONS", "invoke_native_scene",
          "get_snapshot(", "read_archive(", "read_active_scene", "get_environment(",
          "play_actor_audio", "stop_actor_audio", "set_actor_state", "remove_actor(")


def script_operations():
    text = TABLE.read_text(encoding="utf-8-sig")
    start = text.index("kEditorOperations[] = {")
    end = text.index("};", start)
    return {
        (group, name)
        for group, name, _method, _web, script, _arity in TABLE_ROW.findall(text[start:end])
        if script == "true"
    }


def test_every_call_site_names_a_published_operation():
    published = script_operations()
    unknown = []
    for path in CALL_SITES:
        text = path.read_text(encoding="utf-8-sig")
        namespaces = dict(ASSIGN.findall(text))
        found = {(group, method) for group, method in CALL.findall(text) if VERB.match(method)}
        found |= {(namespaces[var], method)
                  for var, method in CALL_ON_VAR.findall(text)
                  if var in namespaces and VERB.match(method)}
        for group, method in sorted(found):
            if (group, method) not in published:
                unknown.append(f"{path.relative_to(ROOT)}: {group}.{method}")
    assert not unknown, f"call sites name operations the table does not publish: {unknown}"


def test_no_retired_name_survives_in_a_call_site():
    offenders = []
    for path in CALL_SITES:
        text = path.read_text(encoding="utf-8-sig")
        for name in LEGACY:
            if name in text:
                offenders.append(f"{path.relative_to(ROOT)}: {name}")
    assert not offenders, f"retired names came back: {offenders}"

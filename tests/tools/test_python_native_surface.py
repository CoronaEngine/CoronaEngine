"""Rules about the Python face: what it must not expose, and what it must keep installing.

The exact set of bound names used to be compared with a hand-written list here. That is gone:
the comparison made every binding addition a two-file edit, and when it failed it only said
"the sets differ" - it could not tell an added function from a removed one.

What remains are the rules a refactor can violate in silence:

- no submodule comes back and no deleted name is bound again;
- no binding source names the editor scene layer (Python owns the archive, C++ owns the scene);
- a type and a function never share a name;
- the module installs its two groups, in order;
- the loader recognises the module by a name the module really binds.
"""
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import python_surface

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
MODULE = ROOT / "src/systems/script/python/corona_engine_module.cpp"
LOADER = "embedded_python/runtime/native_engine.py"

# A file binds into the module if it declares a module-level `m.def("name"`, a
# `nb::class_<T>(<owner>, "Name")` or a submodule. `NB_MODULE(CoronaEngine, m)` is the only
# module in the tree.
BINDS = re.compile(r'^\s*m\.def\(\s*"\w+"|class_<[^>]*>\(\s*\w+\s*,\s*"\w+"|def_submodule\(',
                   re.M)
SUBMODULE = re.compile(r'(\w+)\s*=\s*m\.def_submodule\(\s*"(\w+)"', re.S)
CLASS = re.compile(r'class_<[^>]*>\(\s*(\w+)\s*,\s*"(\w+)"')
MODULE_FN = re.compile(r'^\s*m\.def\(\s*"(\w+)"')
MEMBER = re.compile(r'^\s*\.def(?:_static|_ro|_rw)?\(')
GROUP_CALL = re.compile(r"EngineScripts::(\w+)\s*\(\s*m\s*\)")

# The only doors to editor scene semantics: the scene-operation entry points, their request
# and method types, and the headers that declare them. A Python binding that names one of
# these could read or mutate the editor's live scene, which is not the Python host's job -
# it owns the project archive on disk, and C++ owns the scene. `native_editor_state` is not
# on this list on purpose: `cef_py_bind.cpp` reads the project root and session identity from
# it, which is host support rather than semantics.
EDITOR_SEMANTICS = (
    "cef_scene_operations.h",
    "editor_scene_access.h",
    "editor_operation_table.h",
    "execute_scene_operation",
    "execute_window_operation",
    "SceneRequest",
    "SceneMethod",
    "SceneReply",
    "WindowRequest",
    "WindowMethod",
)


def _discover_binding_sources() -> list[str]:
    found = []
    for path in sorted(SRC.rglob("*")):
        if path.suffix not in {".cpp", ".h", ".hpp"} or not path.is_file():
            continue
        if BINDS.search(path.read_text(encoding="utf-8-sig", errors="replace")):
            found.append(path.relative_to(ROOT).as_posix())
    return found


def _surface() -> tuple[dict[str, int], list[str], dict[str, str]]:
    """(class -> member count, functions, submodule vars) as the sources declare them."""
    submodule_vars: dict[str, str] = {}
    classes: dict[str, int] = {}
    functions: list[str] = []
    current: str | None = None
    for relative in _discover_binding_sources():
        text = (ROOT / relative).read_text(encoding="utf-8-sig")
        # Submodules are matched against the whole file: the name may sit on the same line as
        # `def_submodule(` or on the next one, and a reformat must not hide the declaration.
        for match in SUBMODULE.finditer(text):
            submodule_vars[match.group(1)] = match.group(2)
        for line in text.splitlines():
            match = CLASS.search(line)
            if match:
                current = match.group(2)
                classes[current] = 0
                continue
            module = MODULE_FN.match(line)
            if module:
                current = None
                functions.append(module.group(1))
                continue
            if MEMBER.match(line) and current is not None:
                classes[current] += 1
    return classes, functions, submodule_vars


def test_the_binding_is_still_discoverable():
    # A moved or reformatted binding would make the rules below pass over nothing.
    _, functions, _ = _surface()
    assert functions, "no module-level `m.def` found - did the binding change shape?"


def test_the_object_model_is_gone():
    # There is no `.script` submodule and no script class on the module.
    _, _, submodules = _surface()
    assert not submodules, (
        f"a submodule came back: {sorted(submodules.values())}. The object model was deleted "
        "on purpose - deleting it again starts with this guard")


def test_removed_names_are_not_bound_anywhere():
    text = "\n".join(
        (ROOT / relative).read_text(encoding="utf-8-sig")
        for relative in _discover_binding_sources()
    )
    text += "\n" + MODULE.read_text(encoding="utf-8-sig")
    returned = sorted(name for name in python_surface.ABSENT_MODULE_NAMES
                      if re.search(rf'"{re.escape(name)}"|{re.escape(name)}\s*\(', text))
    assert not returned, (
        f"names the deletion removed are back in the module: {returned}")


def test_the_python_face_cannot_reach_editor_semantics():
    """Python owns the project archive on disk; C++ owns the live editor scene.

    Nothing on the Python side may name the scene-operation layer - not the executor, not its
    request/method types, not the headers - because that is the only way to read or mutate the
    editor's scene from Python. Host support may read project *identity* (`active_project_path`,
    `active_project_session`), which is why `native_editor_state` is not on the deny list.
    """
    offenders = {}
    for relative in _discover_binding_sources():
        text = (ROOT / relative).read_text(encoding="utf-8-sig")
        hit = sorted(name for name in EDITOR_SEMANTICS if name in text)
        if hit:
            offenders[relative] = hit
    assert not offenders, (
        f"the Python face reaches editor scene semantics: {offenders}. Editor semantics are "
        "Web-only (typed V8); Python services own the archive, not the scene")


def test_the_groups_do_not_overlap():
    classes, functions, _ = _surface()
    overlap = sorted(set(classes) & set(functions))
    assert not overlap, (
        f"these names are both a type and a function on the module: {overlap}. The groups are "
        "meant to be distinguishable by their caller")


def test_every_bound_name_is_a_snake_case_identifier():
    _, functions, _ = _surface()
    bad = sorted(name for name in functions
                 if not re.fullmatch(r"[a-z][a-z0-9]*(?:_[a-z0-9]+)*", name))
    assert not bad, f"bound names that are not snake_case: {bad}"


def test_the_loader_recognises_the_module_by_a_name_the_module_binds():
    """The Python loader must not depend on a name a refactor can delete.

    `runtime/native_engine.py` decides whether `import CoronaEngine` yielded the engine by
    looking for marker attributes. It used to look for `Scene`, which the object-model removal
    deleted: recognition then returned None for every optional binding, and the editor sat on
    "archive service initialising" with nothing raising. At least one marker has to name
    something the module really binds; `Scene` may stay as a legacy marker only.
    """
    source = (ROOT / LOADER).read_text(encoding="utf-8")
    markers = set(re.findall(r"hasattr\(mod, '(\w+)'\)", source))
    declared = re.search(r"LOADER_MARKERS\s*=\s*\(([^)]*)\)", source)
    assert declared, "the loader no longer declares its markers as LOADER_MARKERS"
    markers |= set(re.findall(r"'(\w+)'", declared.group(1)))
    _, functions, _ = _surface()
    bound = markers & set(functions)
    assert bound, (
        f"the loader recognises the module by {sorted(markers)}, none of which the module binds "
        f"({sorted(functions)}); a deleted marker silently disables every optional binding")


def test_the_module_installs_the_two_groups_in_order():
    calls = GROUP_CALL.findall(MODULE.read_text(encoding="utf-8-sig"))
    assert calls == list(python_surface.BINDING_GROUPS), (
        f"the module installs {calls}; expected {list(python_surface.BINDING_GROUPS)} - host "
        "support first, then the CEF transport")

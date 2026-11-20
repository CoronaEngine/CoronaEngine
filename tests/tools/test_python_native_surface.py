"""`CoronaEngine` publishes two disjoint groups; this pins both - and pins the deleted third.

The Web face has been pinned since the manifest retirement (`native_surface.py` plus
`test_typed_native_surface.py`), but the Python face had no test at all: an `m.def` could
appear, a class could be renamed, and nothing failed. That asymmetry is what made a dead
binding (`camera_follow_set_input_enabled`, no caller anywhere) and a whole unimported object
model invisible.

What is pinned here:

1. Which translation units bind into the module - discovered from `src/` and compared against a
   fixed list, so a *new* file cannot slip a name in unannounced.
2. The top-level surface: seven host-support functions plus the payload types they hand out.
3. The absence of the script object model: no submodule, no classes beyond the payload, and none
   of the names in `python_surface.ABSENT_MODULE_NAMES` bound anywhere.
4. The module installs those two groups, in that order.
"""
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import python_surface

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
MODULE = ROOT / "src/systems/script/python/corona_engine_module.cpp"

# The translation units that add names to the CoronaEngine module. `cef_py_bind.cpp` lives in
# the UI system but binds into the script module, which is why the discovery cannot be limited
# to one system's sources.
BINDING_SOURCES = (
    "src/systems/script/python/host_support_bindings.cpp",
    "src/systems/ui/cef/cef_py_bind.cpp",
)

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


def test_the_binding_sources_are_the_pinned_ones():
    discovered = _discover_binding_sources()
    missing = sorted(set(BINDING_SOURCES) - set(discovered))
    extra = sorted(set(discovered) - set(BINDING_SOURCES))
    assert not missing, f"a pinned binding source stopped binding: {missing}"
    assert not extra, (
        f"a translation unit binds into the module without being declared: {extra}. "
        "Add it to BINDING_SOURCES, or keep the new group out of CoronaEngine")


def test_the_surface_is_not_empty():
    # A moved or reformatted binding would make every assertion below pass over nothing.
    classes, functions, _ = _surface()
    assert classes, "no `nb::class_` declaration found - did the binding change shape?"
    assert functions, "no module-level `m.def` found - did the binding change shape?"


def test_the_host_support_functions_are_exactly_the_pinned_set():
    _, functions, _ = _surface()
    actual = set(functions)
    missing = sorted(python_surface.HOST_SUPPORT_FUNCTIONS - actual)
    extra = sorted(actual - python_surface.HOST_SUPPORT_FUNCTIONS)
    assert not missing, f"declared host-support functions that are no longer bound: {missing}"
    assert not extra, (
        f"functions bound without a decision in python_surface.HOST_SUPPORT_FUNCTIONS: {extra}. "
        "The Python face is a contract too: add the name there (and document who calls it) "
        "or do not bind it")
    assert len(functions) == len(actual), f"a function is bound twice: {sorted(functions)}"


def test_the_whole_type_surface_is_exactly_the_pinned_payloads():
    classes, _, _ = _surface()
    assert set(classes) == set(python_surface.HOST_SUPPORT_TYPES), (
        f"top-level types changed: {sorted(classes)} vs "
        f"{sorted(python_surface.HOST_SUPPORT_TYPES)} - a payload type belongs with the function "
        "that hands it out, and nothing else belongs on the module")
    for name, expected in python_surface.HOST_SUPPORT_TYPES.items():
        assert classes[name] == expected, (
            f"{name} publishes {classes[name]} members (pinned: {expected})")


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


def test_every_host_support_name_is_a_snake_case_identifier():
    bad = sorted(name for name in python_surface.HOST_SUPPORT_FUNCTIONS
                 if not re.fullmatch(r"[a-z][a-z0-9]*(?:_[a-z0-9]+)*", name))
    assert not bad, f"host-support names that are not snake_case: {bad}"


def test_the_module_installs_the_two_groups_in_order():
    calls = GROUP_CALL.findall(MODULE.read_text(encoding="utf-8-sig"))
    assert calls == list(python_surface.BINDING_GROUPS), (
        f"the module installs {calls}; expected {list(python_surface.BINDING_GROUPS)} - host "
        "support, then the editor transport")

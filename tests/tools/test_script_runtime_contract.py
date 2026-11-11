"""The ScriptRuntime wire contract, checked as data on all three sides.

`script_runtime_api.h` carries the contract as a table (`kScriptOperations`), the bindings
in `cef_py_bind.cpp` register those names, and `native_scene_adapter.py` builds the argument
arrays. Before this guard the only thing tying them together was that a typo failed at
runtime, in the editor, on the machine of whoever ran the block.

These tests fail the build when a name, an order or an argument count drifts.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "src/systems/ui/cef/script_runtime_api.h"
BINDINGS = ROOT / "src/systems/ui/cef/cef_py_bind.cpp"
SERVICE = ROOT / "src/systems/ui/cef/editor_scene_service.cpp"
ADAPTER = ROOT / "embedded_python/script_runtime/native_scene_adapter.py"


def _split_top_level(text):
    """Split a bracket body on commas that are not inside braces, brackets or strings."""
    parts, depth, current, quote = [], 0, [], None
    for ch in text:
        if quote:
            current.append(ch)
            if ch == quote:
                quote = None
            continue
        if ch in "\"'":
            quote = ch
            current.append(ch)
        elif ch in "[{(":
            depth += 1
            current.append(ch)
        elif ch in "]})":
            depth -= 1
            current.append(ch)
        elif ch == "," and depth == 0:
            parts.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    tail = "".join(current).strip()
    if tail:
        parts.append(tail)
    return parts


def cpp_table():
    text = HEADER.read_text(encoding="utf-8-sig")
    rows = re.findall(r'\{"(script_scene_\w+)",\s*"([^"]*)",', text)
    assert rows, "the contract table did not parse - did the format change?"
    table = {}
    for name, arguments in rows:
        body = arguments.strip()
        assert body.startswith("[") and body.endswith("]"), f"{name}: arguments must be an array"
        inner = body[1:-1].strip()
        table[name] = (0 if not inner else len(_split_top_level(inner)), arguments)
    return table


def test_the_table_covers_every_declared_operation():
    declared = set(re.findall(r"NativeResult (script_scene_\w+)\(",
                              HEADER.read_text(encoding="utf-8-sig")))
    table = set(cpp_table())
    assert declared == table, (
        f"declared but not described: {sorted(declared - table)}; "
        f"described but not declared: {sorted(table - declared)}")


def test_the_table_covers_every_binding():
    bound = set(re.findall(r'bind_script_operation\("(script_scene_\w+)"',
                           BINDINGS.read_text(encoding="utf-8-sig")))
    assert bound == set(cpp_table()), (
        f"bound but not described: {sorted(bound - set(cpp_table()))}; "
        f"described but not bound: {sorted(set(cpp_table()) - bound)}")


def test_python_passes_the_documented_argument_count():
    text = ADAPTER.read_text(encoding="utf-8-sig")
    mapping = dict(re.findall(r'"(\w+)":\s*"(script_scene_\w+)"', text))
    assert len(mapping) == 16, f"expected sixteen operation aliases, found {len(mapping)}"

    calls = re.findall(r'invoke_native_scene\(\s*"(\w+)",\s*\[(.*?)\]\s*\)', text, re.S)
    assert calls, "no invoke_native_scene call sites found - did the adapter change?"
    table = cpp_table()
    problems = []
    for alias, body in calls:
        binding = mapping.get(alias)
        if binding is None:
            problems.append(f"unknown operation alias {alias!r}")
            continue
        expected, arguments = table[binding]
        actual = 0 if not body.strip() else len(_split_top_level(body))
        if actual != expected:
            problems.append(
                f"{alias} -> {binding} passes {actual} arguments, contract says {expected} {arguments}")
    assert not problems, "\n".join(problems)


def test_every_alias_is_used_by_a_call_site():
    text = ADAPTER.read_text(encoding="utf-8-sig")
    mapping = dict(re.findall(r'"(\w+)":\s*"(script_scene_\w+)"', text))
    called = {alias for alias, _ in
              re.findall(r'invoke_native_scene\(\s*"(\w+)",\s*\[(.*?)\]\s*\)', text, re.S)}
    unused = sorted(set(mapping) - called)
    assert not unused, f"aliases with no call site: {unused}"


def _function_body(source: str, name: str):
    """The brace-balanced body of `name`, or None when the file does not define it."""
    match = re.search(r"^[^\n]*\b" + name + r"\([^;{]*\)\s*\{", source, re.M)
    if not match:
        return None
    start = source.index("{", match.start())
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    return None


def test_no_binding_reads_past_the_documented_arguments():
    """The body must not read a position the contract does not list.

    Bodies either read `request.args` themselves or hand the request to a helper in the same
    file, so one level of delegation is followed. Anything deeper is not a binding detail and
    the test stays out of it.
    """
    source = SERVICE.read_text(encoding="utf-8-sig")
    table = cpp_table()
    problems = []
    for name, (arity, arguments) in table.items():
        body = _function_body(source, name)
        if body is None:
            problems.append(f"{name}: no definition found in {SERVICE.name}")
            continue
        indices = [int(i) for i in re.findall(r"arg_\w+\(request\.args,\s*(\d+)", body)]
        for callee in set(re.findall(r"\b(\w+)\(\s*request\b", body)):
            delegated = _function_body(source, callee)
            if delegated:
                indices += [int(i) for i in re.findall(r"arg_\w+\(request\.args,\s*(\d+)", delegated)]
        if not indices:
            if arity != 0:
                problems.append(f"{name}: contract says {arity} argument(s) {arguments}, "
                                f"but the body reads none")
            continue
        highest = max(indices) + 1
        if highest > arity:
            problems.append(f"{name}: reads argument {highest}, contract documents {arity} {arguments}")
    assert not problems, "\n".join(problems)

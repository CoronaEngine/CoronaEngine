"""The host loop's three pull entries must exist on either side of the boundary."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
BINDINGS = [
    ROOT / "src/systems/script/python/host_support_bindings.cpp",
    ROOT / "src/systems/script/python/service_queue_bindings.cpp",
]


def test_the_loop_entries_are_bound():
    text = "\n".join(path.read_text(encoding="utf-8-sig") for path in BINDINGS if path.exists())
    for name in ("should_stop", "poll_service_requests", "complete_service_request"):
        assert re.search(rf'm\.def\(\s*"{name}"', text), f"{name} is not bound"


API = ROOT / "src/systems/script/python/python_api.cpp"
GETATTR = re.compile(r'getattr\((?:\w+|\w+\.\w+),\s*"(\w+)"\)')
INITIALIZE_INTERPRETER = "bool PythonAPI::initializeInterpreter()"


def _function_span(source, signature):
    """Return the [start, end) span of a function definition, brace-matched."""
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for offset in range(brace, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return start, offset + 1
    raise AssertionError(f"unterminated function definition: {signature}")


def test_the_only_name_lookups_are_the_two_entries():
    source = API.read_text(encoding="utf-8-sig")
    start, end = _function_span(source, INITIALIZE_INTERPRETER)
    matches = list(GETATTR.finditer(source))
    names = [match.group(1) for match in matches]
    assert sorted(names) == ["loop", "run"], (
        f"python_api.cpp looks up {sorted(names)}; only the two entry names may be resolved by "
        "string, and every other hook is now called from inside Python")
    outside = [match.group(1) for match in matches if not start <= match.start() < end]
    assert not outside, (
        f"python_api.cpp resolves {outside} outside initializeInterpreter(); the two entry "
        "names may only be resolved there")


def test_python_api_never_probes_hooks_with_hasattr():
    source = API.read_text(encoding="utf-8-sig")
    assert "hasattr" not in source, (
        "python_api.cpp probes a hook with hasattr; a missing hook must fail loudly, "
        "not be skipped as if it were optional")


def test_bootstrap_exports_both_entries():
    source = (ROOT / "embedded_python/runtime/bootstrap.py").read_text(encoding="utf-8-sig")
    for name in ("def run(", "def loop("):
        assert name in source, f"bootstrap.py no longer defines {name}"
    module = (ROOT / "embedded_python/main.py").read_text(encoding="utf-8-sig")
    assert "loop" in module, "main.py does not re-export loop"

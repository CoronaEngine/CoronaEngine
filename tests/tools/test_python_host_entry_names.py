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


def test_the_only_name_lookups_are_the_two_entries():
    names = GETATTR.findall(API.read_text(encoding="utf-8-sig"))
    assert sorted(names) == ["loop", "run"], (
        f"python_api.cpp looks up {sorted(names)}; only the two entry names may be resolved by "
        "string, and every other hook is now called from inside Python")


def test_bootstrap_exports_both_entries():
    source = (ROOT / "embedded_python/runtime/bootstrap.py").read_text(encoding="utf-8-sig")
    for name in ("def run(", "def loop("):
        assert name in source, f"bootstrap.py no longer defines {name}"
    module = (ROOT / "embedded_python/main.py").read_text(encoding="utf-8-sig")
    assert "loop" in module, "main.py does not re-export loop"

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

"""The retired coronaBridge has no production owner or view-layer exceptions."""

import re
from pathlib import Path


FRONTEND_ROOT = Path(__file__).resolve().parents[2]
SRC_ROOT = FRONTEND_ROOT / "src"

STRING_LITERAL_RE = re.compile(
    r"'(?:\\.|[^'\\])*'|\"(?:\\.|[^\"\\])*\"|`(?:\\.|[^`\\])*`",
    re.DOTALL,
)


def _source_without_strings(path):
    return STRING_LITERAL_RE.sub("''", path.read_text(encoding="utf-8"))


def _bridge_consumers():
    consumers = set()
    for path in SRC_ROOT.rglob("*"):
        if not path.is_file() or path.suffix not in {".js", ".mjs", ".vue"}:
            continue
        if "coronaBridge" in _source_without_strings(path):
            consumers.add(path.relative_to(SRC_ROOT).as_posix())
    return consumers


def test_corona_bridge_has_no_production_consumers():
    consumers = _bridge_consumers()
    assert consumers == set(), f"retired coronaBridge consumers: {sorted(consumers)}"

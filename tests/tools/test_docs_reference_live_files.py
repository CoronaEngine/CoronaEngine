"""Documents must not point at source files that no longer exist.

A stale path is worse than no path: it sends the reader to a file that vanished, and the
split that removed it is exactly the change they were trying to understand. Documents that
describe an older tree on purpose opt out with a HISTORICAL marker and say where the code
lives now.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
REF = re.compile(r"(?:src|include|tests)/[\w./-]+\.(?:cpp|h|hpp|py)")
HISTORICAL = re.compile(r"HISTORICAL-DOC", re.IGNORECASE)


def _documents():
    for pattern in ("docs/*.md", "docs/*.html", "*.md", "embedded_python/**/*.md"):
        for path in ROOT.glob(pattern):
            if path.is_file():
                yield path


def test_no_document_points_at_a_deleted_file():
    problems = []
    for path in _documents():
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        stale = sorted(r for r in set(REF.findall(text)) if not (ROOT / r).exists())
        if not stale:
            continue
        relative = path.relative_to(ROOT).as_posix()
        if HISTORICAL.search(text):
            continue
        problems.append(f"{relative}: {', '.join(stale[:3])}")
    assert not problems, (
        "documents reference files that do not exist; update the path or mark the document "
        "HISTORICAL-DOC with where the code lives now:\n" + "\n".join(problems))

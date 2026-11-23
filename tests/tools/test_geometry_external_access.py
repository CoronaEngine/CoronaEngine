"""Only the geometry owner touches GeometryStorage slots.

Scene state reaches the editor through GeometrySceneSnapshot, and writes go through
GeometryCommandChannel; a cross-thread `try_acquire_write` on a geometry slot would put two
writers on data the DAG owns. The single allowed read fetches the AnimationPlayer control
object: every one of its methods (set_sources / set_paused / request_switch / snapshot) takes
the player's own mutex, so it is a cross-thread-safe control surface, not scene state. A
snapshot copy would duplicate its state and add a tick of latency without removing a lock.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UI = ROOT / "src" / "systems" / "ui"

# file name -> why this read is allowed
ALLOWED_READS = {
    "editor_actor_projection.cpp": "AnimationPlayer 是活控制对象，不是场景状态",
}

WRITE = re.compile(r"geometry_storage\(\)[\s\S]{0,120}?try_acquire_write")
READ = re.compile(r"geometry_storage\(\)[\s\S]{0,120}?try_acquire_read")


def sources():
    return sorted(UI.rglob("*.cpp"))


def test_the_ui_layer_never_writes_a_geometry_slot():
    offenders = [path.name for path in sources() if WRITE.search(path.read_text(encoding="utf-8"))]
    assert not offenders, (
        f"these UI files write GeometryStorage from outside the DAG: {offenders}; "
        f"enqueue a GeometryCommand instead")


def test_geometry_reads_are_the_documented_exception_only():
    readers = {path.name for path in sources() if READ.search(path.read_text(encoding="utf-8"))}
    assert readers == set(ALLOWED_READS), (
        f"unexpected GeometryStorage readers: {sorted(readers - set(ALLOWED_READS))}; "
        f"read GeometrySceneSnapshot instead, or justify an exception here")


def test_the_exception_is_still_real():
    # Shrink this set when the player gains a snapshot-based control surface; growing it
    # needs a reason above.
    assert set(ALLOWED_READS) == {"editor_actor_projection.cpp"}

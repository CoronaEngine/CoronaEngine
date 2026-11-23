"""The ECS registry has exactly one writer: the systems DAG thread.

`include/corona/ecs/world.h` has no synchronization at all — that is the design
premise, not an omission (`entt::registry` must be touched by a single thread).
So only the owner (the `SystemsThread` that drives the DAG) may create/destroy
entities, iterate `view<>` or fetch components. Every other thread — the CEF
editor UI, scripts, the network — has to go through a command channel:
structural changes via `corona/ecs/structural_commands.h` (enqueue + synchronous
settle at the tick boundary), value writes via the existing per-system queues.

This guard is a **ratchet**. Task 1a landed it on the first pilot call site
(`create_geometry`) instead of waiting for the migration to finish:

* a non-owner translation unit that starts touching the registry fails;
* the allowlist below names every remaining direct touch, why it is tolerated and
  which task removes it; counts are exact, so migrating one *forces* the
  allowlist to shrink instead of quietly staying stale.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"

# Direct registry access. `Corona::Ecs::view<...>` matches through the namespace
# qualifier, so a "temporarily unprefixed" shortcut does not slip past.
FORBIDDEN = {
    "Ecs::World::instance()": re.compile(r"\bEcs::World::instance\(\)"),
    "Ecs::emplace": re.compile(r"\bEcs::emplace\b"),
    "Ecs::destroy": re.compile(r"\bEcs::destroy\b"),
    "Ecs::view<": re.compile(r"\bEcs::view<"),
    "Ecs::get<": re.compile(r"\bEcs::get<"),
}

# Files allowed to touch the registry because they either ARE the owner thread
# (DAG systems) or hold the owner-side primitives.
#
# `src/shared_data_hub.cpp` is the storage facade. It still contains the direct
# primitives for every component kind, including the three API creators that this
# slice deliberately did NOT migrate (`create_scene`, `create_actor_component`,
# `create_profile_component` — Task 1b) and `create_geometry`, which from Task 1a
# on is only reached from the tick-boundary applier in
# `include/corona/ecs/structural_commands.h` and from single-threaded test
# fixtures. The hub's own `create_*` / `destroy_*` stay here until Task 1b moves
# them behind the channel.
OWNER_FILES = {
    "src/shared_data_hub.cpp":
        "storage facade: owner-side create_*/destroy_* primitives; also hosts the "
        "never-migrated kinds and the three API creators Task 1b takes over",
    "src/systems/geometry/geometry_system.cpp":
        "GeometrySystem runs inside the systems DAG, i.e. on the registry owner thread",
}

# Non-owner translation units with pre-existing direct touches. The tuple is
# (exact number of touches per pattern, the task that removes them). Shrinking a
# count without updating this table fails the guard on purpose.
PENDING = {
    "src/systems/network/sync_engine.cpp": (
        {"Ecs::view<": 8, "Ecs::get<": 3},
        "Task 2 moves the network reads/writes onto the DAG (spec §1.2)",
    ),
}


def production_sources():
    return sorted(SRC.rglob("*.cpp"))


def relative(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def count_touches(text: str) -> dict:
    counts = {}
    for name, pattern in FORBIDDEN.items():
        found = len(pattern.findall(text))
        if found:
            counts[name] = found
    return counts


def test_no_unlisted_translation_unit_touches_the_registry():
    offenders = []
    for path in production_sources():
        name = relative(path)
        if name in OWNER_FILES or name in PENDING:
            continue
        counts = count_touches(read(path))
        if counts:
            offenders.append(f"{name} -> {counts}")
    assert not offenders, (
        "these translation units touch the ECS registry directly but are not the "
        "owner thread: " + "; ".join(offenders) + ". Structural changes go through "
        "corona/ecs/structural_commands.h (request_* + settle at the tick boundary); "
        "value writes go through the owning system's command queue. Adding an "
        "allowlist entry needs a task reference, not a comment.")


def test_pending_direct_touches_do_not_grow():
    for name, (expected, task) in PENDING.items():
        path = ROOT / name
        assert path.is_file(), f"stale allowlist entry: {name} does not exist ({task})"
        actual = count_touches(read(path))
        assert actual == expected, (
            f"{name} direct registry touches changed: expected {expected}, found {actual} "
            f"({task}). More touches: route them through the channel instead. Fewer: "
            f"lower the allowlist so the ratchet stays tight.")


def test_the_owner_files_still_exist():
    for name, reason in OWNER_FILES.items():
        assert (ROOT / name).is_file(), f"stale allowlist entry: {name} ({reason})"
    for name in PENDING:
        assert (ROOT / name).is_file(), f"stale allowlist entry: {name}"


def test_the_migrated_pilot_creates_geometry_only_through_the_channel():
    # Task 1a: `create_geometry` is requested, never called directly from a
    # production call site. Task 1b extends this to the other three creators.
    call = re.compile(r"\bcreate_geometry\s*\(")
    offenders = []
    for path in production_sources():
        name = relative(path)
        if name == "src/shared_data_hub.cpp":
            continue  # definition + the owner-side primitive itself
        for number, line in enumerate(read(path).splitlines(), start=1):
            if call.search(line):
                offenders.append(f"{name}:{number}: {line.strip()}")
    assert not offenders, (
        "create_geometry must not be called directly from a non-owner translation "
        "unit; build a Corona::Ecs::CreateGeometryPayload and call "
        "Corona::Ecs::request_create_geometry(...) so the DAG applies it at the tick "
        "boundary: " + "; ".join(offenders))


def test_structural_commands_are_applied_before_scene_commands_in_the_tick_boundary():
    # Requirement of this slice: scene membership commands must see the entities
    # created in the same tick, so the structural drain runs first.
    text = read(ROOT / "src" / "engine.cpp")
    structural = text.find("apply_pending_structural_commands()")
    scene = text.find("apply_pending_scene_commands()")
    assert structural != -1, "the tick boundary no longer applies structural commands"
    assert scene != -1, "the tick boundary no longer applies scene commands"
    assert structural < scene, (
        "apply_pending_structural_commands() must run before "
        "apply_pending_scene_commands() in the tick boundary")

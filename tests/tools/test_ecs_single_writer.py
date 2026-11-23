"""Keep ECS registry access on the systems DAG thread (ratchet).

`include/corona/ecs/world.h` has no synchronization at all: `entt::registry` must be
touched by a single thread. Every create/destroy/iterate/fetch from another thread
(CEF editor UI, scripts, network) is a data race with the DAG.

**Status of the migration (be precise, do not overclaim):**
- structural creates: `create_geometry` goes through
  `corona/ecs/structural_commands.h` (request -> settle at the tick boundary, Task 1a);
  `create_scene` / `create_actor_component` / `create_profile_component` and all
  `destroy_*` are still direct (Task 1b);
- network reads/writes are still direct (Task 2);
- one non-owner read remains on the UI candidate-surface path (see `EXCEPTIONS`).

So this guard does NOT prove "exactly one thread touches the registry" today. It
proves the set of direct touches is *exactly the documented set below and nothing
new*: a new direct touch fails, and every documented one must still exist (a stale
entry fails too, so the allowlist can only shrink).

Registry access is matched per line with the whole accessor family:

    Ecs::World::instance()  Ecs::emplace  Ecs::destroy  Ecs::view<  Ecs::get<
    Ecs::all_components<  Ecs::try_get  Ecs::try_get_const  Ecs::alive

Lines, not counts: swapping `view<T>` for `all_components<T>` keeps a count equal
but changes the line, so it fails here instead of silently staying a violation.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"

FORBIDDEN = {
    "Ecs::World::instance()": re.compile(r"\bEcs::World::instance\(\)"),
    "Ecs::emplace": re.compile(r"\bEcs::emplace\b"),
    "Ecs::destroy": re.compile(r"\bEcs::destroy\b"),
    "Ecs::view<": re.compile(r"\bEcs::view<"),
    "Ecs::get<": re.compile(r"\bEcs::get<"),
    "Ecs::all_components<": re.compile(r"\bEcs::all_components<"),
    "Ecs::try_get": re.compile(r"\bEcs::try_get\b"),
    "Ecs::try_get_const": re.compile(r"\bEcs::try_get_const\b"),
    "Ecs::alive": re.compile(r"\bEcs::alive\b"),
}

# Whole-file allowlist: these files either ARE the owner thread (the systems that
# run inside SystemsThread) or hold the owner-side primitives.
OWNER_FILES = {
    "src/shared_data_hub.cpp":
        "storage facade: owner-side create_*/destroy_* primitives. Hosts the three "
        "creators this slice did not migrate (create_scene, create_actor_component, "
        "create_profile_component, all Task 1b), the other component kinds' primitives, "
        "and create_geometry, which is now a thin wrapper over the single authoritative "
        "Ecs::create_geometry_entity() and is only called by the tick-boundary applier "
        "and by single-threaded test fixtures",
    "src/systems/geometry/geometry_system.cpp":
        "GeometrySystem runs inside the systems DAG, i.e. on the registry owner thread",
    "src/systems/geometry/geometry_queries.cpp":
        "geometry query helpers called from GeometrySystem::update() on the owner thread",
    "src/systems/acoustics/acoustics_system.cpp":
        "AcousticsSystem runs inside the systems DAG (owner thread)",
    "src/systems/mechanics/mechanics_system.cpp":
        "MechanicsSystem runs inside the systems DAG (owner thread)",
    "src/systems/optics/optics_system.cpp":
        "OpticsSystem runs inside the systems DAG (owner thread)",
}

# Per-line exceptions for non-owner translation units. Every match in the file must
# be one of the listed source lines (stripped); any other registry access in the
# same file still fails. `lines` is unique line text, so an identical line repeated
# in the file is covered once.
EXCEPTIONS = {
    "src/systems/network/sync_engine.cpp": {
        "reason": "SyncEngine reads the registry straight from the network thread; "
                  "Task 2 moves serialization onto the DAG (spec §1.2)",
        "lines": {
            "for (const auto entity : Corona::Ecs::view<ModelTransform>()) {",
            "for (const auto entity : Corona::Ecs::view<ModelResource>()) {",
            "for (const auto entity : Corona::Ecs::view<GeometryDevice>()) {",
            "for (const auto entity : Corona::Ecs::view<OpticsDevice>()) {",
            "for (const auto entity : Corona::Ecs::view<EnvironmentDevice>()) {",
            "const ModelTransform& data = Corona::Ecs::get<ModelTransform>(obj_id);",
            "const OpticsDevice& data = Corona::Ecs::get<OpticsDevice>(obj_id);",
            "const EnvironmentDevice& data = Corona::Ecs::get<EnvironmentDevice>(obj_id);",
        },
    },
    "src/engine/engine_runtime_api.cpp": {
        "reason": "KNOWN VIOLATION, not silently allowed: set_default_surface() walks "
                  "CameraDevice components on the CEF UI path (vulkan_backend.cpp calls "
                  "it) to find cameras that follow the default surface. It is a read, and "
                  "the writes it triggers already go through the camera command channel, "
                  "but the read still races the DAG. Not removed in Task 1a/1b: it needs "
                  "a published camera-follow snapshot (Task 5) or a camera-surface command "
                  "applied on the owner thread. Until then this single line is the only "
                  "registry access allowed outside the owner files",
        "lines": {
            "auto camera_storage_ref = Corona::Ecs::all_components<Corona::CameraDevice>();",
        },
    },
}


def production_sources():
    return sorted(SRC.rglob("*.cpp"))


def relative(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def registry_touches(path: Path):
    """[(line number, [token, ...], stripped line)] for every direct registry touch."""
    touches = []
    for number, line in enumerate(read(path).splitlines(), start=1):
        hits = [token for token, pattern in FORBIDDEN.items() if pattern.search(line)]
        if hits:
            touches.append((number, hits, line.strip()))
    return touches


def test_no_unlisted_translation_unit_touches_the_registry():
    offenders = []
    for path in production_sources():
        name = relative(path)
        if name in OWNER_FILES or name in EXCEPTIONS:
            continue
        for number, hits, text in registry_touches(path):
            offenders.append(f"{name}:{number} {hits} :: {text}")
    assert not offenders, (
        "these translation units touch the ECS registry directly but are not the "
        "owner thread:\n  " + "\n  ".join(offenders) + "\nStructural changes go through "
        "corona/ecs/structural_commands.h (request_* + settle at the tick boundary); "
        "value writes go through the owning system's command queue. Adding an "
        "allowlist entry needs a reason and the task that removes it.")


def test_documented_exceptions_are_exactly_what_the_source_contains():
    problems = []
    for name, spec in EXCEPTIONS.items():
        path = ROOT / name
        if not path.is_file():
            problems.append(f"stale allowlist entry: {name} does not exist")
            continue
        allowed = set(spec["lines"])
        found = {text for _, _, text in registry_touches(path)}
        for text in sorted(found - allowed):
            problems.append(f"{name}: undocumented registry access :: {text}")
        for text in sorted(allowed - found):
            problems.append(f"{name}: stale exception, line no longer present :: {text}")
    assert not problems, "\n".join(problems) + (
        "\nMigrating a site is good: delete its exception line. A new direct touch in "
        "an excepted file is not covered by the existing exception.")


def test_the_owner_files_still_exist():
    for name, reason in OWNER_FILES.items():
        assert (ROOT / name).is_file(), f"stale allowlist entry: {name} ({reason})"


def test_the_migrated_pilot_creates_geometry_only_through_the_channel():
    # Task 1a: `create_geometry` is requested, never called from a production call
    # site. The owner-side primitive in shared_data_hub.cpp is exempt.
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
    # Scene membership commands must see the entities created in the same tick.
    text = read(ROOT / "src" / "engine.cpp")
    structural = text.find("apply_pending_structural_commands()")
    scene = text.find("apply_pending_scene_commands()")
    assert structural != -1, "the tick boundary no longer applies structural commands"
    assert scene != -1, "the tick boundary no longer applies scene commands"
    assert structural < scene, (
        "apply_pending_structural_commands() must run before "
        "apply_pending_scene_commands() in the tick boundary")


def test_the_legacy_geometry_drain_is_gated_off_in_dag_mode():
    # Entity set must not change inside a tick in DAG mode: the boundary drains, and
    # GeometrySystem::update() must only drain when no boundary hook exists.
    engine = read(ROOT / "src" / "engine.cpp")
    assert "notify_tick_boundary_installed()" in engine, (
        "engine.cpp must mark the tick boundary as installed, otherwise the legacy "
        "GeometrySystem drain stays active in DAG mode")
    geometry = read(ROOT / "src" / "systems" / "geometry" / "geometry_system.cpp")
    assert "apply_pending_structural_commands_legacy_fallback()" in geometry
    assert "apply_pending_structural_commands();" not in geometry, (
        "GeometrySystem must use the gated legacy fallback, not the unconditional drain")


def test_the_create_timeout_is_derived_from_the_configured_tick():
    # Two frames of the real tick, not a hardcoded 120 Hz value: 30 Hz
    # (CORONA_SYSTEMS_DAG_TICK accepts 30..480) would otherwise time out.
    engine = read(ROOT / "src" / "engine.cpp")
    assert "set_tick_period(" in engine, (
        "engine.cpp must tell the structural command channel the real frame period")
    channel = read(ROOT / "include" / "corona" / "ecs" / "structural_commands.h")
    assert "two_frame_timeout_ms" in channel
    assert "set_tick_period" in channel

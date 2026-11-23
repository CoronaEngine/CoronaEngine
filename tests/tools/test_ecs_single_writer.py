"""Keep ECS registry access on the systems DAG thread (ratchet).

`include/corona/ecs/world.h` has no synchronization at all: `entt::registry` must be
touched by a single thread. Every create/destroy/iterate/fetch from another thread
(CEF editor UI, scripts, network) is a data race with the DAG.

**Status of the migration (be precise, do not overclaim):**
- structural creates and destroys: every production call site goes through
  `corona/ecs/structural_commands.h` (`request_create_*` / `request_destroy_*` ->
  settle at the tick boundary, Tasks 1a + 1b). `SharedDataHub::create_*` / `destroy_*`
  are owner-side primitives for the tick-boundary appliers, the DAG systems and
  single-threaded test fixtures only;
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

Documented exceptions are counted **per occurrence**, not per unique line: a second
byte-identical copy of an allowed line is still a new direct registry access, and
one line of allowance must not cover it.
"""

import re
from collections import Counter
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
        "storage facade: the owner-side accessors (`try_get` / `view` / `all_components`) "
        "and the destroy primitives (`Ecs::destroy`, `Ecs::destroy_image_entity`). Its "
        "`create_*` / `destroy_*` wrappers are default-valued shims for single-threaded "
        "test fixtures and engine assembly only; the authoritative creation primitives "
        "are `Ecs::create_*_entity()` in corona/ecs/structural_commands.h, called by the "
        "tick-boundary applier, and every production call site uses the structural "
        "command channel instead of these wrappers",
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

# Per-line exceptions for non-owner translation units. `lines` maps the stripped
# source line to the number of occurrences that are allowed: a duplicate line is a
# new direct registry access and fails, and so does a stale entry.
EXCEPTIONS = {
    "src/systems/network/sync_engine.cpp": {
        "reason": "SyncEngine reads the registry straight from the network thread; "
                  "Task 2 moves serialization onto the DAG (spec §1.2)",
        "lines": {
            # 2 occurrences each: the serialize and the deserialize loop both iterate
            # the same component type. Counting per occurrence is why these are 2 here
            # instead of one unique-line entry.
            "for (const auto entity : Corona::Ecs::view<ModelTransform>()) {": 2,
            "for (const auto entity : Corona::Ecs::view<ModelResource>()) {": 1,
            "for (const auto entity : Corona::Ecs::view<GeometryDevice>()) {": 1,
            "for (const auto entity : Corona::Ecs::view<OpticsDevice>()) {": 2,
            "for (const auto entity : Corona::Ecs::view<EnvironmentDevice>()) {": 2,
            "const ModelTransform& data = Corona::Ecs::get<ModelTransform>(obj_id);": 1,
            "const OpticsDevice& data = Corona::Ecs::get<OpticsDevice>(obj_id);": 1,
            "const EnvironmentDevice& data = Corona::Ecs::get<EnvironmentDevice>(obj_id);": 1,
        },
    },
    "src/engine/engine_runtime_api.cpp": {
        "reason": "KNOWN VIOLATION in this file, not silently allowed: set_default_surface() "
                  "walks CameraDevice components on the CEF UI path (vulkan_backend.cpp "
                  "calls it) to find cameras that follow the default surface. It is a read, "
                  "and the writes it triggers already go through the camera command "
                  "channel, but the read still races the DAG. Not removed in Tasks 1a/1b: "
                  "it needs a published camera-follow snapshot (Task 5) or a camera-surface "
                  "command applied on the owner thread. This file has exactly this one "
                  "allowance; the other non-owner allowance is the sync_engine.cpp block "
                  "above (Task 2), and neither covers the other's lines",
        "lines": {
            "auto camera_storage_ref = Corona::Ecs::all_components<Corona::CameraDevice>();": 1,
        },
    },
}

# Structural primitives of the hub. Only the hub itself (definitions), the DAG
# systems (owner thread) and single-threaded fixtures may call them; a production
# non-owner call site must build a payload and use `Ecs::request_*`.
STRUCTURAL_PRIMITIVE = re.compile(
    r"\b(?:create|destroy)_(?:model_resource|transform|geometry|mechanics|acoustics"
    r"|optics|profile_component|actor_component|camera|environment|scene|image)\s*\(")


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
        allowed = spec["lines"]
        found = Counter(text for _, _, text in registry_touches(path))
        for text, count in sorted(found.items()):
            if text not in allowed:
                problems.append(
                    f"{name}: undocumented registry access ({count} occurrence(s)) :: {text}")
            elif count > allowed[text]:
                problems.append(
                    f"{name}: {count} occurrences but only {allowed[text]} documented :: {text} "
                    "(a duplicated allowlisted line is still a new direct registry access)")
        for text, count in sorted(allowed.items()):
            if found.get(text, 0) < count:
                problems.append(
                    f"{name}: stale exception, line no longer present :: {text}")
    assert not problems, "\n".join(problems) + (
        "\nMigrating a site is good: delete its exception line. A new direct touch in "
        "an excepted file is not covered by the existing exception.")


def test_the_owner_files_still_exist():
    for name, reason in OWNER_FILES.items():
        assert (ROOT / name).is_file(), f"stale allowlist entry: {name} ({reason})"


def test_structural_creates_and_destroys_go_through_the_channel():
    # Task 1a/1b: no production call site calls the hub's structural primitives.
    # The owner thread and the hub itself are exempt; test fixtures under tests/ are
    # not scanned (they are single-threaded by construction).
    offenders = []
    for path in production_sources():
        name = relative(path)
        if name == "src/shared_data_hub.cpp" or name in OWNER_FILES:
            continue
        for number, line in enumerate(read(path).splitlines(), start=1):
            if STRUCTURAL_PRIMITIVE.search(line):
                offenders.append(f"{name}:{number}: {line.strip()}")
    assert not offenders, (
        "structural writes must not call SharedDataHub::create_*/destroy_* from a "
        "non-owner translation unit; build a Corona::Ecs::Create*Payload and call "
        "Corona::Ecs::request_create_*(...) (or request_destroy_entity / "
        "request_destroy_image) so the DAG applies it at the tick boundary: "
        + "; ".join(offenders))


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


def test_the_create_wait_is_liveness_based_not_wall_clock():
    # Task 1c: waiting must be judged by "is the DAG advancing", not by a fixed budget.
    # The two-frame value survives only as a diagnostic threshold, and the assembly still
    # has to tell the channel the real frame period (it now only decides when a slow wait
    # logs a warning). Failures are bounded by the no-progress budget (default 10 s).
    engine = read(ROOT / "src" / "engine.cpp")
    assert "set_tick_period(" in engine, (
        "engine.cpp must tell the structural command channel the real frame period")
    channel = read(ROOT / "include" / "corona" / "ecs" / "structural_commands.h")
    assert "two_frame_diagnostic_ms" in channel, (
        "the old two-frame value must be demoted to a diagnostic threshold")
    assert "two_frame_timeout_ms" not in channel, (
        "the two-frame value must not be used as a wait limit again")
    assert "publish_tick_boundary" in channel, (
        "the tick boundary must publish a heartbeat for the liveness wait")
    assert "no_progress_budget" in channel, (
        "structural waits must be bounded by a no-progress budget, not a wall-clock timeout")
    assert "kDefaultNoProgressBudgetMs = 10'000" in channel, (
        "the default no-progress budget must be 10s")
    # Every boundary applier goes through the beating applier; the legacy fallback must
    # keep using it so a CORONA_SYSTEMS_DAG=0 run also advances the heartbeat.
    assert "apply_pending_structural_commands_legacy_fallback()" in read(
        ROOT / "src" / "systems" / "geometry" / "geometry_system.cpp"), (
        "the legacy fallback boundary must still go through the beating applier")
    assert "publish_tick_boundary()" in channel.split("bool apply_pending()")[1].split("}")[0], (
        "apply_pending() must publish the heartbeat before draining")

    # Assembly must tell the channel that an owner is coming; before that, structural
    # requests apply on the calling thread (no second writer exists yet).
    assert "notify_owner_expected()" in engine, (
        "engine.cpp must announce that a structural owner thread is coming, otherwise "
        "runtime structural requests keep applying on the calling thread")


def test_a_stuck_dag_does_not_multiply_the_wait_per_entity():
    # Task 1c review: once a no-progress timeout proves the DAG is not advancing, later
    # requests must fail immediately (the latch), and destroys must never wait - otherwise
    # a failed load's rollback costs N entities x budget on the CEF/main thread.
    channel = read(ROOT / "include" / "corona" / "ecs" / "structural_commands.h")
    assert "unresponsive_" in channel and "mark_unresponsive" in channel
    assert "kUnresponsiveReason" in channel and "DAG_UNRESPONSIVE" in channel
    assert "if (channel.unresponsive())" in channel, (
        "submit_structural_command must reject fast once the DAG is unresponsive")
    assert "submit_structural_command_without_waiting" in channel
    destroy = channel.split("request_destroy_entity(EntityId handle")[1].split(
        "request_destroy_image")[0]
    assert "submit_structural_command_without_waiting(" in destroy
    assert "submit_structural_command(" not in destroy, (
        "destroys must not wait for settlement: the rollback path would multiply the "
        "no-progress budget by the entity count")


def test_a_profile_without_engine_storage_fails_the_actor_materialisation():
    # Task 1c review: a Profile* handed out without its engine-side ProfileStorage is a
    # half-linked actor. add_profile must report that, and the editor load path must treat
    # it as the same failure as a missing actor entity.
    api = read(ROOT / "src" / "engine" / "engine_runtime_api.cpp")
    add_profile = api.split("Corona::API::Actor::add_profile")[1].split(
        "void Corona::API::Actor::remove_profile")[0]
    assert "storage_profile_handle == 0" in add_profile
    assert "profiles_.erase(profile_handle)" in add_profile, (
        "add_profile must roll back the in-memory registration when the storage create "
        "did not settle")
    assert "return nullptr" in add_profile, (
        "add_profile must not hand back a usable Profile when its storage is missing")

    editor = read(ROOT / "src" / "systems" / "ui" / "cef" / "editor_scene_service.cpp")
    materialize = editor.split("NativeEditorActor& add_native_actor_to_scene")[1].split(
        "void materialize_actor_snapshot")[0]
    assert "add_profile(" in materialize
    assert "profile_ref != nullptr" in materialize, (
        "the editor must take the structural failure path when add_profile returns nullptr")
    assert "StructuralCreateFailed" in materialize

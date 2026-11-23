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
- network reads/writes: zero direct touches **in network translation units** since
  Task 2. `sync_engine.cpp` requests a serialized frame from the tick boundary
  (`sync_boundary.h::request_sync_frame`) and hands incoming remote state to the same
  boundary (`submit_remote_state`); the component reads/writes live in
  `src/systems/network/sync_boundary.cpp`, which is an owner-thread file (registered on
  the structural channel's tick boundary). NOT clean yet: the header-only
  `NetworkIdentityRegistry::register_actor()` (see `KNOWN_HEADER_VIOLATIONS`) still reads
  components and its production caller runs on the network thread;
- one non-owner read remains on the UI candidate-surface path (see `EXCEPTIONS`).

So this guard does NOT prove "exactly one thread touches the registry" today. It
proves the set of direct touches is *exactly the documented set below and nothing
new*: a new direct touch fails, and every documented one must still exist (a stale
entry fails too, so the allowlist can only shrink).

Registry access is matched per line with the whole accessor family:

    Ecs::World::instance()  Ecs::emplace  Ecs::destroy  Ecs::view<  Ecs::get<
    Ecs::all_components<  Ecs::try_get  Ecs::try_get_const  Ecs::alive

Network translation units are also matched against the owner-side convenience accessors
(`Corona::find_*` / `try_*`): the header-only escape is documented in
`KNOWN_HEADER_VIOLATIONS`, and a *.cpp* in `src/systems/network/` must not reintroduce
those reads. The accessor family is not enforced engine-wide yet: many pre-existing
non-owner call sites read components through it (Tasks 5/6 territory), and adding them to
the global list here would only pile up new allowances.

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
    "src/systems/network/sync_boundary.cpp":
        "network sync boundary applier: NOT the network thread. It is registered as a "
        "tick-boundary task on the structural command channel (Task 2), so it runs on the "
        "registry owner thread - the DAG boundary hook in DAG mode, GeometrySystem's "
        "legacy drain with CORONA_SYSTEMS_DAG=0, or the calling thread in single-threaded "
        "fixtures before assembly. `sync_engine.cpp` (the network thread) has no registry "
        "access and no allowance",
}

# Per-line exceptions for non-owner translation units. `lines` maps the stripped
# source line to the number of occurrences that are allowed: a duplicate line is a
# new direct registry access and fails, and so does a stale entry.
EXCEPTIONS = {
    "src/engine/engine_runtime_api.cpp": {
        "reason": "KNOWN VIOLATION in this file, not silently allowed: set_default_surface() "
                  "walks CameraDevice components on the CEF UI path (vulkan_backend.cpp "
                  "calls it) to find cameras that follow the default surface. It is a read, "
                  "and the writes it triggers already go through the camera command "
                  "channel, but the read still races the DAG. Not removed in Tasks 1a/1b: "
                  "it needs a published camera-follow snapshot (Task 5) or a camera-surface "
                  "command applied on the owner thread. This is the ONLY per-line "
                  "non-owner allowance left: the network allowance went to zero in Task 2 "
                  "(`sync_engine.cpp` no longer touches the registry at all)",
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

# The owner-side convenience accessors (`Corona::find_actor`, `hub.try_optics`, ...).
# They are registry reads with no synchronization; the header-only network-identity read
# escaped the `Ecs::` token list through them. Enforced for network *.cpp* units (where
# Task 2 closed the registry access); NOT engine-wide yet because many pre-existing
# non-owner call sites use them (Tasks 5/6 territory) and adding them to the global list
# would only pile up new allowances.
ACCESSOR_FAMILY = re.compile(
    r"\b(?:find_(?:actor|profiles|profile|transform|optics|visibility|geometry"
    r"|model_resource|mechanics|bounds|scene|camera|model_source|streaming|model_path"
    r"|follow_camera|environment|acoustics|image)"
    r"|try_(?:actor|profiles|profile|transform|optics|visibility|geometry"
    r"|model_resource|mechanics|bounds|scene|camera|model_source|streaming|model_path"
    r"|follow_camera|environment|acoustics|image))\s*\(")

# Known header-only violations the .cpp scanner cannot see. Each entry is a named
# follow-up, not an allowance: the test asserts the file still exists so this note
# cannot go stale silently.
KNOWN_HEADER_VIOLATIONS = {
    "include/corona/systems/network/network_identity.h":
        "NetworkIdentityRegistry::register_actor() reads ActorDevice / Profiles / "
        "GeometryDevice / OpticsDevice / MechanicsDevice / AcousticsDevice through "
        "find_*/try_*, and its production caller (the RegisterActorIdentity command in "
        "network_system.cpp) is drained on the network thread. Out of Task 2 scope; "
        "named follow-up in task-2-report.md §9",
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
    assert "unresponsive_beat_" in channel, (
        "the latch must be stamped with the heartbeat, not a plain bool: a completed "
        "boundary has to invalidate it without a clear that can race")
    assert "kUnresponsiveReason" in channel and "DAG_UNRESPONSIVE" in channel
    assert "if (channel.unresponsive())" in channel, (
        "submit_structural_command must reject fast once the DAG is unresponsive")
    assert "submit_structural_command_without_waiting" in channel
    destroy = channel.split("submit_structural_command_without_waiting")[1].split(
        "request_destroy_entity(EntityId handle")[0]
    assert "enqueue_deferred_destroy(" in destroy


def test_the_deferred_destroy_queue_is_not_the_capacity_capped_fifo():
    # Task 1c review item 1: a fire-and-forget teardown of a large scene (~8 destroys per
    # actor) must not hit CommandBuffer's 4096 cap - rejected destroys would orphan live
    # entities (and their GPU resources) on a perfectly healthy DAG.
    channel = read(ROOT / "include" / "corona" / "ecs" / "structural_commands.h")
    assert "std::deque<DeferredDestroy>" in channel
    assert "destroy_queue_.push_back(" in channel
    assert "enqueue_deferred_destroy" in channel
    assert "pending_destroy_count" in channel
    destroy = channel.split("submit_structural_command_without_waiting")[1].split(
        "request_destroy_entity(EntityId handle")[0]
    assert "commands().push(" not in destroy, (
        "deferred destroys must not go through the capacity-capped shared Fifo")
    assert "destroy_queue_.size() >=" not in channel, (
        "the deferred destroy queue must not reject because of a fixed capacity")


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


def test_the_network_thread_has_no_registry_allowance_at_all():
    # Task 2: the per-line network allowance is GONE, not reduced. Any direct touch left in
    # a network translation unit must fail the first test instead of being covered here.
    assert not any(name.startswith("src/systems/network/") for name in EXCEPTIONS), (
        "network translation units must have no per-line registry allowance")
    assert "src/systems/network/sync_engine.cpp" not in OWNER_FILES, (
        "sync_engine.cpp runs on the network thread; it is not an owner file")

    engine = read(ROOT / "src" / "systems" / "network" / "sync_engine.cpp")
    assert "request_sync_frame(" in engine, (
        "the network thread must ask the tick boundary for its outbound frame")
    assert "submit_remote_state(" in engine, (
        "incoming remote state must be handed to the tick boundary")
    for token in ("Ecs::view<", "Ecs::get<", "Ecs::try_get", "Ecs::all_components<",
                  "Ecs::emplace", "Ecs::destroy", "Ecs::World::instance()"):
        assert token not in engine, f"network thread must not use {token}"


def test_network_serialisation_runs_on_the_owner_side_of_the_tick_boundary():
    # The serialiser is a task on the SAME boundary as structural commands, so both the DAG
    # hook and the CORONA_SYSTEMS_DAG=0 fallback (GeometrySystem -> apply_pending) execute
    # it on the registry owner thread.
    channel = read(ROOT / "include" / "corona" / "ecs" / "structural_commands.h")
    assert "using TickBoundaryTask = void (*)();" in channel
    assert "add_tick_boundary_task" in channel
    assert "run_boundary_tasks();" in channel, (
        "apply_pending() must execute the registered boundary tasks")

    network = read(ROOT / "src" / "systems" / "network" / "network_system.cpp")
    assert "Corona::Ecs::add_tick_boundary_task(" in network, (
        "NetworkSystem::initialize must register the sync boundary on the ECS tick boundary")
    assert "&Corona::Network::apply_pending_sync_boundary" in network, (
        "the registered boundary task must be the owner-side sync applier")
    assert "add_tick_boundary_stop_task" in channel
    assert "&Corona::Network::reset_sync_boundary_state" in network, (
        "the DAG stop hook must clear queued remote writes (no writes across a restart)")

    owner = read(ROOT / "src" / "systems" / "network" / "sync_boundary.cpp")
    assert "Ecs::view<" in owner, (
        "the owner-side frame serialiser reads the registry (on the owner thread)")
    assert "apply_pending()" in owner and "apply_remote_state(" in owner

    # Round-3 hardenings that must not silently regress:
    #  - the write queue is bounded and coalesces with true write-recency (list + move to
    #    the back), so "drop oldest" cannot evict a just-refreshed key;
    #  - the full-sync target list is deduped/capped (one request cannot amplify).
    assert "kMaxQueuedRemoteWrites = 4096" in owner
    assert "std::list<RemoteStateWrite>" in owner
    assert "writes_.erase(existing->second)" in owner and "std::prev(writes_.end())" in owner
    assert "kMaxPendingFullSyncs" in network and "queue_full_sync(" in network
    assert "std::erase(pending_full_syncs, peer_id)" in network

    # The wire codec is shared by both threads and must stay pure values: no registry.
    codec = read(ROOT / "include" / "corona" / "systems" / "network" / "sync_codec.h")
    assert "Ecs::" not in codec, "sync_codec.h must be pure values (no registry access)"
    assert "static_assert(sizeof(OpticsPacked) == 84" in codec, (
        "the optics wire layout is the protocol; pin it")

    # The network-facing header must stay declaration-only (the registry touches are all in
    # the owner-side .cpp, listed in OWNER_FILES).
    boundary = read(ROOT / "include" / "corona" / "systems" / "network" / "sync_boundary.h")
    assert "Ecs::view<" not in boundary and "Ecs::get<" not in boundary


def test_network_translation_units_do_not_use_registry_accessors():
    # The `Ecs::` token list above cannot see the owner-side convenience accessors
    # (`Corona::find_*` / `try_*`), which is exactly how the header-only network-identity
    # read escapes it. Enforce the family for network .cpp units: a new direct accessor
    # read on the network thread must fail instead of hiding behind the `Ecs::` spelling.
    offenders = []
    network_src = SRC / "systems" / "network"
    for path in sorted(network_src.rglob("*.cpp")):
        name = relative(path)
        if name in OWNER_FILES:
            # The owner-side applier is allowed to read components; it runs on the owner.
            continue
        for number, line in enumerate(read(path).splitlines(), start=1):
            if ACCESSOR_FAMILY.search(line):
                offenders.append(f"{name}:{number}: {line.strip()}")
    assert not offenders, (
        "network translation units must not read components through the owner-side "
        "accessors (that is a registry read on the network thread):\n  " +
        "\n  ".join(offenders))


def test_known_header_only_violations_are_named_and_still_exist():
    for name, reason in KNOWN_HEADER_VIOLATIONS.items():
        assert (ROOT / name).is_file(), (
            f"stale known header violation: {name} no longer exists; remove the entry")
        assert reason.strip(), f"{name} needs a reason / follow-up reference"

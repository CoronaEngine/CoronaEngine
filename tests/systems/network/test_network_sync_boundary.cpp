// Task 2: the network sync boundary is served by the registry owner thread.
//
// Two things are covered here that the single-threaded protocol TU cannot:
//
//  1. a real two-thread harness: a dedicated owner thread publishes the tick boundary
//     and drains the network boundary queues, while the test thread behaves like the
//     network thread (requests an outbound frame, submits remote state) and never
//     touches the registry after the owner thread starts;
//  2. the per-frame cost of the boundary scan at a realistic actor count. The identity
//     lookup moved from "per entity × O(actors) on the tick boundary" to "one O(actors)
//     index snapshot on the network thread + O(1) per entity on the boundary"; the
//     benchmark prints the before/after numbers.

#include <corona/ecs/access.h>
#include <corona/ecs/structural_commands.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/network/network_identity.h>
#include <corona/systems/network/protocol.h>
#include <corona/systems/network/sync_boundary.h>
#include <corona/systems/network/sync_codec.h>
#include <corona/systems/network/sync_engine.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

int g_failed = 0;

void expect_true(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failed;
    }
}

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = std::chrono::seconds{10}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return predicate();
}

template <typename Clock = std::chrono::steady_clock, typename Fn>
double elapsed_ms(Fn&& fn) {
    const auto start = Clock::now();
    fn();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

bool outgoing_has(const std::vector<std::vector<std::uint8_t>>& packets,
                  const std::string& actor_guid, const char* field) {
    for (const auto& packet : packets) {
        const auto operation =
            Corona::Network::parse_editor_sync_operation(packet.data(), packet.size());
        if (operation && operation->actor_guid == actor_guid &&
            operation->field_name == field) {
            return true;
        }
    }
    return false;
}

bool outgoing_has_environment(const std::vector<std::vector<std::uint8_t>>& packets) {
    for (const auto& packet : packets) {
        const auto operation =
            Corona::Network::parse_editor_sync_operation(packet.data(), packet.size());
        if (operation && operation->actor_guid == "__scene__" &&
            operation->field_name == "environment") {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Fixture: one wired actor (actor + profile + geometry + transform + optics) in the
// single-threaded world. Returns the handles; the caller destroys them.
// ---------------------------------------------------------------------------

struct ActorFixture {
    Corona::EntityId transform{0};
    Corona::EntityId model_resource{0};
    Corona::EntityId geometry{0};
    Corona::EntityId optics{0};
    Corona::EntityId environment{0};
    Corona::EntityId profile{0};
    Corona::EntityId actor{0};
};

ActorFixture make_actor_fixture(Corona::SharedDataHub& hub,
                                const std::string& actor_guid,
                                bool with_environment = false) {
    ActorFixture fixture;
    fixture.transform = hub.create_transform();
    fixture.model_resource = hub.create_model_resource();
    fixture.geometry = hub.create_geometry();
    fixture.optics = hub.create_optics();
    fixture.profile = hub.create_profile_component();
    fixture.actor = hub.create_actor_component();
    if (with_environment) {
        fixture.environment = hub.create_environment();
    }
    {
        auto* g = Corona::find_geometry(fixture.geometry);
        g->transform_handle = fixture.transform;
        g->model_resource_handle = fixture.model_resource;
    }
    {
        auto* o = Corona::find_optics(fixture.optics);
        o->geometry_handle = fixture.geometry;
    }
    {
        auto* p = hub.try_profile(fixture.profile);
        p->geometry_handle = fixture.geometry;
        p->optics_handle = fixture.optics;
    }
    {
        auto* profiles = Corona::find_profiles(fixture.actor);
        profiles->handles.push_back(fixture.profile);
    }
    (void)actor_guid;
    return fixture;
}

void destroy_fixture(Corona::SharedDataHub& hub, const ActorFixture& fixture) {
    if (fixture.actor != 0) hub.destroy_actor_component(fixture.actor);
    if (fixture.profile != 0) hub.destroy_profile_component(fixture.profile);
    if (fixture.environment != 0) hub.destroy_environment(fixture.environment);
    if (fixture.optics != 0) hub.destroy_optics(fixture.optics);
    if (fixture.geometry != 0) hub.destroy_geometry(fixture.geometry);
    if (fixture.model_resource != 0) hub.destroy_model_resource(fixture.model_resource);
    if (fixture.transform != 0) hub.destroy_transform(fixture.transform);
}

// ---------------------------------------------------------------------------
// Benchmark: boundary frame-build cost at a realistic actor count.
// ---------------------------------------------------------------------------

void test_frame_build_cost_at_realistic_actor_count() {
    constexpr int kActors = 2000;
    auto& hub = Corona::SharedDataHub::instance();
    Corona::Network::NetworkIdentityRegistry registry(hub);

    std::vector<ActorFixture> fixtures;
    fixtures.reserve(kActors);
    std::vector<std::uint64_t> transform_seqs;
    transform_seqs.reserve(kActors);
    for (int i = 0; i < kActors; ++i) {
        auto fixture = make_actor_fixture(hub, "bench-actor", false);
        {
            auto* t = Corona::find_transform(fixture.transform);
            t->position = ktm::fvec3{static_cast<float>(i), 1.0f, 2.0f};
        }
        const auto guid = "bench-actor-" + std::to_string(i);
        (void)registry.register_actor(guid, fixture.actor);
        transform_seqs.push_back(
            static_cast<std::uint64_t>(Corona::Ecs::seq_of(fixture.transform)));
        fixtures.push_back(fixture);
    }
    expect_true(static_cast<int>(fixtures.size()) == kActors,
                "benchmark fixture created");

    // BEFORE: what the old boundary did per entity — an O(actors) linear scan under the
    // registry mutex for the guid and again for ownership, twice per actor.
    const double old_identity_ms = elapsed_ms([&] {
        std::size_t found = 0;
        for (const auto seq : transform_seqs) {
            if (!registry
                     .actor_guid_for_storage_seq(Corona::Network::StorageID::ST_MODEL_TRANSFORM,
                                                 seq)
                     .empty()) {
                ++found;
            }
            (void)registry.local_ownership_for_storage_seq(
                Corona::Network::StorageID::ST_MODEL_TRANSFORM, seq);
        }
        expect_true(found == transform_seqs.size(),
                    "benchmark before-path resolves every transform");
    });

    // AFTER: one index snapshot (network thread) + a boundary scan that only resolves
    // identities for dirty rows.
    std::vector<Corona::Network::SyncIdentityEntry> index;
    const double index_ms = elapsed_ms([&] { index = registry.sync_identity_index(); });

    std::unordered_map<std::uint64_t, std::uint64_t> last_synced;
    auto key_of = [](Corona::Network::StorageID storage, std::uint64_t seq) {
        return (static_cast<std::uint64_t>(storage) << 56) | (seq & 0x00FFFFFFFFFFFFFFull);
    };
    Corona::Network::SyncFrameRequestOptions options;
    options.identity_index = index;
    options.is_dirty = [&last_synced, &key_of](Corona::Network::StorageID storage,
                                               std::uint64_t seq, std::uint64_t hash) {
        const auto it = last_synced.find(key_of(storage, seq));
        return it == last_synced.end() || it->second != hash;
    };

    // First frame: every row is dirty (identity + encode for each row).
    double dirty_frame_ms = 0.0;
    double dirty_rows = 0.0;
    dirty_frame_ms = elapsed_ms([&] {
        auto frame = Corona::Network::request_sync_frame(options);
        expect_true(frame.has_value(), "benchmark dirty frame is built");
        const auto rows = frame->transforms.size() + frame->optics.size();
        dirty_rows = static_cast<double>(rows);
        for (const auto& row : frame->transforms) {
            last_synced[key_of(Corona::Network::StorageID::ST_MODEL_TRANSFORM,
                               row.entity_seq)] = row.hash;
        }
        for (const auto& row : frame->optics) {
            last_synced[key_of(Corona::Network::StorageID::ST_OPTICS, row.entity_seq)] =
                row.hash;
        }
    });

    // Second frame: steady state, nothing dirty -> no identity lookup, no encoding.
    double clean_frame_ms = 0.0;
    double clean_rows = 0.0;
    clean_frame_ms = elapsed_ms([&] {
        auto frame = Corona::Network::request_sync_frame(options);
        expect_true(frame.has_value(), "benchmark clean frame is built");
        clean_rows = static_cast<double>(frame->transforms.size() + frame->optics.size());
    });

    std::cout << "[sync-boundary] actors=" << kActors << " old per-frame identity scan="
              << old_identity_ms << "ms index snapshot=" << index_ms
              << "ms dirty frame=" << dirty_frame_ms << "ms (" << dirty_rows
              << " rows) clean frame=" << clean_frame_ms << "ms (" << clean_rows
              << " rows)\n";

    expect_true(dirty_rows == 2 * kActors,
                "the first frame carries transform + optics for every actor");
    expect_true(clean_rows == 0,
                "a steady-state frame carries only dirty rows (none here)");
    expect_true(clean_frame_ms < old_identity_ms,
                "the clean frame build is cheaper than the old per-entity identity scan");
    expect_true(index_ms < old_identity_ms,
                "one index snapshot is cheaper than a per-entity scan");

    for (const auto& fixture : fixtures) {
        destroy_fixture(hub, fixture);
    }
}

// ---------------------------------------------------------------------------
// Two-thread harness
// ---------------------------------------------------------------------------

void test_remote_state_is_applied_by_the_owner_thread_and_never_echoed() {
    auto& hub = Corona::SharedDataHub::instance();

    // ---- fixture: single-threaded construction (no owner thread yet) ----
    const auto fixture = make_actor_fixture(hub, "actor-boundary", true);
    const auto transform = fixture.transform;
    const auto optics = fixture.optics;
    const auto environment = fixture.environment;
    {
        auto* t = Corona::find_transform(transform);
        t->position = ktm::fvec3{1.0f, 2.0f, 3.0f};
    }
    {
        auto* o = Corona::find_optics(optics);
        o->metallic = 0.25f;
    }
    {
        auto* e = hub.try_environment(environment);
        e->exposure = 1.5f;
    }

    Corona::Network::NetworkIdentityRegistry registry(hub);
    expect_true(registry.register_actor("actor-boundary", fixture.actor),
                "identity registration for the sync boundary fixture succeeds");

    // ---- start the registry owner thread (the DAG boundary in production) ----
    (void)Corona::Ecs::add_tick_boundary_task(&Corona::Network::apply_pending_sync_boundary);
    (void)Corona::Ecs::add_tick_boundary_stop_task(
        &Corona::Network::reset_sync_boundary_state);
    Corona::Network::reset_sync_boundary_stats();
    Corona::Ecs::StructuralCommandChannel::instance().notify_owner_expected();

    std::atomic<bool> stop{false};
    std::thread owner_thread([&stop] {
        while (!stop.load(std::memory_order_acquire)) {
            Corona::Ecs::apply_pending_structural_commands();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    });

    Corona::Network::SyncEngine sync;
    std::vector<std::vector<std::uint8_t>> outgoing;
    sync.initialize("local-peer");
    sync.set_identity_mapping_callbacks(
        [&registry](Corona::Network::StorageID storage_id, std::uint64_t entity_seq) {
            return registry.actor_guid_for_storage_seq(storage_id, entity_seq);
        },
        [&registry](Corona::Network::StorageID storage_id, const std::string& actor_guid)
            -> std::optional<std::uint64_t> {
            return registry.storage_seq_for_actor_guid(storage_id, actor_guid);
        },
        [&registry](Corona::Network::StorageID storage_id, std::uint64_t entity_seq)
            -> std::optional<bool> {
            return registry.local_ownership_for_storage_seq(storage_id, entity_seq);
        },
        [&registry] { return registry.sync_identity_index(); });
    sync.set_on_outgoing([&outgoing](const std::vector<std::uint8_t>& packet) {
        outgoing.push_back(packet);
    });

    // 1) Outbound frame: built on the owner thread (frames_inline stays 0) and carries
    //    the current transform, optics and environment state.
    sync.poll_and_sync();
    expect_true(outgoing_has(outgoing, "actor-boundary", "xform"),
                "owner-thread frame emits the actor transform");
    expect_true(outgoing_has(outgoing, "actor-boundary", "optics"),
                "owner-thread frame emits the optics state");
    expect_true(outgoing_has_environment(outgoing),
                "owner-thread frame emits the environment state");
    expect_true(Corona::Network::sync_boundary_stats().frames_built >= 1,
                "the frame was built at the boundary");
    expect_true(Corona::Network::sync_boundary_stats().frames_inline == 0,
                "the frame was NOT built inline on the network thread");

    // 2) Remote state is submitted from this (network) thread and applied by the owner.
    auto submit_remote = [&sync](const char* field, const std::string& actor_guid,
                                 std::vector<std::uint8_t> value, std::uint64_t version) {
        Corona::Network::EditorSyncOperation op;
        op.kind = Corona::Network::EditorSyncOperationKind::Upsert;
        op.actor_guid = actor_guid;
        op.field_name = field;
        op.value = std::move(value);
        op.version = {version, "remote-peer"};
        return sync.apply_editor_operation(op) ==
               Corona::Network::LwwApplyResult::Applied;
    };

    Corona::Network::TransformState xform_state{};
    xform_state.xform[0] = 99.0f;
    xform_state.xform[1] = 98.0f;
    xform_state.xform[2] = 97.0f;
    xform_state.xform[6] = 1.0f;
    xform_state.xform[7] = 1.0f;
    xform_state.xform[8] = 1.0f;
    expect_true(submit_remote("xform", "actor-boundary",
                              Corona::Network::encode_transform(xform_state), 100),
                "remote transform operation is accepted");

    Corona::Network::OpticsPacked optics_state{};
    optics_state.metallic = 0.9f;
    optics_state.roughness = 0.1f;
    expect_true(submit_remote("optics", "actor-boundary",
                              Corona::Network::encode_optics(optics_state), 101),
                "remote optics operation is accepted");

    Corona::Network::EnvironmentState environment_state{};
    environment_state.sun_intensity = 3.0f;
    environment_state.exposure = 2.5f;
    expect_true(submit_remote("environment", "__scene__",
                              Corona::Network::encode_environment(environment_state), 102),
                "remote environment operation is accepted");

    // 3) THE ordering assertion: poll immediately (the writes are queued but not
    //    necessarily applied yet). The boundary drains writes before building the frame,
    //    so the just-received remote values must not be echoed back. If the frame were
    //    built first, the old local values would be dirty against the recorded remote
    //    hashes and would be sent back.
    outgoing.clear();
    sync.poll_and_sync();
    expect_true(!outgoing_has(outgoing, "actor-boundary", "xform"),
                "a remote transform is not re-broadcast (writes-before-frames)");
    expect_true(!outgoing_has(outgoing, "actor-boundary", "optics"),
                "a remote optics state is not re-broadcast (writes-before-frames)");
    expect_true(!outgoing_has_environment(outgoing),
                "a remote environment state is not re-broadcast");

    expect_true(wait_until([] {
                    return Corona::Network::sync_boundary_stats().remote_writes_applied >= 3;
                }),
                "the owner thread applied the three remote writes");

    outgoing.clear();
    sync.poll_and_sync();
    expect_true(outgoing.empty(), "a settled remote state produces no further packets");

    // 4) Stop the owner before reading the components: single writer again.
    stop.store(true, std::memory_order_release);
    owner_thread.join();

    const auto* applied_transform = Corona::find_transform(transform);
    expect_true(applied_transform != nullptr && applied_transform->position.x == 99.0f &&
                    applied_transform->position.y == 98.0f &&
                    applied_transform->position.z == 97.0f,
                "the remote transform landed in the component (applied by the owner)");
    const auto* applied_optics = Corona::find_optics(optics);
    expect_true(applied_optics != nullptr && applied_optics->metallic == 0.9f &&
                    applied_optics->roughness == 0.1f,
                "the remote optics state landed in the component");
    const auto* applied_environment = hub.try_environment(environment);
    expect_true(applied_environment != nullptr &&
                    applied_environment->sun_intensity == 3.0f &&
                    applied_environment->exposure == 2.5f,
                "the remote environment state landed in the component");
    expect_true(Corona::Network::sync_boundary_stats().remote_writes_dropped == 0,
                "no remote write was dropped for an existing target");

    // 5) Once the DAG is gone, both directions fail fast (no wall-clock budget, no hang).
    Corona::Ecs::on_dag_thread_stopping();
    Corona::Network::RemoteStateWrite late_write{};
    late_write.storage = Corona::Network::StorageID::ST_MODEL_TRANSFORM;
    late_write.entity_seq = static_cast<std::uint64_t>(Corona::Ecs::seq_of(transform));
    expect_true(!Corona::Network::submit_remote_state(late_write),
                "a remote write is rejected immediately once the DAG is stopped");
    Corona::Network::SyncFrameRequestOptions options;
    options.guid_for_entity =
        [&registry](Corona::Network::StorageID storage_id, std::uint64_t entity_seq) {
            return registry.actor_guid_for_storage_seq(storage_id, entity_seq);
        };
    expect_true(!Corona::Network::request_sync_frame(std::move(options)).has_value(),
                "a frame request is rejected immediately once the DAG is stopped");
    expect_true(Corona::Network::sync_boundary_stats().remote_writes_rejected >= 1,
                "the rejected remote write is counted");

    sync.shutdown();
    destroy_fixture(hub, fixture);
}

// ---------------------------------------------------------------------------
// Queue policy: coalesce per key, cap at 4096 (drop oldest), reset on stop.
// ---------------------------------------------------------------------------

void test_remote_write_queue_policy() {
    auto& hub = Corona::SharedDataHub::instance();
    const auto fixture = make_actor_fixture(hub, "actor-queue", false);

    // The previous test stopped the shared channel on purpose; a new "DAG run" restarts
    // it (the engine only stops once, at shutdown).
    Corona::Ecs::on_dag_thread_started();
    (void)Corona::Ecs::add_tick_boundary_task(&Corona::Network::apply_pending_sync_boundary);
    Corona::Network::reset_sync_boundary_stats();

    std::atomic<bool> stop{false};
    // The owner thread only drains while it holds the gate; the test holds the gate
    // across "submit + inspect stats" so it can observe the queue deterministically.
    std::mutex boundary_gate;
    std::thread owner_thread([&stop, &boundary_gate] {
        while (!stop.load(std::memory_order_acquire)) {
            {
                std::lock_guard lock(boundary_gate);
                Corona::Ecs::apply_pending_structural_commands();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    });

    Corona::Network::RemoteStateWrite write{};
    write.storage = Corona::Network::StorageID::ST_MODEL_TRANSFORM;
    write.entity_seq = static_cast<std::uint64_t>(Corona::Ecs::seq_of(fixture.transform));

    // Coalesce: three submissions for the same (storage, seq) keep only the newest.
    {
        std::lock_guard gate(boundary_gate);
        Corona::Network::reset_sync_boundary_stats();
        for (int value = 10; value <= 30; value += 10) {
            write.transform = Corona::Network::TransformState{};
            write.transform.xform[0] = static_cast<float>(value);
            expect_true(Corona::Network::submit_remote_state(write),
                        "coalescing write is accepted");
        }
        const auto stats = Corona::Network::sync_boundary_stats();
        expect_true(stats.remote_writes_pending == 1,
                    "same (storage, seq) coalesces to one queued write");
        expect_true(stats.remote_writes_coalesced == 2,
                    "the two superseded writes are counted as coalesced");
    }
    // Completion signal is "applied", not "queue empty": the owner drains (pending → 0)
    // before it applies the writes.
    expect_true(wait_until([] {
                    return Corona::Network::sync_boundary_stats().remote_writes_applied >= 1;
                }),
                "the coalesced write is applied");
    {
        const auto* transform = Corona::find_transform(fixture.transform);
        expect_true(transform != nullptr && transform->position.x == 30.0f,
                    "coalescing keeps the newest value");
    }

    // Cap: 5000 distinct keys -> 4096 queued, 904 oldest evicted.
    std::uint64_t drained_baseline = 0;
    {
        std::lock_guard gate(boundary_gate);
        Corona::Network::reset_sync_boundary_stats();
        constexpr int kSubmitted = 5000;
        for (int i = 0; i < kSubmitted; ++i) {
            write.entity_seq = 1'000'000 + static_cast<std::uint64_t>(i);
            write.transform = Corona::Network::TransformState{};
            write.transform.xform[0] = static_cast<float>(i);
            (void)Corona::Network::submit_remote_state(write);
        }
        const auto stats = Corona::Network::sync_boundary_stats();
        expect_true(stats.remote_writes_submitted == kSubmitted,
                    "every submission is counted");
        expect_true(stats.remote_writes_pending == 4096,
                    "the queue stops at its 4096 cap");
        expect_true(stats.remote_writes_evicted == kSubmitted - 4096,
                    "overflow drops the oldest and counts it");
        expect_true(stats.remote_writes_coalesced == 0,
                    "distinct keys are not coalesced");
        drained_baseline = stats.remote_writes_applied + stats.remote_writes_dropped;
    }
    // Wait until every drained write is *settled* (applied or dropped), not merely
    // swapped out of the queue.
    expect_true(wait_until([drained_baseline] {
                    const auto stats = Corona::Network::sync_boundary_stats();
                    return stats.remote_writes_applied + stats.remote_writes_dropped >=
                           drained_baseline + 4096;
                }),
                "the capped queue drains and settles at the boundary");
    expect_true(Corona::Network::sync_boundary_stats().remote_writes_dropped >= 1,
                "writes to missing entities are dropped (not retried)");

    // Recency: a coalesced key moves to the back, so "drop oldest" evicts the oldest
    // *write*, not the oldest first-insert position. Without the move, the just-refreshed
    // key would sit at the front and the next new key would evict it.
    {
        std::lock_guard gate(boundary_gate);
        Corona::Network::reset_sync_boundary_stats();
        const auto fixture_seq =
            static_cast<std::uint64_t>(Corona::Ecs::seq_of(fixture.transform));
        write.entity_seq = fixture_seq;
        write.transform = Corona::Network::TransformState{};
        write.transform.xform[0] = 5.0f;
        expect_true(Corona::Network::submit_remote_state(write),
                    "oldest write (the fixture) is accepted");
        for (int i = 1; i < 4096; ++i) {
            write.entity_seq = 2'000'000 + static_cast<std::uint64_t>(i);
            write.transform = Corona::Network::TransformState{};
            (void)Corona::Network::submit_remote_state(write);
        }
        // Refresh the oldest key; it must move to the back of the queue.
        write.entity_seq = fixture_seq;
        write.transform = Corona::Network::TransformState{};
        write.transform.xform[0] = 777.0f;
        expect_true(Corona::Network::submit_remote_state(write),
                    "coalesced fixture write is accepted");
        // One more new key: the eviction victim must be the next-oldest write, not the
        // refreshed fixture entry.
        write.entity_seq = 2'000'000 + 4096;
        write.transform = Corona::Network::TransformState{};
        (void)Corona::Network::submit_remote_state(write);
        const auto stats = Corona::Network::sync_boundary_stats();
        expect_true(stats.remote_writes_pending == 4096,
                    "recency test keeps the 4096 cap");
        expect_true(stats.remote_writes_coalesced == 1,
                    "the fixture refresh is counted as a coalesce");
        expect_true(stats.remote_writes_evicted == 1,
                    "the new key evicts exactly one oldest write");
    }
    expect_true(wait_until([] {
                    const auto stats = Corona::Network::sync_boundary_stats();
                    return stats.remote_writes_applied + stats.remote_writes_dropped >= 4096;
                }),
                "the recency queue drains and settles");
    {
        const auto* transform = Corona::find_transform(fixture.transform);
        expect_true(transform != nullptr && transform->position.x == 777.0f,
                    "the refreshed key survives eviction (drop-oldest = oldest write)");
    }

    // Reset: a stopped session must not leave remote state for the next one.
    {
        std::lock_guard gate(boundary_gate);
        Corona::Network::reset_sync_boundary_stats();
        write.entity_seq = static_cast<std::uint64_t>(Corona::Ecs::seq_of(fixture.transform));
        expect_true(Corona::Network::submit_remote_state(write),
                    "write before reset is accepted");
        expect_true(Corona::Network::sync_boundary_stats().remote_writes_pending == 1,
                    "one write is pending before reset");
        Corona::Network::reset_sync_boundary_state();
        expect_true(Corona::Network::sync_boundary_stats().remote_writes_pending == 0,
                    "reset clears the pending remote writes");
    }

    // DAG stop hook: a queued remote write must not survive into a restarted DAG.
    {
        std::lock_guard gate(boundary_gate);
        Corona::Network::reset_sync_boundary_stats();
        write.entity_seq = static_cast<std::uint64_t>(Corona::Ecs::seq_of(fixture.transform));
        expect_true(Corona::Network::submit_remote_state(write),
                    "write before the DAG stop is accepted");
        expect_true(Corona::Network::sync_boundary_stats().remote_writes_pending == 1,
                    "one write is pending before the DAG stop");
        Corona::Ecs::on_dag_thread_stopping();
        expect_true(Corona::Network::sync_boundary_stats().remote_writes_pending == 0,
                    "the DAG stop hook clears the pending remote writes");
    }

    stop.store(true, std::memory_order_release);
    owner_thread.join();
    Corona::Network::reset_sync_boundary_state();
    destroy_fixture(hub, fixture);
}

}  // namespace

int main() {
    // Runs first, while the process is still single-threaded (owner not announced yet).
    test_frame_build_cost_at_realistic_actor_count();
    test_remote_state_is_applied_by_the_owner_thread_and_never_echoed();
    test_remote_write_queue_policy();
    if (g_failed != 0) {
        std::cerr << g_failed << " network sync boundary test(s) failed\n";
        return 1;
    }
    return 0;
}

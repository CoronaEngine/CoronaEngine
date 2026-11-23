// Task 2: the network sync boundary is served by the registry owner thread.
//
// This is a real two-thread harness (unlike the CEF smoke): a dedicated owner thread
// publishes the tick boundary and drains the network boundary queues, while the test
// thread behaves like the network thread (requests an outbound frame, submits remote
// state) and never touches the registry after the owner thread starts.

#include <corona/ecs/access.h>
#include <corona/ecs/structural_commands.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/network/network_identity.h>
#include <corona/systems/network/protocol.h>
#include <corona/systems/network/sync_boundary.h>
#include <corona/systems/network/sync_codec.h>
#include <corona/systems/network/sync_engine.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
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
                std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return predicate();
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

void test_remote_state_is_applied_by_the_owner_thread_and_never_echoed() {
    auto& hub = Corona::SharedDataHub::instance();

    // ---- fixture: single-threaded construction (no owner thread yet) ----
    const auto transform = hub.create_transform();
    const auto model_resource = hub.create_model_resource();
    const auto geometry = hub.create_geometry();
    const auto optics = hub.create_optics();
    const auto environment = hub.create_environment();
    const auto profile = hub.create_profile_component();
    const auto actor = hub.create_actor_component();
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
    {
        auto* g = Corona::find_geometry(geometry);
        g->transform_handle = transform;
        g->model_resource_handle = model_resource;
    }
    {
        auto* o = Corona::find_optics(optics);
        o->geometry_handle = geometry;
    }
    {
        auto* p = hub.try_profile(profile);
        p->geometry_handle = geometry;
        p->optics_handle = optics;
    }
    {
        auto* profiles = Corona::find_profiles(actor);
        expect_true(profiles != nullptr, "actor profile list exists");
        if (profiles != nullptr) {
            profiles->handles.push_back(profile);
        }
    }

    Corona::Network::NetworkIdentityRegistry registry(hub);
    expect_true(registry.register_actor("actor-boundary", actor),
                "identity registration for the sync boundary fixture succeeds");

    // ---- start the registry owner thread (the DAG boundary in production) ----
    Corona::Ecs::add_tick_boundary_task(&Corona::Network::apply_pending_sync_boundary);
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
        });
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
    outgoing.clear();
    Corona::Network::EditorSyncOperation xform_op;
    xform_op.kind = Corona::Network::EditorSyncOperationKind::Upsert;
    xform_op.actor_guid = "actor-boundary";
    xform_op.field_name = "xform";
    Corona::Network::TransformState xform_state{};
    xform_state.xform[0] = 99.0f;
    xform_state.xform[1] = 98.0f;
    xform_state.xform[2] = 97.0f;
    xform_state.xform[6] = 1.0f;
    xform_state.xform[7] = 1.0f;
    xform_state.xform[8] = 1.0f;
    xform_op.value = Corona::Network::encode_transform(xform_state);
    xform_op.version = {100, "remote-peer"};
    expect_true(sync.apply_editor_operation(xform_op) ==
                    Corona::Network::LwwApplyResult::Applied,
                "remote transform operation is accepted");

    Corona::Network::EditorSyncOperation optics_op;
    optics_op.kind = Corona::Network::EditorSyncOperationKind::Upsert;
    optics_op.actor_guid = "actor-boundary";
    optics_op.field_name = "optics";
    Corona::Network::OpticsPacked optics_state{};
    optics_state.metallic = 0.9f;
    optics_state.roughness = 0.1f;
    optics_op.value = Corona::Network::encode_optics(optics_state);
    optics_op.version = {101, "remote-peer"};
    expect_true(sync.apply_editor_operation(optics_op) ==
                    Corona::Network::LwwApplyResult::Applied,
                "remote optics operation is accepted");

    Corona::Network::EditorSyncOperation environment_op;
    environment_op.kind = Corona::Network::EditorSyncOperationKind::Upsert;
    environment_op.actor_guid = "__scene__";
    environment_op.field_name = "environment";
    Corona::Network::EnvironmentState environment_state{};
    environment_state.sun_intensity = 3.0f;
    environment_state.exposure = 2.5f;
    environment_op.value = Corona::Network::encode_environment(environment_state);
    environment_op.version = {102, "remote-peer"};
    expect_true(sync.apply_editor_operation(environment_op) ==
                    Corona::Network::LwwApplyResult::Applied,
                "remote environment operation is accepted");

    expect_true(wait_until([] {
                    return Corona::Network::sync_boundary_stats().remote_writes_applied >= 3;
                }),
                "the owner thread applied the three remote writes");

    // 3) None of the just-applied remote values may be echoed back. Two polls: the first
    //    must already see the applied state (the boundary applies writes before frames).
    outgoing.clear();
    sync.poll_and_sync();
    expect_true(!outgoing_has(outgoing, "actor-boundary", "xform"),
                "a remote transform is not re-broadcast");
    expect_true(!outgoing_has(outgoing, "actor-boundary", "optics"),
                "a remote optics state is not re-broadcast");
    expect_true(!outgoing_has_environment(outgoing),
                "a remote environment state is not re-broadcast");
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
    expect_true(!Corona::Network::sync_boundary_stats().remote_writes_dropped,
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
    hub.destroy_actor_component(actor);
    hub.destroy_profile_component(profile);
    hub.destroy_environment(environment);
    hub.destroy_optics(optics);
    hub.destroy_geometry(geometry);
    hub.destroy_model_resource(model_resource);
    hub.destroy_transform(transform);
}

}  // namespace

int main() {
    test_remote_state_is_applied_by_the_owner_thread_and_never_echoed();
    if (g_failed != 0) {
        std::cerr << g_failed << " network sync boundary test(s) failed\n";
        return 1;
    }
    return 0;
}

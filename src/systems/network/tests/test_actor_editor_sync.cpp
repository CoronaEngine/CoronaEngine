#include <corona/systems/network/actor_editor_sync.h>
#include <corona/systems/network/scoped_bool_override.h>

#include <iostream>

namespace {

#define CORONA_TEST_STRINGIZE_IMPL(value) #value
#define CORONA_TEST_STRINGIZE(value) CORONA_TEST_STRINGIZE_IMPL(value)

int g_failed = 0;

void expect_true(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message;
        ++g_failed;
    }
}

void test_actor_state_packet_becomes_lww_operation() {
    const auto legacy = Corona::Network::build_actor_state_update(
        CORONA_TEST_STRINGIZE(actor-chair),
        CORONA_TEST_STRINGIZE(Scene/main.scene),
        CORONA_TEST_STRINGIZE({name:Chair}));
    Corona::Network::LwwState state(CORONA_TEST_STRINGIZE(peer-a));

    const auto operation = Corona::Network::make_actor_editor_upsert(
        state, CORONA_TEST_STRINGIZE(actor-chair),
        CORONA_TEST_STRINGIZE(actor.state), legacy,
        Corona::Network::MessageType::ACTOR_STATE_UPDATE);

    expect_true(operation.has_value(), __func__);
    expect_true(operation && operation->version.counter == 1 &&
                    operation->version.writer_peer_id ==
                        CORONA_TEST_STRINGIZE(peer-a),
                __func__);
    expect_true(operation &&
                    state.value(CORONA_TEST_STRINGIZE(actor-chair),
                                CORONA_TEST_STRINGIZE(actor.state)) ==
                        operation->value,
                __func__);
}

void test_actor_editor_operation_rebuilds_legacy_packet() {
    const auto legacy = Corona::Network::build_actor_state_update(
        CORONA_TEST_STRINGIZE(actor-chair),
        CORONA_TEST_STRINGIZE(Scene/main.scene),
        CORONA_TEST_STRINGIZE({name:Chair}));
    Corona::Network::LwwState state(CORONA_TEST_STRINGIZE(peer-a));
    const auto operation = Corona::Network::make_actor_editor_upsert(
        state, CORONA_TEST_STRINGIZE(actor-chair),
        CORONA_TEST_STRINGIZE(actor.state), legacy,
        Corona::Network::MessageType::ACTOR_STATE_UPDATE);

    const auto rebuilt = operation
        ? Corona::Network::rebuild_actor_editor_packet(
              *operation, Corona::Network::MessageType::ACTOR_STATE_UPDATE)
        : std::vector<uint8_t>{};

    expect_true(rebuilt == legacy, __func__);
}

void test_actor_transform_packet_round_trips_through_lww_operation() {
    float transform[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    const auto legacy = Corona::Network::build_actor_transform_update(
        CORONA_TEST_STRINGIZE(actor-chair),
        CORONA_TEST_STRINGIZE(Scene/main.scene), transform,
        CORONA_TEST_STRINGIZE(user-a), CORONA_TEST_STRINGIZE(corr-1));
    Corona::Network::LwwState state(CORONA_TEST_STRINGIZE(peer-a));
    const auto operation = Corona::Network::make_actor_editor_upsert(
        state, CORONA_TEST_STRINGIZE(actor-chair),
        CORONA_TEST_STRINGIZE(actor.transform), legacy,
        Corona::Network::MessageType::ACTOR_TRANSFORM_UPDATE);

    const auto rebuilt = operation
        ? Corona::Network::rebuild_actor_editor_packet(
              *operation, Corona::Network::MessageType::ACTOR_TRANSFORM_UPDATE)
        : std::vector<uint8_t>{};
    expect_true(rebuilt == legacy, __func__);
}

void test_actor_editor_upsert_rejects_wrong_legacy_type() {
    const auto legacy = Corona::Network::build_actor_state_update(
        CORONA_TEST_STRINGIZE(actor-chair),
        CORONA_TEST_STRINGIZE(Scene/main.scene), {});
    Corona::Network::LwwState state(CORONA_TEST_STRINGIZE(peer-a));

    expect_true(!Corona::Network::make_actor_editor_upsert(
                    state, CORONA_TEST_STRINGIZE(actor-chair),
                    CORONA_TEST_STRINGIZE(actor.create), legacy,
                    Corona::Network::MessageType::ACTOR_CREATE),
                __func__);
}

void test_scoped_bool_override_restores_previous_value() {
    bool applying = false;
    {
        Corona::Network::ScopedBoolOverride guard(applying, true);
        expect_true(applying, __func__);
    }
    expect_true(!applying, __func__);
}

void test_newer_upsert_clears_actor_tombstone() {
    Corona::Network::LwwState state(CORONA_TEST_STRINGIZE(peer-a));
    expect_true(state.apply_upsert(CORONA_TEST_STRINGIZE(actor-chair),
                                   CORONA_TEST_STRINGIZE(actor.create), {1},
                                   {3, CORONA_TEST_STRINGIZE(peer-a)}) ==
                    Corona::Network::LwwApplyResult::Applied,
                __func__);
    expect_true(state.apply_delete(CORONA_TEST_STRINGIZE(actor-chair),
                                   {4, CORONA_TEST_STRINGIZE(peer-b)}) ==
                    Corona::Network::LwwApplyResult::Applied,
                __func__);
    expect_true(state.apply_upsert(CORONA_TEST_STRINGIZE(actor-chair),
                                   CORONA_TEST_STRINGIZE(actor.create), {2},
                                   {5, CORONA_TEST_STRINGIZE(peer-a)}) ==
                    Corona::Network::LwwApplyResult::Applied,
                __func__);
    expect_true(!state.is_deleted(CORONA_TEST_STRINGIZE(actor-chair)), __func__);
}

}  // namespace

int main() {
    test_actor_state_packet_becomes_lww_operation();
    test_actor_editor_operation_rebuilds_legacy_packet();
    test_actor_transform_packet_round_trips_through_lww_operation();
    test_actor_editor_upsert_rejects_wrong_legacy_type();
    test_scoped_bool_override_restores_previous_value();
    test_newer_upsert_clears_actor_tombstone();
    return g_failed == 0 ? 0 : 1;
}

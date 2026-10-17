#include "ik_runtime.h"

#include <cstdio>
#include <limits>

using namespace Corona::Resource;
using namespace Corona::Systems::MechanicsInternal;

namespace {
int failed = 0;
int passed = 0;

void expect(bool condition, const char* message) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    condition ? ++passed : ++failed;
}

bool near(float a, float b) { return std::abs(a - b) < 1e-5f; }

IkChain foot() {
    IkChain chain;
    chain.id = "left-foot";
    chain.end_node = 2;
    chain.mode = IkChain::Mode::FootPlant;
    chain.enabled = true;
    chain.weight = 1;
    chain.contact_weight_decay = 2;
    return chain;
}

void test_foot_endpoint_contract() {
    SkeletonData skeleton;
    skeleton.nodes.resize(4);
    skeleton.nodes[0].name = "Root";
    skeleton.nodes[0].children = {1};
    skeleton.nodes[1].name = "Knee";
    skeleton.nodes[1].parent = 0;
    skeleton.nodes[1].children = {2, 3};
    skeleton.nodes[2].name = "Foot_End";
    skeleton.nodes[2].parent = 1;
    skeleton.nodes[3].name = "Other_End";
    skeleton.nodes[3].parent = 1;
    expect(is_valid_foot_ik_end_node(skeleton, 2) && is_valid_foot_ik_end_node(skeleton, 3),
           "Foot accepts non-root leaves without requiring skinning weights or name conventions");
    expect(!is_valid_foot_ik_end_node(skeleton, 1) && !is_valid_foot_ik_end_node(skeleton, 0) &&
               !is_valid_foot_ik_end_node(skeleton, -1) && !is_valid_foot_ik_end_node(skeleton, 4),
           "Foot rejects internal joints, the root, unresolved names and out-of-range indices");
    SkeletonData single_root;
    single_root.nodes.resize(1);
    expect(!is_valid_foot_ik_end_node(single_root, 0), "a childless root is not a valid Foot endpoint");

    auto chain = foot();
    chain.end_bone_name = "Foot_End";
    const std::array<float, 3> ground{2, 0, 3};
    advance_ik_activation(chain, ground, 0.1f);
    const auto active_weight = chain.runtime.weight;
    expect(sanitize_foot_ik_endpoint(chain, skeleton) && chain.runtime.target == ground &&
               chain.runtime.weight == active_weight && ik_solver_parameters(chain),
           "valid Foot endpoints retain their active support");

    // This is the same guard used after resolving an archived name and before probing.
    chain.end_bone_name = "Knee";
    chain.end_node = 1;
    chain.runtime.releasing = true;
    chain.runtime.ground_handle = 123;
    chain.runtime.anchor_local = {4, 5, 6};
    expect(!sanitize_foot_ik_endpoint(chain, skeleton) && chain.end_node == -1 &&
               !chain.runtime.has_target && chain.runtime.weight == 0 && !chain.runtime.grounded &&
               !chain.runtime.releasing && chain.runtime.ground_handle == 0 &&
               chain.runtime.anchor_local == std::array<float, 3>{0, 0, 0} && !ik_solver_parameters(chain),
           "a saved or native non-leaf Foot cannot retain support or enter the solver");
    expect(chain.end_bone_name == "Knee" && chain.id == "left-foot" && chain.enabled && chain.weight == 1,
           "invalid Foot runtime is cleared while its user configuration remains editable");

    for (const auto mode : {IkChain::Mode::Contact, IkChain::Mode::LookAt, IkChain::Mode::WeaponAim}) {
        auto other = foot();
        other.mode = mode;
        other.end_node = 1;
        advance_ik_activation(other, ground, 0.1f);
        expect(sanitize_foot_ik_endpoint(other, skeleton) && other.end_node == 1 &&
                   other.runtime.has_target && other.runtime.weight == active_weight,
               "the Foot guard leaves internal-joint Contact and manual modes unchanged");
    }

    auto replaced_model = foot();
    advance_ik_activation(replaced_model, ground, 0.1f);
    skeleton.nodes.emplace_back();
    skeleton.nodes[4].parent = 2;
    skeleton.nodes[2].children = {4};
    expect(!sanitize_foot_ik_endpoint(replaced_model, skeleton) && !ik_solver_parameters(replaced_model),
           "a model change that makes the previous endpoint internal immediately removes Foot influence");
}

void test_activation_contract() {
    auto chain = foot();
    advance_ik_activation(chain, std::nullopt, 1.0f / 60.0f);
    expect(chain.enabled && !chain.runtime.has_target && !ik_solver_parameters(chain),
           "new enabled foot with weight 1 cannot solve toward the default origin before a ground hit");

    const std::array<float, 3> ground{2, 3, 4};
    chain.enabled = false;
    chain.runtime.has_target = true;
    chain.runtime.weight = 1;
    advance_ik_activation(chain, ground, 0.1f);
    expect(!chain.enabled && !chain.runtime.has_target && !ik_solver_parameters(chain),
           "ground hits cannot re-enable a user-disabled foot or retain its previous activation");

    chain.enabled = true;
    chain.weight = 0.25f;
    chain.target = {9, 8, 7};
    for (int frame = 0; frame < 5; ++frame) advance_ik_activation(chain, ground, 0.1f);
    auto solver = ik_solver_parameters(chain);
    expect(solver && near(solver->weight, 0.25f) && solver->target == ground,
           "automatic activation respects the user maximum blend weight");
    expect(chain.weight == 0.25f && chain.target == std::array<float, 3>{9, 8, 7} && chain.enabled,
           "runtime ground following does not overwrite user weight, target, or enabled configuration");

    advance_ik_activation(chain, std::nullopt, 0.1f);
    solver = ik_solver_parameters(chain);
    expect(solver && near(solver->weight, 0.2f) && solver->target == ground && !chain.runtime.grounded,
           "losing contact releases the last valid target once per update");
    for (int frame = 0; frame < 5; ++frame) advance_ik_activation(chain, std::nullopt, 0.1f);
    expect(chain.enabled && !ik_solver_parameters(chain) && !chain.runtime.has_target,
           "completed release clears runtime influence while preserving user enablement");

    chain.weight = 0;
    advance_ik_activation(chain, ground, 0.1f);
    expect(!ik_solver_parameters(chain) && !chain.runtime.has_target,
           "an explicit zero maximum weight remains zero even on valid ground");
}

void test_contact_time_integration() {
    auto chain = foot();
    chain.mode = IkChain::Mode::Contact;
    chain.contact_driven = true;
    const std::array<float, 3> contact{1, 2, 3};
    for (int frame = 0; frame < 6; ++frame) advance_ik_activation(chain, contact, 1.0f / 60.0f);
    auto solver = ik_solver_parameters(chain);
    expect(solver && near(solver->weight, 0.2f),
           "sustained Contact with equal rise/decay rates accumulates influence instead of cancelling it");
    advance_ik_activation(chain, std::nullopt, 1.0f / 60.0f);
    solver = ik_solver_parameters(chain);
    expect(solver && near(solver->weight, 1.0f / 6.0f),
           "a released Contact decays by one frame interval");

    auto one_frame = foot();
    auto ten_frames = foot();
    advance_ik_activation(one_frame, contact, 0.1f);
    for (int frame = 0; frame < 10; ++frame) advance_ik_activation(ten_frames, contact, 0.01f);
    expect(near(one_frame.runtime.weight, ten_frames.runtime.weight),
           "equal elapsed activation time gives equal influence at different animation frame rates");

    auto invalid_target = foot();
    advance_ik_activation(invalid_target,
        std::array<float, 3>{std::numeric_limits<float>::quiet_NaN(), 0, 0}, 0.1f);
    expect(!ik_solver_parameters(invalid_target), "non-finite contacts cannot activate a foot");
}

void test_foot_plant_release_lifecycle() {
    auto chain = foot();
    const std::array<float, 3> old_anchor{1, 0, 2};
    const std::array<float, 3> new_anchor{4, 0, 5};
    expect(!foot_can_plant(chain, true, false, 0.2f) && !chain.runtime.releasing,
           "a high animation foot does not acquire support merely because a surface is within probe reach");
    expect(foot_can_plant(chain, true, false, 0.02f),
           "a low animation foot may acquire its first ground anchor");
    for (int frame = 0; frame < 5; ++frame) advance_ik_activation(chain, old_anchor, 0.1f);
    expect(foot_can_plant(chain, true, true, chain.plant_threshold + 0.02f) && !chain.runtime.releasing,
           "an existing valid anchor remains supported through the entry-threshold hysteresis band");

    auto lost_support = chain;
    expect(!foot_can_plant(lost_support, false, false, 0.0f) && lost_support.runtime.releasing,
           "losing the supporting surface begins release even if the animation foot remains low");
    advance_ik_activation(lost_support, std::nullopt, 0.1f);
    expect(!foot_can_plant(lost_support, true, false, 0.0f) &&
               lost_support.runtime.target == old_anchor && near(lost_support.runtime.weight, 0.8f),
           "a returning surface cannot replace the old target while support-loss influence is fading");

    expect(!foot_can_plant(chain, true, false, 0.0f) && chain.runtime.releasing,
           "excessive anchor drift begins release instead of immediately planting at a new position");
    advance_ik_activation(chain, std::nullopt, 0.1f);
    auto solver = ik_solver_parameters(chain);
    expect(solver && solver->target == old_anchor && near(solver->weight, 0.8f),
           "anchor release retains the previous solver target while reducing influence");
    expect(!foot_can_plant(chain, true, true, 0.0f),
           "an already releasing foot cannot cancel release and snap back to a recovered anchor");

    bool protected_during_fade = true;
    for (int frame = 0; frame < 10 && chain.runtime.has_target; ++frame) {
        protected_during_fade &= !foot_can_plant(chain, true, false, 0.0f);
        protected_during_fade &= chain.runtime.target == old_anchor;
        advance_ik_activation(chain, std::nullopt, 0.1f);
    }
    expect(protected_during_fade && !chain.runtime.has_target && !chain.runtime.releasing && !ik_solver_parameters(chain),
           "release protects the old anchor until zero influence, then clears the replant barrier");
    const bool may_replant = foot_can_plant(chain, true, false, 0.02f);
    advance_ik_activation(chain, may_replant ? std::optional(new_anchor) : std::nullopt, 0.1f);
    solver = ik_solver_parameters(chain);
    expect(may_replant && solver && solver->target == new_anchor && near(solver->weight, 0.2f),
           "a fully released low foot acquires the new anchor with a fresh activation ramp");

    foot_can_plant(chain, false, false, 0.0f);
    chain.enabled = false;
    advance_ik_activation(chain, std::nullopt, 0.1f);
    expect(!chain.runtime.releasing && !chain.runtime.has_target && !ik_solver_parameters(chain),
           "user disable clears both influence and pending release state");
}

void test_runtime_commit() {
    auto chain = foot();
    chain.runtime.target = {1, 2, 3};
    chain.runtime.weight = 0.1f;
    chain.runtime.has_target = true;
    std::vector<IkChain> destination{chain};
    auto snapshot = destination;
    snapshot[0].runtime.target = {8, 9, 10};
    snapshot[0].runtime.weight = 0.75f;
    expect(!commit_ik_runtime(destination, 2, snapshot, 1) && destination[0].runtime.target == chain.runtime.target,
           "an old animation snapshot cannot overwrite a newer editor configuration revision");

    snapshot[0].id = "right-foot";
    expect(!commit_ik_runtime(destination, 2, snapshot, 2) && near(destination[0].runtime.weight, 0.1f),
           "runtime results cannot be committed to a different chain at the same array index");
    snapshot[0].id = chain.id;
    snapshot[0].end_node = 5;
    expect(!commit_ik_runtime(destination, 2, snapshot, 2), "a changed end bone rejects an old runtime result");
    snapshot[0].end_node = chain.end_node;
    snapshot[0].mode = IkChain::Mode::LookAt;
    expect(!commit_ik_runtime(destination, 2, snapshot, 2), "a changed mode rejects an old runtime result");

    snapshot[0].mode = chain.mode;
    snapshot[0].weight = 0.3f;
    snapshot[0].enabled = false;
    expect(commit_ik_runtime(destination, 2, snapshot, 2) && near(destination[0].runtime.weight, 0.75f) &&
               destination[0].weight == chain.weight && destination[0].enabled == chain.enabled,
           "a current runtime commit updates only runtime state, preserving user configuration");
}

void test_bone_position_space() {
    SkeletonData skeleton;
    skeleton.global_inverse = compose_trs({-2, 3, -1}, {0, 0, 0, 1}, {0.5f, 0.5f, 0.5f});
    skeleton.nodes.resize(2);
    skeleton.nodes[0].parent = -1;
    skeleton.nodes[0].children = {1};
    skeleton.nodes[1].parent = 0;
    std::vector<std::array<float, 16>> locals{
        compose_trs({4, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}),
        compose_trs({0, 2, 0}, {0, 0, 0, 1}, {1, 1, 1})};
    const auto position = ik_bone_model_position(skeleton, locals, 1);
    expect(position && *position == std::array<float, 3>{0, 4, -1},
           "ground probe bone origin includes import normalization and the complete parent hierarchy");
    skeleton.nodes[0].parent = 1;
    expect(!ik_bone_model_position(skeleton, locals, 1), "cyclic bone hierarchy terminates without a probe position");
    skeleton.nodes[0].parent = 50;
    expect(!ik_bone_model_position(skeleton, locals, 1), "out-of-range parent cannot produce a ground probe");
}
} // namespace

int main() {
    test_foot_endpoint_contract();
    test_activation_contract();
    test_contact_time_integration();
    test_foot_plant_release_lifecycle();
    test_runtime_commit();
    test_bone_position_space();
    std::printf("IK runtime: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

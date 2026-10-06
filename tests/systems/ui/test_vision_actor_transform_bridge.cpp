#include "cef/vision_actor_transform_bridge.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <string_view>

#include <nlohmann/json.hpp>

namespace {

using Corona::Systems::UI::VisionActorTransformState;
using Corona::Systems::UI::decode_vision_actor_transform;
using Corona::Systems::UI::encode_vision_actor_transform;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

void expect_near(float actual,
                 float expected,
                 std::string_view message,
                 float epsilon = 1.0e-4f) {
    if (std::abs(actual - expected) > epsilon) {
        fail(message);
    }
}

void expect_vec_near(const std::array<float, 3>& actual,
                     const std::array<float, 3>& expected,
                     std::string_view message,
                     float epsilon = 1.0e-4f) {
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (std::abs(actual[index] - expected[index]) > epsilon) {
            fail(message);
        }
    }
}

nlohmann::json matrix_transform(nlohmann::json matrix) {
    return {
        {"type", "matrix4x4"},
        {"param", {{"matrix4x4", std::move(matrix)}}},
    };
}

void matrix4x4_decomposes_absolute_corona_trs() {
    const auto transform = matrix_transform({
        {2.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 3.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 4.0f, 0.0f},
        {1.0f, 2.0f, 3.0f, 1.0f},
    });

    const auto state = decode_vision_actor_transform(transform);

    expect(state.valid, "affine matrix should decode");
    expect(!state.lossy, "orthogonal matrix should decompose without loss");
    expect_vec_near(state.position, {1.0f, 2.0f, -3.0f},
                    "Vision translation should convert to Corona coordinates");
    expect_vec_near(state.rotation, {0.0f, 0.0f, 0.0f},
                    "identity rotation should remain identity");
    expect_vec_near(state.scale, {2.0f, 3.0f, 4.0f},
                    "non-uniform scale should be preserved");
}

void current_cbox_matrix_keeps_real_transform() {
    const auto transform = matrix_transform({
        {-0.17032849788665771, 7.4452954912374025e-09, 0.5699020028114319, 0.0},
        {-0.5790837407112122, 2.531255205440175e-08, -0.17307265102863312, 0.0},
        {-2.622683403785686e-08, -0.5999999642372131, 1.146411342626294e-15, 0.0},
        {0.3286310136318207, 0.2990000001192093, 0.37459200620651245, 1.0},
    });

    const auto state = decode_vision_actor_transform(transform);

    expect(state.valid, "cbox matrix should decode");
    expect(!state.lossy, "cbox matrix should be a clean TRS matrix");
    expect_vec_near(state.position,
                    {0.3286310136f, 0.2990000001f, -0.3745920062f},
                    "cbox translation should not fall back to zero");
    expect_vec_near(state.scale, {0.594808f, 0.604389f, 0.6f},
                    "cbox scale should not fall back to one", 2.0e-4f);
    expect(std::abs(state.rotation[0]) + std::abs(state.rotation[1]) +
               std::abs(state.rotation[2]) > 0.1f,
           "cbox rotation should not fall back to zero");
}

void trs_round_trips_position_rotation_and_scale() {
    VisionActorTransformState original;
    original.position = {1.25f, -2.0f, 3.5f};
    original.rotation = {0.2f, -0.3f, 0.4f};
    original.scale = {2.0f, 3.0f, 4.0f};
    original.valid = true;

    const auto encoded = encode_vision_actor_transform(original);
    const auto decoded = decode_vision_actor_transform(encoded);

    expect(encoded.at("type") == "trs", "native edits should write Vision trs");
    expect(encoded.at("param").contains("t"), "Vision trs should include translation");
    expect(encoded.at("param").contains("r"), "Vision trs should include quaternion rotation");
    expect(encoded.at("param").at("r").size() == 4,
           "Vision rotation should be axis-angle float4");
    expect(encoded.at("param").contains("s"), "Vision trs should include scale");
    expect(decoded.valid, "encoded trs should decode");
    expect_vec_near(decoded.position, original.position, "position should round trip");
    expect_vec_near(decoded.rotation, original.rotation, "rotation should round trip");
    expect_vec_near(decoded.scale, original.scale, "scale should round trip");
}

void rotation_uses_vision_axis_angle_contract() {
    VisionActorTransformState state;
    state.rotation = {0.0f, 0.0f, std::numbers::pi_v<float> / 2.0f};
    state.valid = true;

    const auto encoded = encode_vision_actor_transform(state);
    const auto& rotation = encoded.at("param").at("r");

    expect_near(rotation.at(0).get<float>(), 0.0f,
                "Z rotation should have zero X axis");
    expect_near(rotation.at(1).get<float>(), 0.0f,
                "Z rotation should have zero Y axis");
    expect_near(rotation.at(2).get<float>(), 1.0f,
                "Z rotation should preserve its Vision axis");
    expect_near(rotation.at(3).get<float>(), 90.0f,
                "Vision axis-angle stores degrees in r.w");
}

void euler_and_look_at_transforms_decode() {
    const nlohmann::json euler = {
        {"type", "Euler"},
        {"param", {{"position", {1.0f, 2.0f, 3.0f}}, {"yaw", 90.0f}}},
    };
    const auto euler_state = decode_vision_actor_transform(euler);
    expect(euler_state.valid, "Vision Euler transform should decode");
    expect_vec_near(euler_state.position, {1.0f, 2.0f, -3.0f},
                    "Euler position should convert coordinates");
    expect_near(euler_state.rotation[1], -std::numbers::pi_v<float> / 2.0f,
                "Vision yaw should convert to Corona yaw", 2.0e-4f);

    const nlohmann::json look_at = {
        {"type", "look_at"},
        {"param",
         {{"position", {0.0f, 1.0f, 3.0f}},
          {"target_pos", {0.0f, 1.0f, 0.0f}},
          {"up", {0.0f, 1.0f, 0.0f}}}},
    };
    const auto look_at_state = decode_vision_actor_transform(look_at);
    expect(look_at_state.valid, "Vision look_at transform should decode");
    expect_vec_near(look_at_state.position, {0.0f, 1.0f, -3.0f},
                    "look_at position should convert coordinates");
    expect_vec_near(look_at_state.rotation, {0.0f, 0.0f, 0.0f},
                    "forward-facing look_at should preserve its basis rotation");
    expect_vec_near(look_at_state.scale, {1.0f, 1.0f, -1.0f},
                    "Vision look_at forward basis should preserve its reflection");
}

void negative_scale_shear_and_invalid_matrices_are_classified() {
    const auto reflected = decode_vision_actor_transform(matrix_transform({
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 2.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, -3.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
    }));
    expect(reflected.valid, "reflected affine matrix should decode");
    expect_near(reflected.scale[2], -3.0f,
                "reflection should use a deterministic negative Z scale");

    const auto sheared = decode_vision_actor_transform(matrix_transform({
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.25f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
    }));
    expect(sheared.valid, "sheared affine matrix should provide a closest TRS");
    expect(sheared.lossy, "sheared matrix should be marked lossy");

    const auto invalid = decode_vision_actor_transform(matrix_transform({
        {1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
    }));
    expect(!invalid.valid, "perspective matrix should not overwrite native transform");
}

void decoding_does_not_rewrite_source_matrix() {
    const auto transform = matrix_transform({
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {4.0f, 5.0f, 6.0f, 1.0f},
    });
    const auto before = transform;

    (void)decode_vision_actor_transform(transform);

    expect(transform == before, "loading should preserve the exact source matrix");
}

}  // namespace

int main() {
    matrix4x4_decomposes_absolute_corona_trs();
    current_cbox_matrix_keeps_real_transform();
    trs_round_trips_position_rotation_and_scale();
    rotation_uses_vision_axis_angle_contract();
    euler_and_look_at_transforms_decode();
    negative_scale_shear_and_invalid_matrices_are_classified();
    decoding_does_not_rewrite_source_matrix();
    std::cout << "Vision actor transform bridge tests passed\n";
    return 0;
}

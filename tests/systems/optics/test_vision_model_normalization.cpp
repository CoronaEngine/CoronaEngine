#include "render_core/shape/model_normalization.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using ocarina::Vertex;
using vision::normalize_model_vertices_to_unit_bounds;

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
                 float epsilon = 1.0e-5f) {
    if (std::abs(actual - expected) > epsilon) {
        fail(message);
    }
}

Vertex vertex(float x, float y, float z) {
    return Vertex(ocarina::make_float3(x, y, z),
                  ocarina::make_float3(0.0f, 1.0f, 0.0f),
                  ocarina::make_float2(0.0f));
}

void offset_mesh_is_centered_and_scaled_to_unit_bounds() {
    std::vector<Vertex> vertices{
        vertex(-2.0f, -1.0f, -1.0f),
        vertex(0.0f, 1.0f, 1.0f),
    };
    std::vector<std::vector<Vertex>*> meshes{&vertices};

    const auto result = normalize_model_vertices_to_unit_bounds(meshes, true);

    expect(result.valid, "non-degenerate model should normalize");
    expect_near(result.center.x, -1.0f, "normalization should report global center X");
    expect_near(result.center.y, 0.0f, "normalization should report global center Y");
    expect_near(result.center.z, 0.0f, "normalization should report global center Z");
    expect_near(result.scale_factor, 0.5f,
                "normalization should use inverse maximum extent");
    expect_near(vertices[0].position().x, -0.5f,
                "minimum X should be centered and normalized");
    expect_near(vertices[1].position().x, 0.5f,
                "maximum X should be centered and normalized");
    expect_near(vertices[0].position().y, -0.5f,
                "minimum Y should share the same uniform scale");
    expect_near(vertices[1].position().z, 0.5f,
                "maximum Z should share the same uniform scale");
}

void multiple_meshes_share_one_global_normalization() {
    std::vector<Vertex> left{vertex(-10.0f, 0.0f, 0.0f)};
    std::vector<Vertex> right{vertex(6.0f, 2.0f, 0.0f)};
    std::vector<std::vector<Vertex>*> meshes{&left, &right};

    const auto result = normalize_model_vertices_to_unit_bounds(meshes, true);

    expect(result.valid, "multi-mesh model should normalize");
    expect_near(result.center.x, -2.0f, "global center should span every mesh");
    expect_near(result.scale_factor, 1.0f / 16.0f,
                "global maximum extent should set one uniform scale");
    expect_near(left.front().position().x, -0.5f,
                "left mesh should retain its global relative position");
    expect_near(right.front().position().x, 0.5f,
                "right mesh should retain its global relative position");
    expect_near(right.front().position().y, 0.0625f,
                "smaller axes should use the global uniform scale");
}

void disabled_normalization_preserves_source_vertices() {
    std::vector<Vertex> vertices{vertex(-2.0f, 3.0f, 4.0f)};
    const auto original = vertices.front().position();
    std::vector<std::vector<Vertex>*> meshes{&vertices};

    const auto result = normalize_model_vertices_to_unit_bounds(meshes, false);

    expect(!result.valid, "disabled normalization should not report an applied transform");
    expect_near(vertices.front().position().x, original.x,
                "disabled normalization should preserve X");
    expect_near(vertices.front().position().y, original.y,
                "disabled normalization should preserve Y");
    expect_near(vertices.front().position().z, original.z,
                "disabled normalization should preserve Z");
}

void empty_and_zero_extent_models_are_unchanged() {
    std::vector<Vertex> empty;
    std::vector<std::vector<Vertex>*> empty_meshes{&empty, nullptr};
    expect(!normalize_model_vertices_to_unit_bounds(empty_meshes, true).valid,
           "empty model should not normalize");

    std::vector<Vertex> point{vertex(4.0f, 5.0f, 6.0f)};
    const auto original = point.front().position();
    std::vector<std::vector<Vertex>*> point_meshes{&point};
    expect(!normalize_model_vertices_to_unit_bounds(point_meshes, true).valid,
           "zero-extent model should not normalize");
    expect_near(point.front().position().x, original.x,
                "zero-extent model should remain unchanged");
}

}  // namespace

int main() {
    offset_mesh_is_centered_and_scaled_to_unit_bounds();
    multiple_meshes_share_one_global_normalization();
    disabled_normalization_preserves_source_vertices();
    empty_and_zero_extent_models_are_unchanged();
    std::cout << "Vision model normalization tests passed\n";
    return 0;
}

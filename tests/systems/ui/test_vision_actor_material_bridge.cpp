#include "cef/vision_actor_material_bridge.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include <nlohmann/json.hpp>

namespace {

using Corona::Systems::UI::VisionActorMaterialState;
using Corona::Systems::UI::bind_vision_actor_material;
using Corona::Systems::UI::ensure_native_actor_model_normalization;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

void expect_near(float actual, float expected, std::string_view message) {
    if (std::abs(actual - expected) > 1.0e-6f) {
        fail(message);
    }
}

void imported_actor_gets_a_principled_material_binding() {
    nlohmann::json scene_data = {{"materials", nlohmann::json::array()}};
    nlohmann::json shape = {
        {"type", "model"},
        {"param", {{"fn", "models/Ball.obj"}}},
    };
    VisionActorMaterialState state;
    state.diffuse = {0.2f, 0.4f, 0.6f};
    state.metallic = 0.75f;
    state.roughness = 0.25f;

    const std::string material_name =
        bind_vision_actor_material(scene_data, shape, "native-ball-guid", state);

    expect(material_name == "corona_actor_material_native-ball-guid",
           "material name should be stable per actor identity");
    expect(shape.at("param").at("material") == material_name,
           "model shape should reference the generated material");
    expect(scene_data.at("materials").size() == 1,
           "generated material should be added to the Vision scene");

    const auto& material = scene_data.at("materials").front();
    expect(material.at("name") == material_name,
           "material registry name should match the shape binding");
    expect(material.at("type") == "principled_bsdf",
           "actor material should use Vision principled_bsdf");
    const auto& params = material.at("param");
    expect_near(params.at("color").at(0).get<float>(), 0.2f,
                "red diffuse channel should be preserved");
    expect_near(params.at("color").at(1).get<float>(), 0.4f,
                "green diffuse channel should be preserved");
    expect_near(params.at("color").at(2).get<float>(), 0.6f,
                "blue diffuse channel should be preserved");
    expect_near(params.at("metallic").get<float>(), 0.75f,
                "metallic should be preserved");
    expect_near(params.at("roughness").get<float>(), 0.25f,
                "roughness should be preserved");
}

void actor_material_values_are_clamped_and_upserted() {
    nlohmann::json scene_data = {{"materials", nlohmann::json::array()}};
    nlohmann::json shape = {{"param", nlohmann::json::object()}};
    VisionActorMaterialState initial;
    const auto initial_name = bind_vision_actor_material(scene_data, shape, "same-guid", initial);

    VisionActorMaterialState updated;
    updated.diffuse = {-1.0f, 0.5f, 2.0f};
    updated.metallic = 4.0f;
    updated.roughness = 0.0f;
    const auto updated_name = bind_vision_actor_material(scene_data, shape, "same-guid", updated);

    expect(initial_name == updated_name,
           "same actor identity should keep the same material name");

    expect(scene_data.at("materials").size() == 1,
           "rebinding the same actor should update rather than duplicate material");
    const auto& params = scene_data.at("materials").front().at("param");
    expect_near(params.at("color").at(0).get<float>(), 0.0f,
                "diffuse should clamp to zero");
    expect_near(params.at("color").at(2).get<float>(), 1.0f,
                "diffuse should clamp to one");
    expect_near(params.at("metallic").get<float>(), 1.0f,
                "metallic should clamp to one");
    expect_near(params.at("roughness").get<float>(), 0.0001f,
                "roughness should respect Vision's nonzero lower bound");
}

void native_actor_models_are_marked_for_unit_bounds_normalization() {
    nlohmann::json newly_created = {
        {"type", "model"},
        {"guid", "custom-guid"},
        {"param", {{"fn", "Ball.obj"}}},
    };
    expect(ensure_native_actor_model_normalization(newly_created, true),
           "explicit native origin should add normalization marker");
    expect(newly_created.at("param").at("normalize_to_unit_bounds") == true,
           "native model should opt into Vision unit-bounds normalization");
    expect(!ensure_native_actor_model_normalization(newly_created, true),
           "normalization migration should be idempotent");

    nlohmann::json legacy_guid = {
        {"type", "model"},
        {"guid", "native-legacy-guid"},
        {"param", {{"fn", "Ball.obj"}}},
    };
    expect(ensure_native_actor_model_normalization(legacy_guid),
           "legacy native GUID should be migrated");

    nlohmann::json legacy_material = {
        {"type", "model"},
        {"guid", "older-guid"},
        {"param",
         {{"fn", "Ball.obj"},
          {"material", "corona_actor_material_older-guid"}}},
    };
    expect(ensure_native_actor_model_normalization(legacy_material),
           "generated actor material should identify legacy native model");
}

void original_vision_shapes_are_not_normalized() {
    nlohmann::json authored_model = {
        {"type", "model"},
        {"guid", "vision-shape-3"},
        {"param", {{"fn", "authored.obj"}, {"material", "AuthoredMaterial"}}},
    };
    expect(!ensure_native_actor_model_normalization(authored_model),
           "authored Vision model should preserve source pivot and units");
    expect(!authored_model.at("param").contains("normalize_to_unit_bounds"),
           "authored Vision model should remain unchanged");

    nlohmann::json bridged_vision_model = {
        {"type", "model"},
        {"guid", "vision-shape-9"},
        {"param",
         {{"fn", "authored.obj"},
          {"material", "corona_actor_material_vision-shape-9"}}},
    };
    expect(!ensure_native_actor_model_normalization(bridged_vision_model),
           "vision GUID should override a generated fallback material marker");

    nlohmann::json native_primitive = {
        {"type", "sphere"},
        {"guid", "native-sphere"},
        {"param", nlohmann::json::object()},
    };
    expect(!ensure_native_actor_model_normalization(native_primitive, true),
           "normalization marker should apply only to model shapes");
}

}  // namespace

int main() {
    imported_actor_gets_a_principled_material_binding();
    actor_material_values_are_clamped_and_upserted();
    native_actor_models_are_marked_for_unit_bounds_normalization();
    original_vision_shapes_are_not_normalized();
    std::cout << "Vision actor material bridge tests passed\n";
    return 0;
}

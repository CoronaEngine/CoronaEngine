#include "vision_medium_adapter.h"

#include <corona/shared_data_hub.h>
#include <nlohmann/json.hpp>
#include <memory>
#include <stdexcept>

#include "base/import/project_desc.h"
#include "base/mgr/scene.h"
#include "base/shape.h"

namespace Corona::Systems::Vision {
namespace {

nlohmann::json vision_medium_descriptor_json(const nlohmann::json& source) {
    if (source.at("type") != "homogeneous") {
        throw std::invalid_argument("Unsupported Vision medium type");
    }
    // HomogeneousMedium applies scale when evaluating its coefficients. Keep
    // source sigma_a/sigma_s unscaled here so density is multiplied only once.
    return {{"type", "homogeneous"}, {"name", source.at("id")}, {"param", {
        {"sigma_a", source.at("sigma_a")}, {"sigma_s", source.at("sigma_s")},
        {"scale", source.at("scale")}, {"g", source.at("g")}}}};
}

::vision::float4x4 vision_medium_boundary_transform(const nlohmann::json& source) {
    const auto& values = source.at("object_to_world");
    if (!values.is_array() || values.size() != 16) {
        throw std::invalid_argument("Medium boundary requires 16 row-major transform values");
    }
    auto matrix = ::vision::make_float4x4(1.f);
    for (unsigned column = 0; column < 4; ++column) {
        for (unsigned row = 0; row < 4; ++row) {
            // These are source/Vision coordinates, independent of Corona's
            // surface adapter conversion. In particular, do not reflect Z.
            matrix[column][row] = values[row * 4 + column].get<float>();
        }
    }
    return matrix;
}

std::shared_ptr<::vision::Medium> resolve_vision_medium(::vision::Scene& scene,
    const nlohmann::json& reference) {
    if (reference.is_null()) return nullptr;
    const auto name = reference.get<std::string>();
    for (const auto& medium : scene.mediums()) {
        if (medium->name() == name) return medium;
    }
    throw std::invalid_argument("Unknown Vision medium reference: " + name);
}

unsigned vision_medium_id(::vision::Scene& scene, const std::shared_ptr<::vision::Medium>& medium) {
    return medium ? scene.mediums().encode_id(medium->index(), medium.get()) : ::vision::InvalidUI32;
}

}  // namespace

VisionMediaTelemetry configure_vision_media(::vision::Scene& scene,
    const nlohmann::json* media, bool enabled) {
    if (!scene.geometry().has_gpu_resource()) {
        throw std::logic_error("Medium configuration requires a private Geometry GPU resource");
    }
    auto& registry = scene.medium_registry();
    registry.elements().clear();
    registry.global_medium().reset();
    registry.global_medium().name.clear();
    registry.set_process_mediums(enabled);
    ::vision::MediumsDesc descriptions;
    descriptions.process = enabled;
    if (enabled && media) {
        if (media->at("schema") != 1) throw std::invalid_argument("Unsupported scene.media schema");
        for (const auto& source : media->at("mediums")) {
            ::vision::MediumDesc description;
            description.init(::vision::ParameterSet{
                ::vision::DataWrap::parse(vision_medium_descriptor_json(source).dump())});
            description.name = source.at("id").get<std::string>();
            descriptions.mediums.push_back(std::move(description));
        }
    }
    scene.load_mediums(descriptions);
    scene.geometry().set_process_mediums(scene.process_mediums());
    ::vision::vector<::vision::MediumBoundary> boundaries;
    if (enabled && media) {
        for (const auto& source : media->at("boundaries")) {
            if (source.at("shape") != "cube") {
                throw std::invalid_argument("Unsupported Vision medium boundary shape");
            }
            boundaries.push_back({vision_medium_boundary_transform(source),
                vision_medium_id(scene, resolve_vision_medium(scene, source.at("inside"))),
                vision_medium_id(scene, resolve_vision_medium(scene, source.at("outside")))});
        }
    }
    scene.geometry().set_medium_boundaries(boundaries);
    return {scene.mediums().size(), scene.geometry().boundary_count(), scene.process_mediums()};
}

void attach_vision_medium_interface(::vision::Scene& scene,
    ::vision::ShapeInstance& instance, const OpticsDevice& optics) {
    if (!scene.process_mediums() || !optics.medium_interface) return;
    const auto& interface_data = *optics.medium_interface;
    auto inside = resolve_vision_medium(scene, interface_data.at("inside"));
    auto outside = resolve_vision_medium(scene, interface_data.at("outside"));
    instance.set_inside(inside);
    instance.set_outside(outside);
    instance.set_inside_name(inside ? inside->name() : "");
    instance.set_outside_name(outside ? outside->name() : "");
    instance.update_inside_medium_id(vision_medium_id(scene, inside));
    instance.update_outside_medium_id(vision_medium_id(scene, outside));
}

}  // namespace Corona::Systems::Vision

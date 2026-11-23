#include "editor_scene_media.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace Corona::Systems::UI {

void validate_authored_medium_interface(
    const nlohmann::json& value,
    const std::unordered_set<std::string>& medium_ids) {
    if (!value.is_object() || !value.contains("inside") || !value.contains("outside")) {
        throw std::invalid_argument("Medium interface requires inside and outside references");
    }
    for (const auto* side : {"inside", "outside"}) {
        const auto& reference = value.at(side);
        if (reference.is_null()) continue;
        if (!reference.is_string() || !medium_ids.contains(reference.get<std::string>())) {
            throw std::invalid_argument(std::string{"Unknown medium reference: "} + side);
        }
    }
}

std::unordered_set<std::string> validate_authored_media(const nlohmann::json& media) {
    if (!media.is_object() || !media.contains("schema") ||
        !media.at("schema").is_number_integer() || media.at("schema") != 1 ||
        !media.contains("mediums") || !media.at("mediums").is_array() ||
        !media.contains("boundaries") || !media.at("boundaries").is_array()) {
        throw std::invalid_argument("Invalid scene.media schema");
    }
    constexpr std::size_t kMaxMediaItems = 100000;
    if (media.at("mediums").size() > kMaxMediaItems ||
        media.at("boundaries").size() > kMaxMediaItems) {
        throw std::invalid_argument("scene.media exceeds the item safety limit");
    }
    const auto finite_float = [](const nlohmann::json& value) {
        return value.is_number() && std::isfinite(value.get<double>()) &&
               std::abs(value.get<double>()) <= std::numeric_limits<float>::max();
    };
    const auto nonnegative_float = [&](const nlohmann::json& value) {
        return finite_float(value) && value.get<double>() >= 0.0;
    };
    std::unordered_set<std::string> medium_ids;
    for (const auto& medium : media.at("mediums")) {
        if (!medium.is_object() || !medium.contains("id") || !medium.at("id").is_string() ||
            medium.at("id").get<std::string>().empty() ||
            !medium_ids.insert(medium.at("id").get<std::string>()).second) {
            throw std::invalid_argument("Medium requires a unique non-empty id");
        }
        if (!medium.contains("type") || medium.at("type") != "homogeneous") {
            throw std::invalid_argument("Unsupported medium type; expected homogeneous");
        }
        if (!medium.contains("scale") || !nonnegative_float(medium.at("scale")) ||
            !medium.contains("g") || !finite_float(medium.at("g")) ||
            !std::isfinite(medium.at("g").get<float>()) ||
            std::abs(medium.at("g").get<float>()) >= 1.0f) {
            throw std::invalid_argument("Medium requires nonnegative finite scale and -1 < g < 1");
        }
        for (const auto* name : {"sigma_a", "sigma_s"}) {
            if (!medium.contains(name) || !medium.at(name).is_array() || medium.at(name).size() != 3) {
                throw std::invalid_argument(std::string{name} + " requires three coefficients");
            }
            for (const auto& coefficient : medium.at(name)) {
                if (!nonnegative_float(coefficient) ||
                    coefficient.get<double>() * medium.at("scale").get<double>() >
                        std::numeric_limits<float>::max()) {
                    throw std::invalid_argument(std::string{name} + " requires finite nonnegative coefficients");
                }
            }
        }
        // Match HomogeneousMedium's float arithmetic: addition precedes scale.
        const float scale = medium.at("scale").get<float>();
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float extinction = medium.at("sigma_a")[channel].get<float>() +
                                     medium.at("sigma_s")[channel].get<float>();
            const float scaled_extinction = extinction * scale;
            if (!std::isfinite(extinction) || !std::isfinite(scaled_extinction)) {
                throw std::invalid_argument("Medium extinction must remain finite in float precision");
            }
        }
    }
    std::unordered_set<std::string> boundary_ids;
    for (const auto& boundary : media.at("boundaries")) {
        if (!boundary.is_object() || !boundary.contains("id") || !boundary.at("id").is_string() ||
            boundary.at("id").get<std::string>().empty() ||
            !boundary_ids.insert(boundary.at("id").get<std::string>()).second) {
            throw std::invalid_argument("Medium boundary requires a unique non-empty id");
        }
        if (!boundary.contains("shape") || boundary.at("shape") != "cube") {
            throw std::invalid_argument("Unsupported medium boundary shape; expected cube");
        }
        validate_authored_medium_interface(boundary, medium_ids);
        if (!boundary.contains("object_to_world") || !boundary.at("object_to_world").is_array() ||
            boundary.at("object_to_world").size() != 16) {
            throw std::invalid_argument("Medium boundary requires a row-major matrix with 16 values");
        }
        const auto& matrix = boundary.at("object_to_world");
        for (const auto& value : matrix) {
            if (!finite_float(value)) throw std::invalid_argument("Medium boundary matrix must be finite");
        }
        for (const auto index : {12, 13, 14, 15}) {
            if (std::abs(matrix[index].get<double>() - (index == 15 ? 1.0 : 0.0)) > 1e-6) {
                throw std::invalid_argument("Medium boundary row-major matrix must be affine");
            }
        }
        const auto m = [&](int row, int column) { return matrix[row * 4 + column].get<double>(); };
        const auto determinant =
            m(0, 0) * (m(1, 1) * m(2, 2) - m(1, 2) * m(2, 1)) -
            m(0, 1) * (m(1, 0) * m(2, 2) - m(1, 2) * m(2, 0)) +
            m(0, 2) * (m(1, 0) * m(2, 1) - m(1, 1) * m(2, 0));
        if (!std::isfinite(determinant) || determinant == 0.0) {
            throw std::invalid_argument("Medium boundary matrix must be invertible");
        }
    }
    return medium_ids;
}

}  // namespace Corona::Systems::UI

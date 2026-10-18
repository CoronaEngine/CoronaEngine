#pragma once

#include <cmath>
#include <map>
#include <set>
#include <string>

namespace Corona::Systems::Vision {

template<typename Json>
inline std::string validate_restir_settings(const Json& settings,
                                          const std::string& path = {}) {
    if (!settings.is_object()) return "Expected ReSTIR settings object: " + path;
    struct NumberRange {
        double minimum;
        double maximum;
        bool integer{false};
    };
    static const std::map<std::string, NumberRange> ranges{
        {"max_depth", {0, 128, true}}, {"min_depth", {0, 20, true}}, {"rr_threshold", {0, 1}},
        {"direct.M_light", {0, 100, true}}, {"direct.M_bsdf", {0, 100, true}},
        {"direct.max_age", {0, 100, true}}, {"direct.max_recursion", {1, 100, true}},
        {"direct.diffuse_factor", {0, 1}}, {"indirect.max_age", {0, 100, true}},
        {"indirect.diffuse_factor", {0, 1}},
        {"direct.temporal.history_limit", {0, 50, true}},
        {"direct.temporal.theta", {0, 90}}, {"direct.temporal.depth", {0, 1}},
        {"direct.temporal.radius", {0, 50}},
        {"direct.spatial.theta", {0, 90}}, {"direct.spatial.depth", {0, 1}},
        {"direct.spatial.radius", {0, 50}},
        {"indirect.temporal.history_limit", {0, 50, true}},
        {"indirect.temporal.theta", {0, 90}}, {"indirect.temporal.depth", {0, 1}},
        {"indirect.temporal.radius", {0, 50}},
        {"indirect.spatial.theta", {0, 90}}, {"indirect.spatial.depth", {0, 1}},
        {"indirect.spatial.radius", {0, 50}},
    };
    static const std::set<std::string> objects{
        "direct", "indirect", "direct.temporal", "direct.spatial",
        "indirect.temporal", "indirect.spatial",
    };
    static const std::set<std::string> booleans{
        "direct.open", "direct.pairwise", "direct.temporal.open", "direct.spatial.open",
        "indirect.open", "indirect.temporal.open", "indirect.spatial.open",
    };
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        if (it.key().find('.') != std::string::npos) return "Unknown ReSTIR parameter: " + it.key();
        const auto field = path.empty() ? it.key() : path + "." + it.key();
        if (objects.count(field)) {
            if (const auto error = validate_restir_settings(*it, field); !error.empty()) return error;
        } else if (booleans.count(field)) {
            if (!it->is_boolean()) return "Expected ReSTIR boolean: " + field;
        } else if (const auto range = ranges.find(field); range != ranges.end()) {
            if (!it->is_number() || (range->second.integer && !it->is_number_integer()))
                return "Invalid ReSTIR parameter type: " + field;
            const double value = it->template get<double>();
            if (!std::isfinite(value) || value < range->second.minimum || value > range->second.maximum)
                return "ReSTIR parameter out of range: " + field;
        } else {
            return "Unknown ReSTIR parameter: " + field;
        }
    }
    return {};
}

}  // namespace Corona::Systems::Vision

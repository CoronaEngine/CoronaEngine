#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace Corona::Systems::UI {

inline bool archive_diagnostics_require_confirmation(const nlohmann::json& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        // Material conversion notes do not prevent an asset from loading.
        // Older/unclassified diagnostics still require the user's decision.
        if (diagnostic.value("severity", std::string{"error"}) == "error" ||
            diagnostic.value("requires_confirmation", true)) {
            return true;
        }
    }
    return false;
}

}  // namespace Corona::Systems::UI

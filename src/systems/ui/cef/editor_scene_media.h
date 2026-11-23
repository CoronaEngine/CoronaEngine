#pragma once

// Authored participating-media values (`scene.media`) and the per-actor medium
// interface references that point into it.
//
// Validation only: these helpers never rescale or rewrite the authored numbers,
// so the only way a document fails is by being rejected outright.

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_set>

namespace Corona::Systems::UI {

/// Validate one `inside`/`outside` reference pair against the medium identity set.
/// An empty set still accepts explicit nulls (vacuum) but rejects named references.
void validate_authored_medium_interface(const nlohmann::json& value,
                                        const std::unordered_set<std::string>& medium_ids);

/// Validate the whole `scene.media` document and return the medium identity set.
/// Throws `std::invalid_argument` on anything the Vision medium adapter cannot honour.
std::unordered_set<std::string> validate_authored_media(const nlohmann::json& media);

}  // namespace Corona::Systems::UI

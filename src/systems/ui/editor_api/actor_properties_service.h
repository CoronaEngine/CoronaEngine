#pragma once

#include "cef/cef_scene_operations.h"

#include <string>
#include <vector>

namespace Corona::Systems::UI::ActorPropertiesService {

struct ValidationError {
    std::string code;
    std::string message;
    explicit operator bool() const { return !code.empty(); }
};

// Validates the typed updateActor patch before any component write or asset
// import: identity/patch emptiness, mechanics finiteness and body type, camera
// lock offsets. A mixed invalid patch must never partly apply.
ValidationError validate_patch(const ActorPropertiesPatch& patch);

// Validates a requested rename: read-only scenes reject first, then empty
// trimmed names, then names already used by another Actor.
ValidationError validate_rename(const std::string& requested_name,
                                bool read_only,
                                const std::vector<std::string>& other_names);

// Trimmed rename value the caller applies after validation.
std::string normalized_name(const std::string& requested_name);

}  // namespace Corona::Systems::UI::ActorPropertiesService

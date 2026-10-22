#pragma once
#include <cstdint>

namespace Corona::Resource {
// Immutable request options. The default keeps editor model import behavior.
// External scenes use original coordinates, no materials, normalization, LOD
// or simplification. Options are part of the cache identity, including reloads.
struct ModelImportOptions {
    bool scene_geometry = false;
    bool flip_uvs = false;
    bool flip_normals = false;
    bool face_normals = false;

    [[nodiscard]] std::uint32_t cache_key() const {
        return scene_geometry ? 1u | (flip_uvs ? 2u : 0u) |
            (flip_normals ? 4u : 0u) | (face_normals ? 8u : 0u) : 0u;
    }
};
}

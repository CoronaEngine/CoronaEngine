#include <corona/shared_data_hub.h>

#include <cstdlib>
#include <iostream>

#include "base/scattering/material.h"
#include "vision/vision_material_adapter.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::uint64_t material_hash(const Corona::OpticsDevice& optics) {
    Corona::MeshDevice mesh;
    mesh.materialColor = {0.25f, 0.5f, 0.75f, 1.0f};

    auto material = Corona::Systems::Vision::create_vision_material(optics, mesh);
    require(static_cast<bool>(material), "create_vision_material returned null");
    return material->hash();
}

}  // namespace

int main() {
    Corona::OpticsDevice base;
    base.metallic = 0.2f;
    base.roughness = 0.4f;

    Corona::OpticsDevice extended = base;
    extended.subsurface = 0.35f;
    extended.anisotropic = 0.45f;
    extended.sheen = 0.55f;
    extended.sheenTint = 0.65f;
    extended.clearcoat = 0.75f;
    extended.clearcoatGloss = 0.25f;

    require(material_hash(base) != material_hash(extended),
            "extended Corona optics fields did not affect Vision material hash");

    Corona::OpticsDevice clamped = extended;
    clamped.subsurface = 5.0f;
    clamped.anisotropic = 5.0f;
    clamped.sheen = 5.0f;
    clamped.sheenTint = 5.0f;
    clamped.clearcoat = 5.0f;
    clamped.clearcoatGloss = -5.0f;

    require(material_hash(clamped) != 0,
            "clamped extended Corona optics fields produced an invalid material hash");

    return 0;
}

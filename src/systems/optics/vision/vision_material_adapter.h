#pragma once

// Vision material adapter - maps CoronaEngine OpticsDevice + MeshDevice material
// parameters to a Vision PrincipledBSDF material.

#include <array>
#include <memory>

// Forward declarations
namespace vision {
class Material;
class Scene;
class ShapeInstance;
}  // namespace vision

namespace Corona {
struct OpticsDevice;
struct MeshDevice;
}  // namespace Corona

namespace Corona::Systems::Vision {

// Maps a single CoronaEngine material to a Vision Material (PrincipledBSDF).
// Mapping rules:
//   MeshDevice::materialColor[0..2] -> color
//   OpticsDevice::roughness          -> roughness
//   OpticsDevice::metallic           -> metallic
//   OpticsDevice::subsurface         -> subsurface_weight
//   OpticsDevice::anisotropic        -> anisotropic
//   OpticsDevice::sheen              -> sheen_weight
//   OpticsDevice::sheenTint          -> sheen_tint (grayscale approximation)
//   OpticsDevice::clearcoat          -> coat_weight
//   OpticsDevice::clearcoatGloss     -> coat_roughness (inverse approximation)
//   unsupported Corona fields        -> Vision defaults
//
// Returns nullptr on failure.
std::shared_ptr<::vision::Material> create_vision_material(
    const OpticsDevice& optics,
    const MeshDevice& mesh_dev);
void attach_vision_emission(::vision::Scene& scene, ::vision::ShapeInstance& instance,
    const OpticsDevice& optics, const MeshDevice& mesh, unsigned instance_index);

}  // namespace Corona::Systems::Vision

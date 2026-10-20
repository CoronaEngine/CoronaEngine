#pragma once

// Vision geometry adapter - converts CoronaEngine scene geometry (CPU mesh data,
// transforms, per-mesh materials) into Vision ShapeGroup / ShapeInstance objects
// and adds them to the Vision scene.

#include <cstdint>

namespace Corona {
struct ExternalVisionBindingDevice;
}

// Forward declarations
namespace vision {
class Scene;
}  // namespace vision

namespace Corona::Systems::Vision {

// Structured outcome of a geometry build pass. Lets callers distinguish a
// genuinely empty scene (no candidate objects at all) from a transient
// "data-not-ready" state (candidates existed but their CPU/GPU mesh data could
// not be loaded yet), so dynamic-scene rebuilds can retry only when warranted.
struct VisionBuildResult {
    int instance_count = 0;   ///< ShapeInstances successfully added.
    int candidate_count = 0;  ///< Visible objects with geometry that passed all filters.
    int skipped_no_data = 0;  ///< Candidates skipped because mesh data was unavailable.
};

struct AddVisionShapeResult {
    bool added{false};
    int shape_index{-1};
    unsigned int material_topology_before{0};
    unsigned int material_topology_after{0};
    int instance_count{0};
    int candidate_count{0};
    int skipped_no_data{0};
};

// Clears the Vision scene's existing shapes and repopulates it from the
// CoronaEngine SharedDataHub (all enabled SceneDevice → actors → geometry).
//
// For each GeometryDevice that has a valid model_resource_handle:
//   - Reads CPU mesh vertices/indices from the Corona resource layer.
//   - Creates a Vision Mesh.
//   - Creates a Vision ShapeInstance with the object-to-world transform and
//     a PrincipledBSDF material mapped from the corresponding OpticsDevice.
//   - Adds it to the Vision scene via a ShapeGroup.
//
// After this call, caller must invoke Pipeline::prepare_geometry(), which
// uploads the shared scene GPU buffers and builds the acceleration structure.
//
// Returns a VisionBuildResult describing how many instances were added and
// whether any candidate objects were skipped due to missing mesh data.
VisionBuildResult build_vision_geometry(::vision::Scene& scene);

AddVisionShapeResult add_vision_shape_for_actor(
    ::vision::Scene& scene,
    std::uintptr_t actor_handle,
    const Corona::ExternalVisionBindingDevice& binding);

bool remove_vision_shape_for_actor(::vision::Scene& scene, unsigned int shape_index);

}  // namespace Corona::Systems::Vision

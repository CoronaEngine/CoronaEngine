#pragma once

// Vision geometry adapter - converts CoronaEngine scene geometry (CPU mesh data,
// transforms, per-mesh materials) into Vision ShapeGroup / ShapeInstance objects
// and adds them to the Vision scene.

#include <cstdint>

namespace Corona {
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
    int candidate_count = 0;  ///< Explicitly enabled objects with geometry (not raster visibility).
    int skipped_no_data = 0;  ///< Candidates skipped because mesh data was unavailable.
    int invalid_data = 0;     ///< Failed imports or malformed meshes; not a readiness delay.

    [[nodiscard]] bool complete() const noexcept {
        return skipped_no_data == 0 && invalid_data == 0;
    }
};



// Stages all CPU data, then replaces the Vision scene's existing shapes from the
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
// An incomplete result leaves existing Vision geometry unchanged. Callers must
// not advance their accepted scene revision until complete() is true.
VisionBuildResult build_vision_geometry(::vision::Scene& scene);

// Readiness/diagnostics pass using the same CPU staging path as the build. Does
// not create a device or mutate a Vision scene. instance_count is the number of
// staged instances (including ready objects when other objects are pending).
VisionBuildResult inspect_vision_geometry();



}  // namespace Corona::Systems::Vision

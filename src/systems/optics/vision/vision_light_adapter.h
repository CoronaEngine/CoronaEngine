#pragma once

// Vision light adapter - maps CoronaEngine EnvironmentDevice sun/sky parameters
// to Vision directional light and environment light.

// Forward declarations
namespace vision {
class Scene;
}  // namespace vision

namespace Corona {
struct EnvironmentDevice;
}  // namespace Corona

namespace Corona::Systems::Vision {

// Clears all existing lights from the Vision scene and adds:
//   1. A DirectionalLight from EnvironmentDevice::sun_position / sun_color / sun_intensity
//   2. An EnvironmentLight (infinite) from sky_intensity
//
// Call once per scene (re)build, not every frame.
void setup_vision_lights(::vision::Scene& scene, const EnvironmentDevice& env);

}  // namespace Corona::Systems::Vision

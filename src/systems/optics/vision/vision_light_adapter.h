#pragma once

#include <cstdint>
#include <array>

// Lighting for Corona-built Vision scenes. Native atmosphere parameters are not
// radiance values, so this bridge deliberately exposes a neutral geometry preview
// until the two renderers share an atmospheric sky implementation. The sun uses
// Vision's true delta-direction light, with Corona's direction and irradiance.

// Forward declarations
namespace vision {
class Scene;
}  // namespace vision

namespace Corona {
struct EnvironmentDevice;
}  // namespace Corona

namespace Corona::Systems::Vision {

enum class VisionLightingMode { None, NeutralPreview };

struct VisionLightingState {
    VisionLightingMode mode{VisionLightingMode::None};
    // Vision stores the outgoing direction, whereas Corona stores toward-sun.
    std::array<float, 3> sun_direction{};
    std::array<float, 3> sun_color{};
    float sun_intensity{0.f};
    bool operator==(const VisionLightingState&) const = default;
};

[[nodiscard]] VisionLightingState make_vision_lighting_state(const EnvironmentDevice& env) noexcept;

struct VisionLightSetupResult {
    VisionLightingMode mode{VisionLightingMode::None};
    float environment_radiance{0.f};
    std::uint32_t environment_count{0};
    std::uint32_t directional_light_count{0};
    std::uint32_t area_light_count{0};
    std::uint32_t punctual_light_count{0};
};

// A positive finite native sky intensity enables a unit-radiance neutral
// preview. Zero/invalid sky intensity disables it. Solar illumination is
// independent and uses the authored sun intensity. No atmosphere parity implied.
[[nodiscard]] VisionLightingMode vision_lighting_mode(const EnvironmentDevice& env) noexcept;
[[nodiscard]] const char* vision_lighting_mode_name(VisionLightingMode mode) noexcept;

// Replaces non-area lights in an EngineBuilt scene, preserving geometry-owned
// area lights. A black environment is retained in None mode because Vision's
// current miss and light-sampling paths require a registered environment.
// Call on scene build or effective lighting-mode change, then prepare/upload the
// scene light data and invalidate accumulated frames before rendering again.
VisionLightSetupResult setup_vision_lights(::vision::Scene& scene, const EnvironmentDevice& env);

}  // namespace Corona::Systems::Vision

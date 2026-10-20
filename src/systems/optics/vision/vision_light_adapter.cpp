#include "vision_light_adapter.h"

#ifdef CORONA_ENABLE_VISION

#include <cmath>
#include <vector>

#include <corona/shared_data_hub.h>

#include "base/illumination/light.h"
#include "base/import/node_desc.h"
#include "base/mgr/scene.h"

namespace Corona::Systems::Vision {

void setup_vision_lights(::vision::Scene& scene, const EnvironmentDevice& env) {
    // =========================================================================
    // Vision lighting model constraints (learned the hard way):
    //
    //  * Both "directional" (DirectionalLight) and "spherical" (SphericalMap)
    //    derive from vision::Environment and carry LightType::Infinite.
    //  * An Infinite light only contributes when registered as the scene's
    //    env_light_, and the ONLY path that assigns env_light_ is
    //    LightManager::init() (for each Infinite LightDesc it does
    //    `env_light_ = dynamic_object_cast<Environment>(light)`).
    //  * The scene supports exactly ONE environment light. Registering TWO
    //    Infinite lights (e.g. directional + spherical) makes the second
    //    overwrite env_light_ while the first stays orphaned in the light list,
    //    corrupting the light sampler's env index / PMF bookkeeping and
    //    crashing the Vulkan device (observed: process exit code -1 right after
    //    "Vision scene rebuilt").
    //
    // Therefore: register a SINGLE Infinite "spherical" light as the sky dome
    // (this is what lights the whole scene and fixes the all-black frame), plus
    // an optional NON-Infinite "point" light to act as the directional "sun".
    // The point light is freely combinable because it is not an Environment.
    // =========================================================================

    // -------------------------------------------------------------------------
    // 1. Spherical environment light (the sole env_light_ / sky dome)
    //    A constant white colour scaled by sky_intensity gives uniform ambient
    //    illumination; SphericalMap::prepare() safely falls back to a 1x1
    //    importance map when the colour node is a constant (no HDRI).
    // -------------------------------------------------------------------------
    float sky = env.sky_intensity;
    if (sky < 0.f) sky = 0.f;
    // Guarantee a non-zero ambient floor so the frame is never fully black even
    // if the scene environment reports a zero sky intensity.
    if (sky < 1.f) sky = 1.f;

    ::vision::LightDesc sky_desc("spherical");
    sky_desc.init(::vision::ParameterSet{::vision::DataWrap::object()});
    sky_desc.set_value("color", ::vision::DataWrap::array({1.f, 1.f, 1.f}));
    sky_desc.set_value("scale", sky);

    // -------------------------------------------------------------------------
    // 2. Point light approximating the directional sun (NON-Infinite)
    //    sun_position is a world-space direction-ish point; place a bright point
    //    light far along that direction. PointLight falls off with 1/distance^2,
    //    so push it out and boost the scale accordingly.
    // -------------------------------------------------------------------------
    float sx = env.sun_position.x;
    float sy = env.sun_position.y;
    float sz = env.sun_position.z;
    float slen = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (slen < 1e-6f) { sx = 0.f; sy = 1.f; sz = 0.f; }
    else { sx /= slen; sy /= slen; sz /= slen; }

    // Distance to place the "sun" point light, and the intensity boost needed to
    // counter the inverse-square falloff at that distance.
    constexpr float kSunDistance = 50.f;
    const float px = sx * kSunDistance;
    const float py = sy * kSunDistance;
    const float pz = -sz * kSunDistance;

    float sun = env.sun_intensity;
    if (sun < 0.f) sun = 0.f;
    // Compensate inverse-square falloff (Le divides by distance^2).
    const float sun_scale = sun * kSunDistance * kSunDistance;

    float cr = env.sun_color.x;
    float cg = env.sun_color.y;
    float cb = env.sun_color.z;

    ::vision::LightDesc sun_desc("point");
    sun_desc.init(::vision::ParameterSet{::vision::DataWrap::object()});
    sun_desc.set_value("color", ::vision::DataWrap::array({cr, cg, cb}));
    sun_desc.set_value("position", ::vision::DataWrap::array({px, py, pz}));
    sun_desc.set_value("scale", sun_scale);

    // -------------------------------------------------------------------------
    // 3. Clear any previously-registered lights BEFORE re-registering.
    //
    //    Root cause (observed crash / no-image after a Vision scene rebuild):
    //    LightManager::init() and add_light() are PURE APPEND operations - they
    //    only push_back into lights_ and never clear the existing list. Calling
    //    setup_vision_lights() again on every rebuild therefore accumulated the
    //    lights (2 -> 4 -> 6 ...), as proven by the log going from
    //    "1 light types with 2 light instances" (initial) to "... 4 light
    //    instances" (after first rebuild).
    //
    //    Worse, each rebuild added a SECOND Infinite "spherical" light. Only the
    //    last one becomes env_light_, leaving the previous Infinite light as an
    //    orphan in lights_. This corrupts the light sampler's env index / PMF
    //    bookkeeping and crashes the Vulkan device - exactly the failure mode the
    //    comment at the top of this function warns about, just triggered across
    //    rebuilds instead of within a single call.
    //
    //    remove_light() erases by underlying pointer and resets env_light_ when
    //    the removed light is the current environment, so draining the snapshot
    //    leaves a clean slate (lights_ empty, env_light_ == nullptr).
    // -------------------------------------------------------------------------
    // -------------------------------------------------------------------------
    // CRITICAL: only remove the lights WE previously injected (the Infinite sky
    // env light and the non-Area point sun). We must NOT touch Area lights.
    //
    // build_vision_geometry() runs BEFORE this function and registers Area lights
    // that are owned by - and back-referenced from - geometry ShapeInstances
    // (LightManager::init() does emission->instance()->set_emission(emission), so
    // the ShapeInstance keeps a pointer to its emission Light). If we blindly drain
    // EVERY light here we erase those Area lights while their ShapeInstances still
    // reference them, leaving the geometry's emission pointers dangling. The very
    // next light_sampler_->prepare() (inside renderer().prepare_lights()) then walks
    // an inconsistent light set whose Area entries point at freed objects, producing
    // an out-of-bounds / use-after-free GPU access and crashing the Vulkan device
    // (observed: process exit -1 right after "task build_accel ...", with NO
    // "This scene contains N light types" log emitted - i.e. crash inside
    // light->prepare()).
    //
    // Removing only the non-Area lights still guarantees a single Infinite env_light_
    // (avoiding the duplicate-env crash) and prevents the per-rebuild accumulation of
    // sky/sun lights, while leaving the geometry-owned Area lights intact.
    // -------------------------------------------------------------------------
    auto& light_mgr = scene.light_manager();
    {
        std::vector<::vision::TLight> to_remove;
        for (auto& light : light_mgr.lights()) {
            if (!light->match(::vision::LightType::Area)) {
                to_remove.push_back(light);
            }
        }
        for (auto& light : to_remove) {
            light_mgr.remove_light(light);
        }
    }

    // Register the single Infinite (spherical) env light through LightManager::init
    // so env_light_ is assigned exactly like a JSON-loaded scene. The point sun is
    // safe to add afterwards because it is NOT an Environment.
    light_mgr.init({sky_desc});
    scene.load_light(sun_desc);
}

}  // namespace Corona::Systems::Vision

#endif  // CORONA_ENABLE_VISION

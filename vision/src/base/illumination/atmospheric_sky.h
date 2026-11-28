#pragma once

#include "base/mgr/pipeline.h"
#include <array>

namespace vision {

// Single scattering in a spherical atmosphere, distances in kilometres.
// The baked map has unit incident solar radiance; spherical.scale controls brightness.
class AtmosphericSky {
    using BakeShader = ocarina::Shader<void(ocarina::Buffer<ocarina::float4>, ocarina::float3)>;
    BakeShader shader_;
    ocarina::float3 direction_{};
    bool baked_{false};

    static auto make_kernel() {
        using namespace ocarina;
        return Kernel{[](BufferVar<float4> output, Float3 sun) {
            configure_block(8u, 8u);
            constexpr float earth = 6371.f, top = earth + 100.f, eye = earth + .001f;
            constexpr uint steps = 16u, sun_steps = 8u;
            const Float2 uv = (make_float2(dispatch_idx().xy()) + .5f) / make_float2(dispatch_dim().xy());
            const Float theta = Pi * uv.y, phi = 2.f * Pi * uv.x;
            // Default spherical light: Z-polar map transformed to Vision Y-up.
            const Float3 view = make_float3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
            const Float3 origin = make_float3(0.f, eye, 0.f);
            const Float b = eye * view.y;
            // Continue the sea-level density below the horizon for a smooth
            // environment backdrop. Scene geometry provides the actual ground.
            const Float distance = -b + sqrt(b*b + (top-eye)*(top+eye));
            const Float step = distance / float(steps);
            const Float3 beta_r = make_float3(.0055f, .0130f, .0224f);
            constexpr float beta_m = .021f, g = .76f;
            const Float mu = clamp(dot(view, sun), -1.f, 1.f);
            const Float phase_r = 3.f * (1.f + mu*mu) / (16.f * Pi);
            const Float phase_m = (1.f-g*g) / (4.f*Pi*pow(1.f+g*g-2.f*g*mu, 1.5f));
            auto density = [&](const Float3 &p) {
                const Float h = max(length(p) - earth, 0.f);
                return exp(-h / make_float2(8.f, 1.2f));
            };
            Float2 view_depth = make_float2(0.f);
            Float3 radiance = make_float3(0.f);
            $for(i, steps) {
                const Float3 p = origin + view * ((cast<float>(i) + .5f) * step);
                const Float2 segment = density(p) * step;
                const Float radius = length(p);
                const Float along_sun = dot(p, sun);
                const Float ground = (radius-earth)*(radius+earth);
                // Earth's shadow blocks sunlight at this scattering point.
                $if(along_sun >= 0.f || along_sun*along_sun < ground) {
                    const Float exit = -along_sun + sqrt(max(along_sun*along_sun + (top-radius)*(top+radius), 0.f));
                    Float2 sun_depth = make_float2(0.f);
                    $for(j, sun_steps) {
                        const Float a = cast<float>(j) / float(sun_steps);
                        const Float z = (cast<float>(j) + 1.f) / float(sun_steps);
                        const Float mid = .5f * (a*a + z*z) * exit;
                        sun_depth += density(p + sun * mid) * ((z*z-a*a) * exit);
                    };
                    const Float2 optical = view_depth + .5f*segment + sun_depth;
                    const Float3 transmission = exp(-beta_r*optical.x - (1.1f*beta_m)*optical.y);
                    radiance += transmission * (beta_r*(phase_r*segment.x) + beta_m*phase_m*segment.y);
                };
                view_depth += segment;
            };
            output.write(dispatch_id(), make_float4(radiance, 1.f));
        }};
    }

public:
    [[nodiscard]] bool matches(ocarina::float3 sun) const noexcept {
        return baked_ && ocarina::all(direction_ == sun);
    }
    void mark_baked(ocarina::float3 sun) noexcept { direction_ = sun; baked_ = true; }

    ocarina::Image bake(Pipeline &pipeline, ocarina::float3 sun) {
        using namespace ocarina;
        Clock clock;
        const auto size = make_uint2(512u, 256u);
        if (!shader_.has_function()) shader_ = pipeline.device().compile(make_kernel(), "Vision-AtmosphericSky");
        auto pixels = pipeline.device().create_buffer<float4>(size.x * size.y);
        auto image = Image::create_empty(PixelStorage::FLOAT4, size);
        pipeline.stream() << shader_(pixels, sun).dispatch(size)
                          << pixels.download(image.pixel_ptr<float4>()) << synchronize() << commit();
        std::array<double, 3> sum{};
        const auto *data = image.pixel_ptr<float4>();
        for (size_t i = 0; i < image.pixel_num(); ++i) {
            for (unsigned c = 0; c < 3; ++c) sum[c] += data[i][c];
        }
        for (unsigned c = 0; c < 3; ++c) image.average<4>()[c] = float(sum[c] / image.pixel_num());
        image.average<4>()[3] = 1.f;
        OC_INFO_FORMAT("Vision atmosphere baked {}x{} toward_sun=({},{},{}) generation_ms={:.3f}",
                       size.x, size.y, sun.x, sun.y, sun.z, clock.elapse_ms());
        return image;
    }
};

} // namespace vision

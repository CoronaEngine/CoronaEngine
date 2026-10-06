//
// Created by Zero on 2023/11/7.
//

#pragma once

#include "core/stl.h"
#include "dsl/dsl.h"
#include "base/import/parameter_set.h"
#include "base/scattering/interaction.h"
#include "base/encoded_object.h"
#include "base/mgr/pipeline.h"
#include "core/concurrency/thread_pool.h"
#include "hotfix/hotfix.h"
#include "reservoir.h"
#include "base/using.h"

namespace vision {
class ReservoirHistory {
private:
    uint previous_camera_jitter_{InvalidUI32};

public:
    [[nodiscard]] bool begin_frame(uint frame, uint camera_jitter) noexcept {
        // A mode switch changes the sample lattice, even when a caller uses a
        // live setter without explicitly invalidating the whole integrator.
        bool valid = frame != 0u && previous_camera_jitter_ == camera_jitter;
        previous_camera_jitter_ = camera_jitter;
        return valid;
    }
};

struct SpatialResamplingParam {
public:
    float theta{};
    float depth_threshold{};
    float sampling_radius{};
    uint sample_num{};
    bool open{};

public:
    SpatialResamplingParam() = default;
    explicit SpatialResamplingParam(const ParameterSet &ps)
        : theta(ps["theta"].as_float(35)),
          depth_threshold(ps["depth"].as_float(0.3f)),
          sampling_radius(ps["radius"].as_float(30)),
          sample_num(ps["sample_num"].as_uint(1)),
          open{ps["open"].as_bool(true)} {
        sample_num = open ? sample_num : 1;
    }

    [[nodiscard]] float dot_threshold() const noexcept {
        return cosf(radians(theta));
    }
};

struct TemporalResamplingParam {
public:
    uint limit{};
    uint N{};
    float sampling_radius{};
    float theta{};
    float depth_threshold{};
    bool open{};
    bool mis{};

public:
    TemporalResamplingParam() = default;
    explicit TemporalResamplingParam(const ParameterSet &ps)
        : limit(ps["history_limit"].as_uint(5)),
          sampling_radius(ps["radius"].as_float(8)),
          theta(ps["theta"].as_float(35)),
          depth_threshold(ps["depth"].as_float(0.3)),
          open{ps["open"].as_bool(true)},
          N{ps["N"].as_uint(9)},
          mis{ps["mis"].as_bool(false)} {}

    [[nodiscard]] float dot_threshold() const noexcept {
        return cosf(radians(theta));
    }
};

}// namespace vision

namespace vision {
// Film samples are at pixel + 0.5 + jitter. Motion excludes camera jitter,
// so subtract the previous offset before choosing a previous reservoir.
[[nodiscard]] inline Float2 previous_reservoir_coord(const Float2 &film, const Float2 &motion,
                                                      const Float2 &previous_offset) noexcept {
    return film - motion - previous_offset;
}

[[nodiscard]] inline Int2 reservoir_pixel(const Float2 &coord) noexcept {
    // Keep negatives signed until the caller has checked in_screen().
    return make_int2(floor(coord));
}

[[nodiscard]] inline Bool is_valid_neighbor(const SurfaceDataVar &cur_surface, const SurfaceDataVar &another_surface,
                                            const Float &dot_threshold, const Float &depth_threshold,
                                            const Float &diff_threshold, const Float &another_depth) noexcept {
    Bool cond0 = dot(cur_surface->normal(), another_surface->normal()) > dot_threshold;
    Bool cond1 = abs(cur_surface->depth() - another_depth) /
                     max(abs(cur_surface->depth()), 1e-6f) < depth_threshold;
    Bool cond2 = abs(cur_surface->diffuse_factor() - another_surface->diffuse_factor()) /
                     max(abs(cur_surface->diffuse_factor()), 1e-6f) < diff_threshold;
    // Replaced hits describe a specular-chain endpoint, while these guides
    // still describe the primary surface. They cannot validate that endpoint.
    return cond0 && cond1 &&
           cond2 && cur_surface.hit->is_hit() && another_surface.hit->is_hit() &&
           !cur_surface.is_replaced && !another_surface.is_replaced;
}

[[nodiscard]] inline Bool is_valid_neighbor(const SurfaceDataVar &cur_surface, const SurfaceDataVar &another_surface,
                                            const Float &dot_threshold, const Float &depth_threshold,
                                            const Float &diff_threshold) noexcept {
    // Spatial guides already store depth in the same camera space.
    return is_valid_neighbor(cur_surface, another_surface, dot_threshold, depth_threshold,
                             diff_threshold, another_surface->depth());
}

class ReSTIR : public Toolkit, public RenderEnv, public GUI, public RuntimeObject {
protected:
    SpatialResamplingParam spatial_{};
    TemporalResamplingParam temporal_{};
    bool open_{true};
    uint max_age_{};
    mutable ReservoirHistory history_{};
    float diff_factor_{0.3f};
    using IntegratorPtr = weak_ptr<IlluminationIntegrator>;
    IntegratorPtr integrator_{};

public:
    ReSTIR() = default;
    explicit ReSTIR(IntegratorPtr integrator, const ParameterSet &desc)
        : integrator_(integrator),
          spatial_(desc["spatial"]),
          temporal_(desc["temporal"]),
          open_(desc["open"].as_bool(true)),
          diff_factor_(desc["diffuse_factor"].as_float(0.3f)),
          max_age_(desc["max_age"].as_uint(30)) {}
    VS_HOTFIX_MAKE_RESTORE(RuntimeObject, spatial_, temporal_, open_, history_,
                           max_age_, diff_factor_, integrator_)
    OC_MAKE_MEMBER_GETTER(open, )
    OC_MAKE_MEMBER_SETTER(integrator)
    [[nodiscard]] IlluminationIntegrator *integrator() noexcept { return integrator_.lock().get(); }
    [[nodiscard]] const IlluminationIntegrator *integrator() const noexcept { return integrator_.lock().get(); }
    [[nodiscard]] Float2 previous_film_offset(const Uint &camera_jitter) const noexcept {
        Float2 offset = make_float2(0.f);
        auto &sampler = renderer().sampler();
        sampler->temporary([&](Sampler *local_sampler) {
            local_sampler->set_seed(make_uint2(0u), max(frame_index(), 1u) - 1u, 0u);
            offset = scene().sensor()->filter()->sample(local_sampler->next_2d()).p;
        });
        return select(camera_jitter != 0u, offset, make_float2(0.f));
    }
    virtual void update_resolution(uint2 res) noexcept {}
    [[nodiscard]] Uint checkerboard_value() const noexcept {
        return frame_buffer().checkerboard_value(dispatch_idx().xy());
    }
    [[nodiscard]] auto prev_surfaces() const noexcept {
        return pipeline()->bindless_array().buffer_var<SurfaceData>(frame_buffer().prev_surfaces_index(frame_index()));
    }
    [[nodiscard]] auto cur_surfaces() const noexcept {
        return pipeline()->bindless_array().buffer_var<SurfaceData>(frame_buffer().cur_surfaces_index(frame_index()));
    }
    [[nodiscard]] auto prev_surface_extends() const noexcept {
        return pipeline()->bindless_array().buffer_var<SurfaceExtend>(frame_buffer().prev_surface_exts_index(frame_index()));
    }
    [[nodiscard]] auto cur_surface_extends() const noexcept {
        return pipeline()->bindless_array().buffer_var<SurfaceExtend>(frame_buffer().cur_surface_exts_index(frame_index()));
    }
    [[nodiscard]] Float3 cur_view_pos(const Bool &is_replace) const noexcept {
        Float3 view_pos;
        $if(is_replace) {
            view_pos = cur_surface_extends().read(dispatch_id()).view_pos;
        }
        $else {
            view_pos = scene().sensor()->device_position();
        };
        return view_pos;
    }
};
}// namespace vision

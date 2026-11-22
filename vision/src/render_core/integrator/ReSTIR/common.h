//
// Created by Zero on 2023/11/7.
//

#pragma once

#include "core/stl.h"
#include "dsl/dsl.h"
#include "base/import/parameter_set.h"
#include "base/scattering/material.h"
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
          sampling_radius(ps["radius"].as_float(0)),
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
// Reproject the actual film sample into a previous pixel cell. Each reservoir
// owns an independently jittered sample, so there is no global previous offset
// to subtract. Surface validation below decides whether that sample is reusable.
[[nodiscard]] inline Float2 previous_reservoir_coord(const Float2 &film, const Float2 &motion) noexcept {
    return film - motion;
}

class ReSTIR : public Toolkit, public RenderEnv, public GUI, public RuntimeObject {
protected:
    SpatialResamplingParam spatial_{};
    TemporalResamplingParam temporal_{};
    bool open_{true};
    float factor_{1.f};
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
          factor_(std::max(desc["factor"].as_float(1.f), 0.f)),
          diff_factor_(desc["diffuse_factor"].as_float(0.3f)),
          max_age_(desc["max_age"].as_uint(30)) {}
    VS_HOTFIX_MAKE_RESTORE(RuntimeObject, spatial_, temporal_, open_, factor_, history_,
                           max_age_, diff_factor_, integrator_)
    OC_MAKE_MEMBER_GETTER(open, )
    OC_MAKE_MEMBER_SETTER(integrator)
    void invalidate_history() noexcept { history_ = {}; }
    [[nodiscard]] DataWrap settings() const {
        return {{"open", open_}, {"max_age", max_age_}, {"diffuse_factor", diff_factor_},
                {"temporal", {{"open", temporal_.open}, {"history_limit", temporal_.limit},
                              {"theta", temporal_.theta}, {"depth", temporal_.depth_threshold},
                              {"radius", temporal_.sampling_radius}}},
                {"spatial", {{"open", spatial_.open}, {"sample_num", spatial_.sample_num}, {"theta", spatial_.theta},
                             {"depth", spatial_.depth_threshold}, {"radius", spatial_.sampling_radius}}}};
    }
    // Called only after the complete request has been validated by the integrator.
    void set_settings(const DataWrap &settings) {
        open_ = settings["open"].get<bool>();
        max_age_ = settings["max_age"].get<uint>();
        diff_factor_ = settings["diffuse_factor"].get<float>();
        const auto &temporal = settings["temporal"];
        temporal_.open = temporal["open"].get<bool>();
        temporal_.limit = temporal["history_limit"].get<uint>();
        temporal_.theta = temporal["theta"].get<float>();
        temporal_.depth_threshold = temporal["depth"].get<float>();
        temporal_.sampling_radius = temporal["radius"].get<float>();
        const auto &spatial = settings["spatial"];
        spatial_.open = spatial["open"].get<bool>();
        spatial_.sample_num = spatial["sample_num"].get<uint>();
        spatial_.theta = spatial["theta"].get<float>();
        spatial_.depth_threshold = spatial["depth"].get<float>();
        spatial_.sampling_radius = spatial["radius"].get<float>();
    }
    [[nodiscard]] IlluminationIntegrator *integrator() noexcept { return integrator_.lock().get(); }
    [[nodiscard]] const IlluminationIntegrator *integrator() const noexcept { return integrator_.lock().get(); }
    virtual void update_resolution(uint2 res) noexcept {}
};
}// namespace vision

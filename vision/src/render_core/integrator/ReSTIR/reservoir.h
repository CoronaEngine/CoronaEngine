//
// Created by Zero on 2023/9/3.
//

#pragma once

#include "math/basic_types.h"
#include "core/stl.h"
#include "dsl/dsl.h"
#include "base/sampler.h"
#include "base/illumination/light.h"
#include "common.h"
#include "base/using.h"

namespace vision {
struct SurfacePoint {
    array<float, 3> pos{};
    array<float, 3> ng{};
    void set_position(float3 p) noexcept {
        pos[0] = p[0];
        pos[1] = p[1];
        pos[2] = p[2];
    }
    [[nodiscard]] auto position() const noexcept { return make_float3(pos[0], pos[1], pos[2]); }
    void set_normal(float3 n) noexcept {
        ng[0] = n[0];
        ng[1] = n[1];
        ng[2] = n[2];
    }
    [[nodiscard]] auto normal() const noexcept { return make_float3(ng[0], ng[1], ng[2]); }
    [[nodiscard]] auto valid() const noexcept { return ocarina::any(normal() != 0.f); }
};
}// namespace vision

// clang-format off
OC_STRUCT(vision, SurfacePoint, pos, ng) {
    void set_position(Float3 p) noexcept {
        pos.set(p);
    }
    [[nodiscard]] auto position() const noexcept { return pos.as_vec(); }
    void set_normal(Float3 n) noexcept {
        ng.set(n);
    }
    [[nodiscard]] auto normal() const noexcept { return ng.as_vec(); }
    void set(const vision::Interaction &it) noexcept {
        set_position(it.pos);
        set_normal(it.ng);
    }
    [[nodiscard]] auto valid() const noexcept { return ocarina::any(normal() != 0.f);}
};
// clang-format on

namespace vision {
struct GISample {
    SurfacePoint sp{};
    array<float, 3> Lo{};
    float age{};
};
}// namespace vision

OC_STRUCT(vision, GISample, sp, Lo, age) {
    static constexpr EPort p = D;
    [[nodiscard]] Float p_hat(const Float3 &bsdf) const noexcept {
        return ocarina::luminance(Lo.as_vec() * bsdf);
    }
};

namespace vision {
struct GIReservoir {
public:
    static constexpr EPort p = H;
    oc_float<p> weight_sum{};
    oc_float<p> C{};
    oc_float<p> W{};
    GISample sample{};

    template<EPort p_ = D>
    [[nodiscard]] static oc_float<p_> cal_weight(oc_float<p_> mis_weight, oc_float<p_> p_hat,
                                                 oc_float<p_> W) noexcept {
        return mis_weight * p_hat * W;
    }

    template<EPort p_ = D>
    [[nodiscard]] static auto safe_weight(oc_float<p_> mis_weight, oc_float<p_> p_hat,
                                          oc_float<p_> W) noexcept {
        oc_float<p_> ret = cal_weight(mis_weight, p_hat, W);
        ret = ocarina::zero_if_nan_inf(ret);
        return ocarina::select(mis_weight > 0.f && p_hat > 0.f && W > 0.f,
                               ocarina::max(ret, 0.f), 0.f);
    }
};
}// namespace vision

// clang-format off
OC_STRUCT(vision,GIReservoir, weight_sum, C, W, sample) {
    static constexpr EPort p = D;
    Bool update(oc_float<p> u, vision::GISampleVar v, oc_float<p> weight, oc_float<p> new_C = 1.f) noexcept {
        weight = ocarina::max(ocarina::zero_if_nan_inf(weight), 0.f);
        weight_sum = ocarina::max(ocarina::zero_if_nan_inf(weight_sum), 0.f);
        oc_float<p> combined = weight_sum + weight;
        // An overflowing incoming weight must not poison future history.
        weight = ocarina::select(ocarina::isnan(combined) || ocarina::isinf(combined), 0.f, weight);
        weight_sum += weight;
        C = ocarina::zero_if_nan_inf(C + ocarina::max(ocarina::zero_if_nan_inf(new_C), 0.f));
        Bool ret = weight > 0.f && u * weight_sum < weight;
        sample = ocarina::select(ret, v, sample);
        return ret;
    }
    void truncation(oc_float<p> limit) noexcept {
        oc_float<p> factor = ocarina::zero_if_nan_inf(limit / C);
        C = ocarina::min(limit, C);
        weight_sum = ocarina::select(factor < 1.f, weight_sum * factor, weight_sum);
        W = ocarina::select(C > 0.f, W, 0.f);
    }
    void process_occluded(oc_bool<p> occluded) noexcept {
        W = ocarina::select(occluded, 0.f, W);
        weight_sum = ocarina::select(occluded, 0.f, weight_sum);
    }
    [[nodiscard]] oc_float<p> cal_W(const oc_float<p> &p_hat) const noexcept {
        oc_float<p> ret = ocarina::zero_if_nan_inf((weight_sum / C) / p_hat);
        return ocarina::select(p_hat > 0.f && C > 0.f, ocarina::max(ret, 0.f), 0.f);
    }
    void update_W_with_support(const oc_float<p> &p_hat, const oc_float<p> &normalization) noexcept {
        oc_float<p> corrected = ocarina::zero_if_nan_inf((weight_sum / normalization) / p_hat);
        W = ocarina::select(p_hat > 0.f && normalization > 0.f && C > 0.f,
                            ocarina::max(corrected, 0.f), 0.f);
        // C remains the stream confidence. Store its equivalent weight sum,
        // so copying this reservoir into another merge preserves corrected W.
        weight_sum = vision::GIReservoir::safe_weight(C, p_hat, W);
        W = ocarina::select(weight_sum > 0.f, W, 0.f);
    }
    void update_W(const oc_float<p> &p_hat) noexcept {
        W = cal_W(p_hat);
    }
};
// clang-format on

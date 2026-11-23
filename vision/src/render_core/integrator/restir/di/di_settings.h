#pragma once

#include "../common.h"

namespace vision {

class DISettings final : public ReSTIR {
private:
    uint M_light_;
    uint M_bsdf_;
    bool pairwise_;
    uint max_recursion_;
    bool stable_planes_enabled_;

public:
    DISettings(IntegratorPtr integrator, const ParameterSet &desc)
        : ReSTIR(integrator, desc),
          M_light_(desc["M_light"].as_uint(1u)),
          M_bsdf_(desc["M_bsdf"].as_uint(1u)),
          pairwise_(desc["pairwise"].as_bool(true)),
          max_recursion_(desc["max_recursion"].as_uint(5u)),
          stable_planes_enabled_(desc["stable_planes"].as_bool(true)) {}

    [[nodiscard]] uint M_light() const noexcept { return M_light_; }
    [[nodiscard]] uint M_bsdf() const noexcept { return M_bsdf_; }
    [[nodiscard]] bool pairwise() const noexcept { return pairwise_; }
    [[nodiscard]] uint max_recursion() const noexcept { return max_recursion_; }
    [[nodiscard]] bool temporal_enabled() const noexcept { return temporal_.open; }
    [[nodiscard]] bool spatial_enabled() const noexcept { return spatial_.open; }
    [[nodiscard]] bool stable_planes_enabled() const noexcept { return stable_planes_enabled_; }
    void set_stable_planes_enabled(bool enabled) noexcept { stable_planes_enabled_ = enabled; }
    [[nodiscard]] float factor() const noexcept { return open_ ? factor_ : 0.f; }

    [[nodiscard]] DataWrap settings() const {
        auto result = ReSTIR::settings();
        result.update(DataWrap{{"M_light", M_light_}, {"M_bsdf", M_bsdf_},
                               {"max_recursion", max_recursion_}, {"pairwise", pairwise_}});
        return result;
    }

    void set_settings(const DataWrap &settings) {
        ReSTIR::set_settings(settings);
        M_light_ = settings["M_light"].get<uint>();
        M_bsdf_ = settings["M_bsdf"].get<uint>();
        max_recursion_ = settings["max_recursion"].get<uint>();
        pairwise_ = settings["pairwise"].get<bool>();
    }

    bool render_UI(Widgets *widgets) noexcept override {
        return widgets->use_tree("ReSTIR DI", [&] {
            changed_ |= widgets->check_box("switch", &open_);
            if (open_) render_sub_UI(widgets);
        });
    }

    void render_sub_UI(Widgets *widgets) noexcept override {
        changed_ |= widgets->input_uint_limit("M light", &M_light_, 0, 100);
        changed_ |= widgets->input_uint_limit("M BSDF", &M_bsdf_, 0, 100);
        changed_ |= widgets->drag_uint("max age", &max_age_, 1, 0, 100);
        changed_ |= widgets->input_uint_limit("max recursion", &max_recursion_, 1, 100);
        changed_ |= widgets->drag_float("diffuse factor threshold", &diff_factor_, 0, 1);
        changed_ |= widgets->check_box("temporal", &temporal_.open);
        changed_ |= widgets->check_box("pairwise", &pairwise_);
        if (temporal_.open) {
            changed_ |= widgets->input_uint_limit("history", &temporal_.limit, 0, 50, 1, 3);
            changed_ |= widgets->input_float_limit("temporal theta", &temporal_.theta, 0, 90, 1, 1);
            changed_ |= widgets->input_float_limit("temporal depth", &temporal_.depth_threshold, 0, 1, 0.02, 0.1);
            changed_ |= widgets->input_float_limit("temporal radius", &temporal_.sampling_radius, 0, 50, 1, 5);
        }
        changed_ |= widgets->check_box("spatial", &spatial_.open);
        if (spatial_.open) {
            changed_ |= widgets->input_float_limit("spatial theta", &spatial_.theta, 0, 90, 1, 1);
            changed_ |= widgets->input_float_limit("spatial depth", &spatial_.depth_threshold, 0, 1, 0.02, 0.1);
            changed_ |= widgets->input_float_limit("spatial radius", &spatial_.sampling_radius, 0, 50, 1, 5);
        }
    }
};

}// namespace vision

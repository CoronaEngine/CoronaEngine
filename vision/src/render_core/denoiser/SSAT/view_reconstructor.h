//
// SSAT diagnostic reconstruction of one discrete light-field view.
//

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "base/mgr/global.h"
#include "base/sensor/light_field_types.h"
#include "utils.h"

namespace vision::ssat {
using namespace ocarina;

class SSAT;

/// Reconstructs a pixel-sized 2D image from valid path-traced subpixels that
/// belong to one discrete view. No angular neighbors are ever mixed in.
class SsatViewReconstructor : public Toolkit, public RuntimeObject {
private:
    struct FilterConfig {
        float sigma{1.f};
        int radius{0};
    };

    SSAT *ssat_{nullptr};
    Shader<void(Buffer<RadType4>, Buffer<float4>, LenticularParams,
                uint, uint, uint, uint, float, int)>
        reconstruction_shader_;
    Shader<void(Buffer<float4>, Buffer<float4>, uint)>
        accumulation_shader_;
    Buffer<float4> current_buffer_;
    std::uint32_t last_view_index_{0u};
    std::uint32_t last_frame_index_{0u};
    std::uint32_t history_length_{0u};
    bool history_valid_{false};

    static constexpr std::uint32_t kMaxHistoryLength = 64u;

    [[nodiscard]] static FilterConfig filter_config(
        std::uint32_t view_count) noexcept {
        if (view_count <= 1u) {
            return {};
        }
        const float spacing = std::sqrt(static_cast<float>(view_count) / 3.f);
        const float sigma = std::max(0.75f, 0.5f * spacing);
        const int radius = std::clamp(
            static_cast<int>(std::ceil(3.f * sigma)), 1, 16);
        return {sigma, radius};
    }

    void ensure_buffer(std::uint32_t total_pixels) noexcept {
        if (current_buffer_.size() != total_pixels) {
            init_buffer_zero(device(), current_buffer_, total_pixels,
                             "SSAT::view_current");
            history_valid_ = false;
            history_length_ = 0u;
        }
    }

public:
    explicit SsatViewReconstructor(SSAT *ssat)
        : ssat_(ssat) {}

    VS_HOTFIX_MAKE_RESTORE(RuntimeObject, ssat_)

    void compile() noexcept {
        Kernel reconstruction = [&](BufferVar<RadType4> source,
                                    BufferVar<float4> output,
                                    Var<LenticularParams> lenticular,
                                    Uint selected_view,
                                    Uint view_count,
                                    Uint width,
                                    Uint height,
                                    Float sigma,
                                    Int radius) {
            const Uint2 pixel = dispatch_idx().xy();
            Float3 weighted_rgb = make_float3(0.f);
            Float weight_sum = 0.f;

            // The target-view mask is a diagonal, non-separable lattice. A
            // horizontal masked pass followed by a vertical pass leaves holes
            // whenever a row has no nearby target-view subpixel, so gather the
            // spatial neighborhood in two dimensions before normalization.
            $for(dy, -radius, radius + 1) {
                const Int source_y = cast<int>(pixel.y) + dy;
                $if(source_y < 0 || source_y >= cast<int>(height)) {
                    $continue;
                };

                $for(dx, -radius, radius + 1) {
                    const Int source_x = cast<int>(pixel.x) + dx;
                    $if(source_x < 0 || source_x >= cast<int>(width)) {
                        $continue;
                    };

                    $for(channel, 0, 3) {
                        const Uint source_x_u = cast<uint>(source_x);
                        const Uint source_y_u = cast<uint>(source_y);
                        const Uint channel_u = cast<uint>(channel);
                        const Uint sample_view = lightfield_subpixel_view_id(
                            source_x_u, source_y_u, channel_u,
                            lenticular, view_count);
                        $if(sample_view != selected_view) {
                            $continue;
                        };

                        const Float subpixel_dx =
                            cast<float>(dx) +
                            (cast<float>(channel) + 0.5f) / 3.f - 0.5f;
                        const Float distance_squared =
                            subpixel_dx * subpixel_dx +
                            cast<float>(dy * dy);
                        const Float weight = select(
                            view_count == 1u,
                            1.f,
                            exp(-0.5f * distance_squared /
                                (sigma * sigma)));
                        const Uint source_index =
                            source_y_u * (width * 3u) +
                            source_x_u * 3u + channel_u;
                        const Float4 sample = source.read(source_index);
                        $if(sample.w <= 0.f) {
                            $continue;
                        };
                        weighted_rgb += sample.xyz() * weight;
                        weight_sum += weight;
                    };
                };
            };

            Float3 result_rgb = make_float3(0.f);
            $if(weight_sum > 1e-6f) {
                result_rgb = weighted_rgb / weight_sum;
            };
            output.write(dispatch_id(), make_float4(result_rgb, 1.f));
        };
        reconstruction_shader_ = device().compile(
            reconstruction, "SSAT-ViewExtract2D");

        Kernel accumulation = [&](BufferVar<float4> current,
                                  BufferVar<float4> output,
                                  Uint history_length) {
            const Float4 sample = current.read(dispatch_id());
            Float3 result = sample.xyz();
            $if(history_length > 0u) {
                const Float3 history = output.read(dispatch_id()).xyz();
                const Float history_weight = cast<float>(history_length);
                result = (history * history_weight + sample.xyz()) /
                         (history_weight + 1.f);
            };
            output.write(dispatch_id(), make_float4(result, 1.f));
        };
        accumulation_shader_ = device().compile(
            accumulation, "SSAT-ViewAccumulate");
    }

    [[nodiscard]] CommandBatch dispatch(
        BufferView<RadType4> source,
        BufferView<float4> output,
        const LenticularParams &lenticular,
        std::uint32_t view_index,
        std::uint32_t frame_index) noexcept {
        CommandBatch ret;
        const std::uint32_t width =
            static_cast<std::uint32_t>(lenticular.res_w);
        const std::uint32_t height =
            static_cast<std::uint32_t>(lenticular.res_h);
        if (width == 0u || height == 0u) {
            return ret;
        }

        const std::uint32_t view_count =
            lightfield_view_count(lenticular.num_views);
        const std::uint32_t selected_view =
            lightfield_effective_view_index(view_index, view_count);
        const FilterConfig filter = filter_config(view_count);
        ensure_buffer(width * height);

        const bool contiguous_history =
            history_valid_ &&
            selected_view == last_view_index_ &&
            frame_index == last_frame_index_ + 1u;
        if (!contiguous_history) {
            history_length_ = 0u;
        }
        const std::uint32_t prior_history_length = history_length_;

        const uint2 resolution = make_uint2(width, height);
        ret << reconstruction_shader_(
                   source, current_buffer_.view(), lenticular,
                   selected_view, view_count, width, height,
                   filter.sigma, filter.radius)
                   .dispatch(resolution);
        ret << accumulation_shader_(
                   current_buffer_.view(), output, prior_history_length)
                   .dispatch(resolution);

        history_valid_ = true;
        last_view_index_ = selected_view;
        last_frame_index_ = frame_index;
        history_length_ = std::min(
            prior_history_length + 1u, kMaxHistoryLength);
        return ret;
    }
};

}// namespace vision::ssat

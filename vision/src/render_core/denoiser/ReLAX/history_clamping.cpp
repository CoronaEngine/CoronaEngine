#include "filter.h"
#include "utils.h"

namespace vision::relax {
void Filter::compile_history_clamping(Device &device) {
    Kernel clamp_history = [](Var<FilterParam> p) {
        Uint idx = dispatch_id(); Int2 pixel = make_int2(dispatch_idx().xy());
        auto center = load_guide(p, idx); auto data = p.history.read(idx);
        Var<PersistentHistory> persistent;
        $if(!valid_guide(center)) {
            p.src_diffuse.write(idx, make_float4(0.f)); p.src_specular.write(idx, make_float4(0.f));
            p.prev_history.write(idx, persistent);
            $return();
        };
        for (uint lobe = 0; lobe < 2; ++lobe) {
            const bool spec = lobe != 0u;
            auto &slow_buffer = spec ? p.src_specular : p.src_diffuse;
            const auto &fast_buffer = spec ? p.dst_specular : p.dst_diffuse;
            const auto &noisy_buffer = spec ? p.prepass_specular : p.prepass_diffuse;
            Float4 slow = slow_buffer.read(idx), fast = fast_buffer.read(idx);
            Float3 mean = make_float3(0.f), second = make_float3(0.f), noisy_mean = make_float3(0.f);
            Float noisy_second = 0.f, count = 0.f;
            $for(y, -2, 3) {
                $for(x, -2, 3) {
                    Uint tap = pixel_index(pixel + make_int2(x, y), p.size);
                    $if(valid_guide(load_guide(p, tap))) {
                        Float3 c = to_ycocg(fast_buffer.read(tap).xyz());
                        Float3 noisy = noisy_buffer.read(tap).xyz();
                        mean += c; second += c * c; noisy_mean += noisy;
                        Float l = luminance(noisy); noisy_second += l * l; count += 1.f;
                    };
                };
            };
            mean /= count; second /= count; noisy_mean /= count; noisy_second /= count;
            Float3 sigma = sqrt(max(second - mean * mean, make_float3(0.f)));
            Float3 fast_ycocg = to_ycocg(fast.xyz()), slow_ycocg = to_ycocg(slow.xyz());
            Float3 lo = min(mean - p.settings.color_box_sigma * sigma, fast_ycocg);
            Float3 hi = max(mean + p.settings.color_box_sigma * sigma, fast_ycocg);
            Float3 clamped = slow_ycocg;
            Float fast_limit = spec ? p.settings.specular_fast_history : p.settings.diffuse_fast_history;
            Float slow_limit = spec ? p.settings.specular_history : p.settings.diffuse_history;
            $if(fast_limit < slow_limit) { clamped = clamp(slow_ycocg, lo, hi); };
            Float4 output = make_float4(to_rgb(clamped), slow.w);
            Bool short_history = data.meta.x <= p.settings.history_fix_frames + 1.f;
            $if(short_history) {
                // Same-type xyz proxy assignment copies the parent float4 in
                // Ocarina. Construct explicitly to retain diffuse's slow M2.
                if (spec) output = fast;
                else output = make_float4(fast.xyz(), slow.w);
            };
            Float factor = 0.f;
            $if(clamped.x != slow_ycocg.x) {
                factor = saturate((clamped.x - slow_ycocg.x) / (fast_ycocg.x - slow_ycocg.x));
            };
            factor = select(short_history, 1.f, factor);
            Float difference = (spec ? .33f : 1.f) * 10.f * p.settings.antilag_acceleration *
                               luminance(abs(fast.xyz() - slow.xyz())) * factor;
            difference = select(short_history, 0.f, difference);
            Float3 distance = noisy_mean - fast.xyz();
            Float distance_l = luminance(abs(distance));
            Float3 acceleration = distance * saturate(difference / max(distance_l, 1e-20f));
            output.xyz() += acceleration; fast.xyz() += acceleration;
            Float original_l = luminance(slow.xyz()), noisy_l = luminance(noisy_mean);
            Float temporal_sigma = p.settings.antilag_temporal_sigma * sqrt(max(0.f, noisy_second - noisy_l * noisy_l));
            Float spatial_sigma = p.settings.antilag_spatial_sigma * sigma.x;
            Float reset = (spec ? .5f : 1.f) * p.settings.antilag_reset *
                          max(0.f, abs(original_l - noisy_l) - spatial_sigma - temporal_sigma) /
                          (1e-6f + max(original_l, noisy_l) + spatial_sigma + temporal_sigma);
            reset = saturate(reset);
            Float3 noisy = noisy_buffer.read(idx).xyz();
            output.xyz() = lerp(reset, output.xyz(), noisy);
            fast.xyz() = lerp(reset, fast.xyz(), noisy);
            Float output_l = luminance(output.xyz());
            output.w = max(0.f, output.w + output_l * output_l - original_l * original_l);
            slow_buffer.write(idx, output);
            if (spec) {
                data.specular_fast = fast;
                persistent.specular = output; persistent.specular_fast = fast;
            } else {
                data.diffuse_fast = make_float4(fast.xyz(), 0.f);
                persistent.diffuse = output; persistent.diffuse_fast = data.diffuse_fast;
            }
        }
        p.history.write(idx, data);
        // Publish the slow/fast histories while they are in registers.
        // The optional firefly pass replaces slow illumination before publishing.
        persistent.length_hit = make_float2(data.meta.x, data.meta.z);
        $if(p.settings.anti_firefly == 0u) { p.prev_history.write(idx, persistent); };
    };
    clamp_ = device.compile(clamp_history, "RELAX-HistoryClamping");

    // The clamped slow buffers already provide the immutable copy needed by
    // RCRS. Fast-history scratch is no longer consumed after clamping.
    Kernel firefly = [](Var<FilterParam> p) {
        Uint idx = dispatch_id(); Int2 pixel = make_int2(dispatch_idx().xy());
        auto center = load_guide(p, idx);
        Var<PersistentHistory> persistent;
        for (uint lobe = 0; lobe < 2; ++lobe) {
            const auto &input = lobe == 0 ? p.src_diffuse : p.src_specular;
            auto &output = lobe == 0 ? p.dst_diffuse : p.dst_specular;
            Float4 color = input.read(idx);
            $if(valid_guide(center)) {
                Float minimum = 1e10f, maximum = -1.f;
                Float3 min_color = color.xyz(), max_color = color.xyz();
                for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
                    if (x == 0 && y == 0) continue;
                    Int2 pos = pixel + make_int2(x, y);
                    $if(inside(pos, p.size)) {
                        Uint tap = pixel_index(pos, p.size); auto g = load_guide(p, tap);
                        $if(spatial_compatible(center, g)) {
                            Float3 c = input.read(tap).xyz(); Float l = luminance(c);
                            $if(l > maximum) { maximum = l; max_color = c; };
                            $if(l < minimum) { minimum = l; min_color = c; };
                        };
                    };
                }
                Float l = luminance(color.xyz());
                $if(l > maximum) { color.xyz() = max_color; };
                $if(l < minimum) { color.xyz() = min_color; };
            };
            output.write(idx, color);
            if (lobe == 0u) persistent.diffuse = color;
            else persistent.specular = color;
        }
        auto data = p.history.read(idx);
        persistent.diffuse_fast = data.diffuse_fast; persistent.specular_fast = data.specular_fast;
        persistent.length_hit = make_float2(data.meta.x, data.meta.z);
        p.prev_history.write(idx, persistent);
    };
    firefly_ = device.compile(firefly, "RELAX-AntiFirefly");
}
}

#include "utils.h"

namespace vision::reblur {
void Filter::compile_stabilization(Device &device) {
    Kernel stabilization = [](Var<FilterParam> p) {
        Uint i = dispatch_id(); Int2 pixel = make_int2(dispatch_idx().xy());
        auto g = p.guide.read(i); auto h = p.history.read(i);
        Float4 d = p.src_diffuse.read(i), s = p.src_specular.read(i);
        $if(valid_guide(g)) {
            for (bool spec : {false, true}) {
                Float4 value = spec ? s : d;
                Float mean = 0.f, m2 = 0.f, sum = 0.f;
                $for(y, -1, 2) {
                    $for(x, -1, 2) {
                        Uint j = pixel_index(pixel + make_int2(x, y), p.size);
                        auto ng = p.guide.read(j);
                        Float v = spec ? p.src_specular.read(j).x : p.src_diffuse.read(j).x;
                        Float w = same_surface(g, ng).cast<float>();
                        mean += v * w; m2 += v * v * w; sum += w;
                    };
                };
                mean /= max(sum, 1.f); m2 /= max(sum, 1.f);
                Float sigma = sqrt(max(m2 - mean * mean, 0.f));
                Float count = spec ? h.fast_count.w : h.fast_count.z;
                Float previous = spec ? h.stabilized.y : h.stabilized.x;
                Float quality = spec ? h.stabilized.w : h.stabilized.z;
                Float lag = antilag(previous, mean, sigma, count, p);
                Float weight = quality * count / (1.f + count);
                Float clipped = clamp(previous, mean - sigma * (1.f + 3.f * weight), mean + sigma * (1.f + 3.f * weight));
                Float stabilized = lerp(min(weight * lag, p.stabilization), value.x, clipped);
                value = change_luma(value, stabilized);
                Float next_count = min(lerp(lag, min(count + 1.f, p.fix_frames), count + 1.f), p.max_history);
                if (spec) { s = value; h.stabilized.y = stabilized; h.fast_count.w = next_count; }
                else { d = value; h.stabilized.x = stabilized; h.fast_count.z = next_count; }
            }
        };
        // Spatial passes read separate scratch buffers. No neighbour reads this write target.
        p.history.write(i, h);
        // All previous-frame neighborhood reads finished in TemporalAccumulation.
        auto saved = p.prev_history.read(i);
        saved.fast_count = h.fast_count; saved.stabilized = h.stabilized.xy(); saved.hit_distance = h.tracking.w;
        p.prev_history.write(i, saved);
        p.dst_diffuse.write(i, d); p.dst_specular.write(i, s);
    };
    stabilize_ = device.compile(stabilization, "REBLUR-TemporalStabilization");
}
}

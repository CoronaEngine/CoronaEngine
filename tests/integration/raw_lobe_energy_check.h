#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

inline void check_raw_lobe_energy(float sum, float rendered, float compact_factor,
                                  unsigned scenario, unsigned frame, unsigned pixel, unsigned channel) {
    float tolerance = 1e-4f * std::max(1.f, std::abs(rendered));
    if (compact_factor > 0.f) {
        // The raw estimator rounds to 12 mantissa bits before presentation
        // scales it. The separate diffuse, specular and bypass are FP32.
        int exponent = 0;
        std::frexp(std::abs(sum) / compact_factor, &exponent);
        const float half_step = sum == 0.f ? 0.f : std::ldexp(compact_factor, exponent - 14);
        tolerance = half_step + 4.f * std::numeric_limits<float>::epsilon() * std::max(1.f, std::abs(sum));
    }
    if (std::isfinite(sum) && std::isfinite(rendered) && std::abs(sum - rendered) <= tolerance) return;
    std::ostringstream detail;
    detail << "raw lobe sum differs from estimator, scenario=" << scenario << " frame=" << frame
           << " pixel=" << pixel << " channel=" << channel << " lobes=" << sum
           << " estimator=" << rendered << " tolerance=" << tolerance;
    throw std::runtime_error(detail.str());
}

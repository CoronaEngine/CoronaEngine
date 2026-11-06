#pragma once

#include "common.h"
#include "base/scattering/material.h"

namespace vision::falcor_di {

// The triangle sample includes the emitter geometry term. The material
// evaluator includes the receiver cosine, as in the final shading pass.
[[nodiscard]] inline Float target_weight(const ScatterEval &eval, const TriangleSample &light) {
    const Float value = luminance(eval.f.vec3() * light.radiance);
    return select(value > 0.f && finite(value), value, 0.f);
}

} // namespace vision::falcor_di

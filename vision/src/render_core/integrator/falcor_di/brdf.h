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

// RTXDI estimates continuous reflection. Compare both directions to the same
// authored frame; a back-side reflection has two negative cosines.
[[nodiscard]] inline Bool reflection_supported(const MaterialEvaluator &bsdf, const Float3 &wo,
                                               const Float3 &wi) {
    const Float3 normal = bsdf.shading_frame().normal();
    return !BxDFFlag::is_specular(bsdf.flag()) && dot(normal, wo) * dot(normal, wi) > 0.f;
}

[[nodiscard]] inline Float reflection_target_weight(const MaterialEvaluator &bsdf, const Float3 &wo,
                                                    const TriangleSample &light) {
    Float target = 0.f;
    $if(reflection_supported(bsdf, wo, light.direction)) {
        target = target_weight(bsdf.evaluate(wo, light.direction, MaterialEvalMode::F), light);
    };
    return target;
}

} // namespace vision::falcor_di

#pragma once
#include "dsl/dsl.h"

namespace vision::restir_di {
using namespace ocarina;

struct DenoiserContribution {
    Float3 diffuse, specular;
};

// The fraction comes from the complete original material at the selected DI
// direction. Derive the remainder from the original RGB contribution so the
// extra denoiser channels preserve that estimator, including black samples.
[[nodiscard]] inline DenoiserContribution split_denoiser_contribution(
    const Float3 &radiance, const Float3 &specular_fraction) {
    Float3 specular = radiance * specular_fraction;
    return {radiance - specular, specular};
}
}

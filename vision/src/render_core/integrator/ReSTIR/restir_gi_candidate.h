#pragma once

#include "reservoir.h"
#include "base/scattering/interaction.h"

namespace vision {
class ReSTIRGI;
}

namespace vision::restir_gi {
struct CandidateTraceResult {
    GISampleVar sample;
    HitBSDFVar proposal;
    Float3 bypass_radiance{make_float3(0.f)};

    CandidateTraceResult() noexcept {
        const Float3 zero = make_float3(0.f);
        sample.sp->set_position(zero);
        sample.sp->set_normal(zero);
        sample.Lo.set(zero);
        sample.age = 0.f;
        proposal.wi.set(zero);
        proposal.bsdf.set(zero);
        proposal.pdf = -1.f;
    }
};

// GI-owned transport output. Shared PT exports unweighted incident Lo; the
// reservoir owns inverse proposal PDF, and final shading applies current prefix.
}// namespace vision::restir_gi

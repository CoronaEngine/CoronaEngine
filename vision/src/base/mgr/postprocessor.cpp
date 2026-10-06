//
// Created by Zero on 2023/6/1.
//

#include "postprocessor.h"
#include "pipeline.h"
#include "switch_profile.h"

namespace vision {
using namespace ocarina;

Postprocessor::Postprocessor(Pipeline *rp)
    : rp_(rp) {}

void Postprocessor::compile_tone_mapping() noexcept {
    switch_profile::Scope profile{"tonemapping.compile", "compile"};
    Kernel<signature> kernel = [&](BufferVar<float4> input, BufferVar<float4> output, Bool gamma) {
        Float4 input_pixel = input.read(dispatch_id());
        Float4 output_pixel = tone_mapper_->apply(input_pixel);
        output_pixel = ocarina::select(gamma, linear_to_srgb(output_pixel), output_pixel);
        output_pixel.w = 1.f;
        output.write(dispatch_id(), output_pixel);
    };
    tone_mapping_shader_ = rp_->device().compile(kernel, "tonemapping");
}
void Postprocessor::tone_mapping(BufferView<float4> input,
                                 BufferView<float4> output,
                                 bool gamma) noexcept {
    Stream &stream = rp_->stream();
    stream << tone_mapping_shader_(input, output, gamma).dispatch(rp_->resolution());
    stream << synchronize();
    stream << commit();
}

}// namespace vision

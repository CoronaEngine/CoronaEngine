#include "gi_receiver.h"
#include "base/integral/integrator.h"

namespace vision {
using namespace ocarina;

void GIReceiver::compile() noexcept {
    auto &fb = frame_buffer();
    auto &sampler = renderer().sampler();
    auto &camera = scene().sensor();
    primary_flags_ = device().create_buffer<uint>(pipeline()->pixel_num(), "GI physical primary flags");
    Kernel kernel = [&](Uint frame) {
        camera->load_data();
        sampler->load_data();
        RenderEnv env;
        env.initial(sampler, frame, spectrum());
        sampler->set_seed(dispatch_idx().xy(), frame, Dimension::ReSTIR_RIS);
        Uint index = dispatch_id();
        auto ray = fb.rays().read(index)->to_ray_state();
        auto hit = fb.impl_type() == "normal" ? fb.cur_visibility_buffer_var(frame).read(index)
                                              : geometry().trace_closest(ray.ray);
        Uint flag = BxDFFlag::Unset;
        $if(hit->is_hit()) {
            auto it = geometry().compute_surface_interaction(hit, ray.ray);
            scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                auto bsdf = material->create_evaluator(it, env.sampled_wavelengths());
                flag = bsdf.flag();
            });
        };
        primary_flags_.write(index, flag);
    };
    kernel.function()->set_block_dim(16u, 16u, 1u);
    prepare_ = device().compile(kernel, "GI physical primary flag");
}

CommandBatch GIReceiver::dispatch(uint frame) const noexcept {
    CommandBatch batch;
    batch << prepare_(frame).dispatch(pipeline()->resolution());
    return batch;
}

}// namespace vision

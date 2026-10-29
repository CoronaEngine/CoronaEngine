//
// Created by Zero on 2023/6/12.
//

#include "base/mgr/pipeline.h"
#include "render_graph/graph.h"

namespace vision {
using namespace ocarina;

class CustomizedRenderPipeline : public Pipeline {
private:
    RenderGraph render_graph_;

public:
    explicit CustomizedRenderPipeline(const PipelineDesc &desc)
        : Pipeline(desc) {}
    VS_MAKE_PLUGIN_NAME_FUNC
    void init_project(const vision::ProjectDesc &project_desc) override {
        Pipeline::init_project(project_desc);
    }

    void init_postprocessor(const DenoiserDesc &desc) override {
        postprocessor_.set_tone_mapper(renderer_.frame_buffer()->tone_mapper());
    }

    void prepare_render_graph() noexcept override {
        SP<RenderPass> integrate = RenderPass::create("integrate");
        render_graph_.add_pass(integrate, "integrate");
        SP<RenderPass> accum = RenderPass::create("accumulate");
        render_graph_.add_pass(accum, "accumulate");
        SP<RenderPass> tonemapping = RenderPass::create("tonemapping");
        render_graph_.add_pass(tonemapping, "tonemapping");
        SP<RenderPass> gamma = RenderPass::create("gamma");
        render_graph_.add_pass(gamma, "gamma");

        render_graph_.add_edge("integrate.radiance", "accumulate.input");
        render_graph_.add_edge("accumulate.output", "tonemapping.input");
        render_graph_.add_edge("tonemapping.output", "gamma.input");
        render_graph_.mark_output("gamma.output");

        render_graph_.setup();
    }

    void after_render() noexcept override {
        Env::debugger().reset_range();
        scene().sensor()->after_render();
//        frame_buffer()->fill_window_buffer(render_graph_.output_buffer());
    }

    void prepare() noexcept override {
        Pipeline::prepare();
        auto pixel_num = resolution().x * resolution().y;
        scene().prepare();
        renderer_.prepare(scene());
        image_pool().prepare(stream());
        prepare_geometry();
        prepare_render_graph();
        upload_bindless_array();
        compile();
        preprocess();
    }

    void compile() noexcept override {
        Global::SceneGpuContextScope scene_gpu_context{
            scene().geometry().bindless_array(),
            scene().geometry().gpu_resource()->device()};
        Pipeline::compile();
        render_graph_.compile();
    }

    void render(double dt) noexcept override {
        stream() << render_graph_.dispatch() << synchronize() << commit();
        integrator()->increase_frame_index();
    }
};

}// namespace vision

VS_MAKE_CLASS_CREATOR(vision::CustomizedRenderPipeline)

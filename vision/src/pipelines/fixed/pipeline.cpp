//
// Created by Zero on 2023/6/12.
//

#include "base/mgr/pipeline.h"
#include "base/mgr/switch_profile.h"

namespace vision {

class FixedRenderPipeline : public Pipeline {
private:
    bool defer_base_compile_{false};
    bool base_compiled_{false};

public:
    explicit FixedRenderPipeline(const PipelineDesc &desc)
        : Pipeline(desc), defer_base_compile_(desc["defer_base_compile"].as_bool(false)) {}
    VS_MAKE_PLUGIN_NAME_FUNC
    void prepare() noexcept override {
        switch_profile::Scope profile{"pipeline.prepare", "resources"};
        Pipeline::prepare();
        scene().prepare();
        renderer_.prepare(scene());
        image_pool().prepare(stream());
        prepare_geometry();
        upload_bindless_array();
        if (!defer_base_compile_) {
            compile();
        }
        preprocess();
    }

    void init_project(const vision::ProjectDesc &project_desc) override {
        Pipeline::init_project(project_desc);
        init_postprocessor(project_desc.renderer_desc.denoiser_desc);
    }

    void init_postprocessor(const DenoiserDesc &desc) override {
//        postprocessor_.set_denoiser(Node::create_shared<Denoiser>(desc));
        postprocessor_.set_tone_mapper(renderer_.frame_buffer()->tone_mapper());
    }

    void compile() noexcept override {
        switch_profile::Scope profile{"pipeline.compile", "compile"};
        // Geometry synchronization can request a rebuild before any camera has
        // rendered. Keep an unused base renderer deferred on that path too.
        if (active_renderer_ == &renderer_ && defer_base_compile_) {
            base_compiled_ = false;
            return;
        }
        Global::SceneGpuContextScope scene_gpu_context{
            scene().geometry().bindless_array(),
            scene().geometry().gpu_resource()->device()};
        Pipeline::compile();
        integrator()->compile();
        if (active_renderer_ == &renderer_) {
            base_compiled_ = true;
        }
    }

    void render(double dt) noexcept override {
        // Embedded editor views own their renderers. Avoid compiling a second,
        // unused set of scene-specialized kernels on every algorithm import.
        // Standalone/base rendering remains supported on its first actual use.
        if (active_renderer_ == &renderer_ && !base_compiled_) {
            defer_base_compile_ = false;
            compile();
        }
        integrator()->render();
    }

    void commit_command() noexcept override {
        if (active_renderer_ == &renderer_ && !base_compiled_) {
            // Import, view retirement and shutdown also drain this stream. They
            // must not dispatch gamma correction from an uncompiled base view.
            activate_global_context();
            stream() << synchronize() << commit();
            return;
        }
        Pipeline::commit_command();
    }
};

}// namespace vision

VS_MAKE_CLASS_CREATOR(vision::FixedRenderPipeline)

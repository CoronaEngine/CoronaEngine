//
// Created by Zero on 2023/6/14.
//

#include "global.h"
#include "pipeline.h"
#include "hotfix/hotfix.h"

namespace vision {

OC_MAKE_INSTANCE_FUNC_DEF_WITH_HOTFIX(Global, s_global)

Global::~Global() {
    RHIContext::destroy_instance();
    ImagePool::destroy_instance();
}

void Global::set_pipeline(SP<Pipeline> pipeline) {
    pipeline_ = pipeline;
}

SP<Pipeline> Global::pipeline_shared() {
    return pipeline_.lock();
}

Global::SceneGpuContextScope::SceneGpuContextScope(BindlessArray &bindless_array,
                                                   Device &device) noexcept
    : global_(Global::instance()),
      previous_(global_.push_scene_gpu_context(bindless_array, device)) {}

Global::SceneGpuContextScope::~SceneGpuContextScope() {
    global_.restore_scene_gpu_context(previous_);
}

Pipeline *Global::pipeline() {
    return pipeline_shared().get();
}

BindlessArray &Global::bindless_array() {
    auto *rp = pipeline();
    if (rp->scene().geometry().has_gpu_resource()) {
        return rp->scene().geometry().bindless_array();
    }
    return rp->bindless_array();
}

Global::SceneGpuContext Global::push_scene_gpu_context(BindlessArray &bindless_array,
                                                       Device &device) noexcept {
    SceneGpuContext previous = scene_gpu_context_;
    scene_gpu_context_ = {.bindless_array = &bindless_array, .device = &device};
    return previous;
}

void Global::restore_scene_gpu_context(SceneGpuContext context) noexcept {
    scene_gpu_context_ = context;
}

void Global::set_scene_path(const fs::path &sp) noexcept {
    scene_path_ = sp;
}

fs::path Global::scene_path() const noexcept {
    return scene_path_;
}

Pipeline *Toolkit::pipeline() noexcept {
    return Global::instance().pipeline();
}

Device &Toolkit::device() noexcept {
    return pipeline()->device();
}

Scene &Toolkit::scene() noexcept {
    return pipeline()->scene();
}

Renderer &Toolkit::renderer() noexcept {
    return pipeline()->renderer();
}

FrameBuffer &Toolkit::frame_buffer() noexcept {
    return *pipeline()->frame_buffer();
}

TSpectrum &Toolkit::spectrum() noexcept {
    return renderer().spectrum();
}

Stream &Toolkit::stream() noexcept {
    return pipeline()->stream();
}

Geometry &Toolkit::geometry() noexcept {
    return scene().geometry();
}

GeometryData *Toolkit::geometry_data() noexcept {
    return geometry().data();
}

}// namespace vision

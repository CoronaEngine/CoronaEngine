//
// Created by Zero on 04/09/2022.
//

#include "pipeline.h"
#include "interactive_runtime_switches.h"
#include "base/sensor/photosensory.h"
#include "base/color/spectrum.h"
#include "window/window.h"
#include <memory>

namespace vision {
using namespace ocarina;

namespace {

[[nodiscard]] bool output_requires_gamma(const OutputDesc &desc) noexcept {
    return !(desc.fn.ends_with("exr") || desc.fn.ends_with("hdr"));
}

[[nodiscard]] bool should_gamma_correct_for_display(const OutputDesc &desc, const Window *window) noexcept {
    return window != nullptr || output_requires_gamma(desc);
}

}// namespace

Pipeline::Pipeline(const vision::PipelineDesc &desc)
    : Node(desc),
      device_(&Global::instance().device()),
      stream_(device().create_stream()),
      bindless_array_(device().create_bindless_array()) {}

void Pipeline::initialize_(const vision::NodeDesc &node_desc) noexcept {
    const Desc &desc = static_cast<const Desc &>(node_desc);
    Global::instance().set_pipeline(shared_from_this());
    frame_buffer_desc_ = desc.frame_buffer_desc;
    Env::printer().init(device());
    Env::debugger().init(device());
    Env::set_code_obfuscation(desc["obfuscation"].as_bool(false));
    Env::set_valid_check(desc["valid_check"].as_bool(false));
    renderer_.set_frame_buffer(Node::create_shared<FrameBuffer>(desc.frame_buffer_desc));
}

void Pipeline::activate_global_context() noexcept {
    try {
        Global::instance().set_pipeline(shared_from_this());
    } catch (const std::bad_weak_ptr &) {
    }
}

bool Pipeline::create_view_context(uint64_t view_id, uint2 resolution) noexcept {
    activate_global_context();
    if (view_id == 0u || view_contexts_.contains(view_id)) {
        return view_id != 0u;
    }

    try {
        auto context = make_unique<ViewContext>();
        context->renderer.set_frame_buffer(Node::create_shared<FrameBuffer>(frame_buffer_desc_));
        auto [it, inserted] = view_contexts_.emplace(view_id, std::move(context));
        if (!inserted || !activate_view_context(view_id)) {
            return false;
        }

        renderer().pre_init(renderer_desc_);
        renderer().init(renderer_desc_, scene_view_);
        scene_view_.sensor().init(sensor_desc_);
        frame_buffer()->update_resolution(resolution);
        frame_buffer()->prepare();
        scene_view_.sensor()->prepare();
        scene_view_.sensor()->update_resolution(resolution);
        scene_view_.sensor()->update_device_data();
        // Enable the denoiser BEFORE renderer().prepare(): PathTracingIntegrator::prepare()
        // only allocates the denoiser's buffers (and registers its GBuffer callback) when
        // the denoiser is enabled. Syncing after prepare() left a denoise-enabled view
        // context with an unprepared denoiser -> unallocated SVGF buffers -> GPU crash on
        // the first dispatch. compile() still follows prepare() (canonical order).
        sync_output_denoise();
        OC_INFO_FORMAT("Pipeline::create_view_context synced output denoise: view={} denoise={}",
                       view_id, output_desc_.denoise);
        renderer().prepare(scene_view_);
        upload_scene_bindless_array();
        frame_buffer()->prepare_view_texture();
        sync_output_denoise();
        compile();
        upload_bindless_array();
        invalidate();
        return true;
    } catch (...) {
        activate_view_context(0u);
        view_contexts_.erase(view_id);
        return false;
    }
}

bool Pipeline::activate_view_context(uint64_t view_id) noexcept {
    if (view_id == 0u) {
        active_renderer_ = &renderer_;
        scene_view_.set_sensor_override(nullptr);
        return true;
    }
    auto it = view_contexts_.find(view_id);
    if (it == view_contexts_.end()) {
        return false;
    }
    active_renderer_ = &it->second->renderer;
    scene_view_.set_sensor_override(&it->second->sensor);
    return true;
}

void Pipeline::remove_view_context(uint64_t view_id) noexcept {
    if (view_id == 0u) return;
    activate_view_context(0u);
    view_contexts_.erase(view_id);
}

void Pipeline::clear_view_contexts() noexcept {
    activate_view_context(0u);
    view_contexts_.clear();
}

void Pipeline::invalidate_view_context(uint64_t view_id) noexcept {
    if (activate_view_context(view_id)) {
        invalidate();
    }
}

void Pipeline::invalidate_all_view_contexts() noexcept {
    for (const auto &[view_id, context] : view_contexts_) {
        (void)context;
        invalidate_view_context(view_id);
    }
    activate_view_context(0u);
}

void Pipeline::rebuild_view_context_renderers() noexcept {
    for (const auto &[view_id, context] : view_contexts_) {
        (void)context;
        if (!activate_view_context(view_id)) {
            continue;
        }
        renderer().prepare_lights(scene_view_);
        upload_scene_bindless_array();
        compile();
        invalidate();
    }
    activate_view_context(0u);
}

void Pipeline::init() noexcept {
    activate_global_context();
}

void Pipeline::sync_output_denoise() noexcept {
    /// output.denoise is the per-mode switch; the integrator-owned denoiser consumes it.
    ///
    /// FORCE: SVGF is force-enabled for the path-tracing pipeline regardless of the
    /// selected CameraVisionRenderMode. PathTracing mode still instantiates an SVGF
    /// denoiser with a normal framebuffer (see configure_vision_scene_for_mode), so
    /// handing the integrator `true` here turns SVGF on for PT without touching the
    /// canonical optics-layer mode model or its unit tests. This is the single point
    /// every denoise-sync path funnels through, so it cannot be undone by upstream
    /// merges of the mode mapping. `VISION_DISABLE_DENOISER` still hard-disables at the
    /// integrator gate. NOTE: output_desc_.denoise (and the create_view_context log)
    /// keep reporting the mode's nominal value, so logs may say denoise=false while it runs.
    renderer().integrator()->set_denoise_enabled(output_desc_.denoise);
}

void Pipeline::prepare() noexcept {
    activate_global_context();
    if (!scene_view_.geometry().has_gpu_resource()) {
        scene_view_.geometry().init(device());
    }
    renderer().frame_buffer()->prepare();
}

void Pipeline::on_touch(ocarina::uint2 pos) noexcept {
    Env::debugger().set_lower(pos);
    auto *fb = renderer_.frame_buffer();
    auto &buffer = fb->hit_buffer();
    stream_ << fb->compute_hit(0, pos);
    stream_ << fb->hit_buffer().download(0, 1);
    stream_ << synchronize() << commit();
    TriangleHit hit = buffer[0];
    mark_selected(hit);
}

void Pipeline::mark_selected(TriangleHit hit) noexcept {
    if (hit.is_miss()) {
        ui().set_cur_node(scene_view_.light_manager().env_light());
        return;
    }
    ShapeInstance *instance = scene_view_.get_instance(hit.inst_id);
    ui().set_cur_node(instance);
}

void Pipeline::register_encoded_object(vision::EncodedObject *object) noexcept {
    if (std::find(encoded_objects.cbegin(), encoded_objects.cend(), object) != encoded_objects.cend()) {
        return;
    }
    encoded_objects.push_back(object);
}

void Pipeline::deregister_encoded_object(vision::EncodedObject *object) noexcept {
    erase_if(encoded_objects, [&](const EncodedObject *iter) -> bool {
        return object == iter;
    });
}

void Pipeline::offline_rendering() noexcept {
    invalidate();
}

void Pipeline::save_result() noexcept {
    OutputDesc desc = output_desc_;
    vector<float4> vec;
    vec.resize(pixel_num());
    final_picture(desc, vec.data());
    Image::save_image(Global::instance().scene_path() / desc.fn, PixelStorage::FLOAT4,
                      resolution(), vec.data());
    if (desc.save_exit) {
        printf("VISION_RENDER_TIME_MS=%.2f\n", integrator()->render_time());
        exit(0);
    }
    need_save_ = false;
}

void Pipeline::check_and_save() noexcept {
    if ((frame_index() == output_desc_.spp && output_desc_.spp != 0) || need_save_) {
        save_result();
    }
}

void Pipeline::update_runtime_object(const vision::IObjectConstructor *constructor) noexcept {
    auto &fb = renderer_.frame_buffer_sp();
    std::tuple tp = {addressof(fb)};
    HotfixSystem::replace_objects(constructor, tp);
}

void Pipeline::invalidate() noexcept {
    integrator()->invalidation();
    if (auto *fb = frame_buffer(); fb != nullptr && fb->enable_accumulation()) {
        stream_ << fb->clear_accumulation_history() << synchronize() << commit();
    }
    total_time_ = 0;
}

void Pipeline::change_resolution(uint2 res) noexcept {
    activate_global_context();
    OC_INFO_FORMAT("Pipeline::change_resolution request=({}, {}), current=({}, {})",
                   res.x, res.y, resolution().x, resolution().y);
    if (all(res == resolution())) { return; }
    frame_buffer()->update_resolution(res);
    scene_view_.sensor()->update_resolution(res);
    integrator()->update_resolution(res);
    OC_INFO_FORMAT("Pipeline::change_resolution applied framebuffer=({}, {}), raytracing=({}, {})",
                   frame_buffer()->resolution().x, frame_buffer()->resolution().y,
                   frame_buffer()->raytracing_resolution().x, frame_buffer()->raytracing_resolution().y);
    upload_scene_bindless_array();
    upload_bindless_array();
}

void Pipeline::prepare_geometry() noexcept {
    activate_global_context();
    scene_view_.update_geometry_instances();
    scene_view_.geometry().reset_device_buffer();
    scene_view_.geometry().upload(stream());
    scene_view_.geometry().build_accel(stream());
    scene_view_.geometry().upload_bindless_array(stream());
}

void Pipeline::update_geometry() noexcept {
    activate_global_context();
    scene_view_.update_geometry_instances();
    scene_view_.geometry().upload(stream());
    scene_view_.geometry().update_accel(stream());
    scene_view_.geometry().upload_bindless_array(stream());
}

void Pipeline::upload_scene_bindless_array() noexcept {
    activate_global_context();
    if (!scene_view_.geometry().has_gpu_resource()) {
        return;
    }
    scene_view_.geometry().upload_bindless_array(stream());
}

void Pipeline::clear_geometry() noexcept {
    activate_global_context();
    scene_view_.geometry().clear();
    scene_view_.clear_shapes();
    scene_view_.geometry().data()->clear_meshes();
}

void Pipeline::upload_bindless_array() noexcept {
    activate_global_context();
    stream_ << bindless_array_.update_slotSOA() << synchronize() << commit();
    stream_ << bindless_array_.upload_handles() << synchronize() << commit();
}

void Pipeline::deregister_buffer(handle_ty index) noexcept {
    bindless_array_->remove_buffer(index);
}

void Pipeline::deregister_texture3d(handle_ty index) noexcept {
    bindless_array_->remove_texture3d(index);
}

void Pipeline::deregister_texture2d(handle_ty index) noexcept {
    bindless_array_->remove_texture2d(index);
}

void Pipeline::before_render() noexcept {
    activate_global_context();
    sync_output_denoise();
    stream_ << Env::debugger().upload();
}

void Pipeline::set_output_denoise(bool denoise) noexcept {
    activate_global_context();
    output_desc_.denoise = denoise;
    sync_output_denoise();
}

void Pipeline::after_render() noexcept {
    activate_global_context();
    Env::debugger().reset_range();
    scene_view_.sensor()->after_render();
    renderer().frame_buffer()->after_render();
}

void Pipeline::upload_data() noexcept {
    activate_global_context();
    renderer().upload_data(scene_view_);
    if (scene_view_.has_changed() || renderer().has_changed()) {
        upload_scene_bindless_array();
        upload_bindless_array();
    }
    for (EncodedObject *object : encoded_objects) {
        object->update_device_data();
    }
}

void Pipeline::commit_command() noexcept {
    activate_global_context();
    if (should_gamma_correct_for_display(output_desc_, window_)) {
        stream_ << renderer().frame_buffer()->gamma_correct();
    }
    stream_ << synchronize();
    stream_ << commit();
}

void Pipeline::display(double dt) noexcept {
    activate_global_context();
    delta_time_ = dt;
    total_time_ += dt;
    Clock clk;
    before_render();
    render(dt);
    commit_command();
    after_render();
    double ms = clk.elapse_ms();
    integrator()->accumulate_render_time(ms);
#if VISION_INTERACTIVE_AUX_WORK
    Env::printer().retrieve_immediately();
#endif
}

void Pipeline::final_picture(const OutputDesc &desc, float4 *data) noexcept {
    bool gamma = output_requires_gamma(desc);
    if (window_ != nullptr) {
        window_->download_background(data);
    } else {
        frame_buffer()->download_final_picture(data);
    }
    if (!gamma && should_gamma_correct_for_display(desc, window_)) {
        auto func = [](float4 *ptr, size_t num) {
            for (size_t i = 0; i < num; ++i) {
                ptr[i] = srgb_to_linear(ptr[i]);
            }
        };
        func(data, pixel_num());
    }
}

}// namespace vision

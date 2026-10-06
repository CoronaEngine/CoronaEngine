//
// Created by Zero on 04/09/2022.
//

#pragma once

#include "rhi/common.h"
#include "scene.h"
#include "renderer.h"
#include "core/image/image.h"
#include "image_pool.h"
#include "base/sensor/frame_buffer.h"
#include "postprocessor.h"
#include "pipeline_ui.h"
#include "UI/GUI.h"
#include "base/using.h"
#include "switch_profile.h"

namespace vision {
class Window;
}

namespace vision {
class Spectrum;
class EncodedObject;

struct GeometryStats {
    uint triangle_num{};
    uint vertex_num{};
    uint mesh_num{};
};

struct BindlessStats {
    uint buffer_num{};
    uint texture_num{};
    size_t buffer_slot_mem_size{};
    size_t texture_slot_mem_size{};
};

class Pipeline : public Node, public Observer, public enable_shared_from_this<Pipeline> {
public:
    using Desc = PipelineDesc;

protected:
    struct ViewContext {
        Renderer renderer;
        TSensor sensor;
    };

    Device *device_{};
    Scene scene_view_{};
    Renderer renderer_{};
    Renderer *active_renderer_{&renderer_};
    unordered_map<uint64_t, UP<ViewContext>> view_contexts_;
    RendererDesc renderer_desc_{};
    SensorDesc sensor_desc_{};
    FrameBufferDesc frame_buffer_desc_{};
    BindlessArray bindless_array_{};
    mutable Stream stream_;
    Postprocessor postprocessor_{this};
    bool need_save_{false};
    OutputDesc output_desc_{};
    float3 light_world_center_{};
    float light_world_radius_{};

    vision::Window *window_{};

    mutable double delta_time_{};
    mutable double total_time_{};

    PipelineUI ui_{this};
    mutable vector<UP<ocarina::ShaderBase>> shaders_;
    vector<EncodedObject *> encoded_objects;

protected:
    [[nodiscard]] auto &integrator() noexcept { return active_renderer_->integrator(); }
    [[nodiscard]] auto &integrator() const noexcept { return active_renderer_->integrator(); }

public:
    explicit Pipeline(const PipelineDesc &desc);
    void activate_global_context() noexcept;
    void init() noexcept;
    void initialize_(const vision::NodeDesc &node_desc) noexcept override;
    void sync_output_denoise() noexcept;
    [[nodiscard]] const Device &device() const noexcept { return *device_; }
    [[nodiscard]] Device &device() noexcept { return *device_; }
    void bind_shared_scene_data(SP<SceneData> scene_data) noexcept {
        scene_view_.bind_shared_data(ocarina::move(scene_data));
    }
    [[nodiscard]] SP<SceneData> shared_scene_data() noexcept { return scene_view_.shared_data(); }
    [[nodiscard]] SP<const SceneData> shared_scene_data() const noexcept { return scene_view_.shared_data(); }
    [[nodiscard]] Scene &scene() noexcept { return scene_view_; }
    [[nodiscard]] const Scene &scene() const noexcept { return scene_view_; }
    [[nodiscard]] Renderer &renderer() noexcept { return *active_renderer_; }
    [[nodiscard]] const Renderer &renderer() const noexcept { return *active_renderer_; }
    [[nodiscard]] auto frame_buffer() const noexcept { return active_renderer_->frame_buffer(); }
    [[nodiscard]] auto frame_buffer() noexcept { return active_renderer_->frame_buffer(); }
    bool create_view_context(uint64_t view_id, uint2 resolution) noexcept;
    bool activate_view_context(uint64_t view_id) noexcept;
    void remove_view_context(uint64_t view_id) noexcept;
    void clear_view_contexts() noexcept;
    void invalidate_view_context(uint64_t view_id) noexcept;
    void invalidate_all_view_contexts() noexcept;
    void rebuild_view_context_renderers() noexcept;
    [[nodiscard]] bool has_view_context(uint64_t view_id) const noexcept {
        return view_contexts_.contains(view_id);
    }
    void on_touch(uint2 pos) noexcept;
    void mark_selected(TriangleHit hit) noexcept;
    void register_encoded_object(EncodedObject *object) noexcept;
    void deregister_encoded_object(EncodedObject *object) noexcept;
    void offline_rendering() noexcept;
    void save_result() noexcept;
    void check_and_save() noexcept;
    OC_MAKE_MEMBER_SETTER(window)
    OC_MAKE_MEMBER_GETTER(ui,&)
    VS_MAKE_GUI_STATUS_FUNC(Node, scene_view_, renderer_)
    /// virtual function start
    void update_runtime_object(const vision::IObjectConstructor *constructor) noexcept override;
    virtual void init_project(const ProjectDesc &project_desc) {
        switch_profile::Scope profile{"scene.init_project", "scene"};
        activate_global_context();
        output_desc_ = project_desc.output_desc;
        renderer_desc_ = project_desc.renderer_desc;
        sensor_desc_ = project_desc.scene_desc.sensor_desc;
        renderer_.pre_init(project_desc.renderer_desc);
        if (!scene_view_.geometry().has_gpu_resource()) {
            scene_view_.geometry().init(device());
        }
        scene_view_.init(project_desc.scene_desc);
        scene_view_.set_min_radius(project_desc.renderer_desc.render_setting.min_world_radius);
        light_world_center_ = scene_view_.world_center();
        light_world_radius_ = scene_view_.world_radius();
        renderer_.init(project_desc.renderer_desc, scene_view_);
        sync_output_denoise();
    };
    virtual void init_postprocessor(const DenoiserDesc &desc) = 0;
    virtual void preprocess() noexcept {}
    void prepare() noexcept override;
    virtual void change_resolution(uint2 res) noexcept;
    virtual void invalidate() noexcept;
    virtual void clear_geometry() noexcept;
    virtual void prepare_geometry(bool geometry_changed = false) noexcept;
    virtual void rebuild_geometry_gpu() noexcept;
    virtual void update_geometry() noexcept;
    void refresh_world_bounds_dependents(bool geometry_changed = false) noexcept;
    void upload_scene_bindless_array() noexcept;
    virtual void prepare_render_graph() noexcept {}
    virtual void compile() noexcept {
        switch_profile::Scope profile{"display.compile", "compile"};
        activate_global_context();
        frame_buffer()->compile();
    }
    virtual void display(double dt) noexcept;
    virtual void render(double dt) noexcept = 0;
    virtual void commit_command() noexcept;
    virtual void before_render() noexcept;
    virtual void after_render() noexcept;
    virtual void upload_data() noexcept;
    virtual void final_picture(const OutputDesc &desc, float4 *data) noexcept;
    [[nodiscard]] virtual uint2 resolution() const noexcept { return active_renderer_->frame_buffer()->resolution(); }
    [[nodiscard]] uint pixel_num() const noexcept { return active_renderer_->frame_buffer()->pixel_num(); }
    [[nodiscard]] OutputDesc &output_desc() noexcept { return output_desc_; }
    [[nodiscard]] const OutputDesc &output_desc() const noexcept { return output_desc_; }
    [[nodiscard]] uint output_spp() const noexcept { return output_desc_.spp; }
    void set_output_spp(uint spp) noexcept { output_desc_.spp = spp; }
    void set_output_save_exit(bool save_exit) noexcept { output_desc_.save_exit = save_exit; }
    void set_output_denoise(bool denoise) noexcept;
    void set_output_fn(string fn) noexcept { output_desc_.fn = std::move(fn); }
    void render_scene_ui(Widgets *widgets) noexcept { scene_view_.render_UI(widgets); }
    void render_renderer_ui(Widgets *widgets) noexcept { renderer_.render_UI(widgets); }
    void render_framebuffer_ui(Widgets *widgets) noexcept { frame_buffer()->render_UI(widgets); }
    [[nodiscard]] GeometryStats geometry_stats() const noexcept {
        return {
            .triangle_num = static_cast<uint>(geometry().accel().triangle_num()),
            .vertex_num = static_cast<uint>(geometry().accel().vertex_num()),
            .mesh_num = static_cast<uint>(geometry().accel().mesh_num())};
    }
    [[nodiscard]] BindlessStats bindless_stats() const noexcept {
        return {
            .buffer_num = bindless_array().buffer_num(),
            .texture_num = bindless_array().texture3d_num(),
            .buffer_slot_mem_size = bindless_array()->buffer_slot_size(),
            .texture_slot_mem_size = bindless_array()->tex3d_slot_size()};
    }
    [[nodiscard]] double frame_delta_ms() const noexcept { return delta_time_ * 1000.0; }
    [[nodiscard]] double avg_frame_delta_ms() const noexcept {
        return frame_index() == 0 ? 0.0 : total_time_ * 1000.0 / static_cast<double>(frame_index());
    }
    void request_save() noexcept { need_save_ = true; }
    /// virtual function end

    template<typename T>
    [[nodiscard]] CommandBatch reset_buffer(BufferView<T> buffer, T elm = T{},
                                           string desc = "clear_buffer") const noexcept {
        static Kernel kernel = [](BufferVar<T> buffer_var, Var<T> value) {
            buffer_var.write(dispatch_id(), value);
        };
        using shader_t = decltype(device().compile(kernel, desc));
        // A compiled shader belongs to this pipeline/device. A static raw
        // pointer survives its owning pipeline and dangles after mode retirement.
        shader_t *shader = nullptr;
        for (const auto &candidate : shaders_) {
            shader = dynamic_cast<shader_t *>(candidate.get());
            if (shader) break;
        }
        if (!shader) {
            auto owned_shader = make_unique<shader_t>(device().compile(kernel, desc));
            shader = owned_shader.get();
            shaders_.push_back(std::move(owned_shader));
        }
        CommandBatch ret;
        ret << (*shader)(buffer, elm).dispatch(buffer.size());
        return ret;
    }

    [[nodiscard]] virtual uint frame_index() const noexcept { return integrator()->frame_index(); }
    [[nodiscard]] double render_time() const noexcept { return integrator()->render_time(); }
    [[nodiscard]] double cur_render_time() const noexcept { return integrator()->cur_render_time(); }
    static void flip_debugger() noexcept { Env::debugger().filp_enabled(); }
    void deregister_buffer(handle_ty index) noexcept;
    void deregister_texture3d(handle_ty index) noexcept;
    void deregister_texture2d(handle_ty index) noexcept;
    [[nodiscard]] ImagePool &image_pool() noexcept { return scene_view_.image_pool(); }
    [[nodiscard]] BindlessArray &bindless_array() noexcept { return bindless_array_; }
    [[nodiscard]] const BindlessArray &bindless_array() const noexcept { return bindless_array_; }
    void upload_bindless_array() noexcept;
    [[nodiscard]] Geometry &geometry() noexcept { return scene_view_.geometry(); }
    [[nodiscard]] const Geometry &geometry() const noexcept { return scene_view_.geometry(); }
    [[nodiscard]] Stream &stream() const noexcept { return stream_; }
};

}// namespace vision

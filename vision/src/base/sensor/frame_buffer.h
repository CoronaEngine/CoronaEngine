//
// Created by Zero on 2024/2/17.
//

#pragma once

#include "dsl/dsl.h"
#include "base/node.h"
#include "sensor.h"
#include "tonemapper.h"
#include "base/scattering/interaction.h"
#include "visualizer.h"
#include "upsampler.h"
#include "base/using.h"

namespace vision {
}// namespace vision

namespace vision {

/// use for pingpong
template<typename T>
requires is_integral_expr_v<T>
[[nodiscard]] inline T prev_index(const T &frame_index) noexcept { return frame_index & 1; }
template<typename T>
requires is_integral_expr_v<T>
[[nodiscard]] inline T cur_index(const T &frame_index) noexcept { return (frame_index + 1) & 1; }

template<typename TPixel, typename Func>
requires is_general_int_vector2_v<remove_device_t<TPixel>>
void foreach_neighbor(const TPixel &pixel, Func func, const Int2 &radius = make_int2(1)) {
    Int2 cur_pixel = make_int2(pixel);
    Int2 res = make_int2(dispatch_dim().xy());
    Int x_start = cur_pixel.x - radius.x;
    x_start = max(0, x_start);
    Int x_end = cur_pixel.x + radius.x;
    x_end = min(x_end, res.x - 1);
    Int y_start = cur_pixel.y - radius.y;
    y_start = max(0, y_start);
    Int y_end = cur_pixel.y + radius.y;
    y_end = min(y_end, res.y - 1);
    $for(x, x_start, x_end + 1) {
        $for(y, y_start, y_end + 1) {
            func(make_int2(x, y));
        };
    };
}

class Sensor;

class ScreenBuffer : public RegistrableManaged<float4>,
                     public enable_shared_from_this<ScreenBuffer> {
public:
    using manager_type = ocarina::map<string, SP<ScreenBuffer>>;
    using RegistrableManaged<float4>::RegistrableManaged;
    using Super = RegistrableManaged<float4>;

public:
    ScreenBuffer() = default;
    explicit ScreenBuffer(string key) : Super() {
        name_ = std::move(key);
    }
    [[nodiscard]] const Super &super() const { return *this; }
    [[nodiscard]] Super &super() { return *this; }
    void update_resolution(uint2 res, Device &device) noexcept;
};
}// namespace vision

namespace vision {
struct RayData {
    float4 org_ior{};
    float4 dir_medium{};
};
}// namespace vision
// clang-format off
OC_STRUCT(vision, RayData, org_ior,dir_medium) {
    [[nodiscard]] Float3 origin() const noexcept { return org_ior.xyz(); }
    void set_origin(const Float3 &val) noexcept { org_ior.xyz() = val; }
    [[nodiscard]] Float3 direction() const noexcept { return dir_medium.xyz(); }
    void set_direction(const Float3 &val) noexcept { dir_medium.xyz() = val; }
    [[nodiscard]] Float ior() const noexcept { return org_ior.w; }
    void set_ior(const Float &ior) noexcept { org_ior.w = ior; }
    [[nodiscard]] Bool in_medium() const noexcept { return medium_id() != InvalidUI32; }
    [[nodiscard]] Uint medium_id() const noexcept { return as<uint>(dir_medium.w); }
    void set_medium(const Uint &id) noexcept { dir_medium.w = as<float>(id); }
    [[nodiscard]] RayVar to_ray() const noexcept { return ocarina::make_ray(origin(), direction()); }
    void from_ray(const RayVar &ray) noexcept {
        set_origin(ray->origin());
        set_direction(ray->direction());
    }
    [[nodiscard]] vision::RayState to_ray_state() const noexcept {
        return {.ray = to_ray(), .ior = ior(), .medium = medium_id()};
    }
    void from_ray_state(const vision::RayState &rs) noexcept {
        from_ray(rs.ray);
        set_ior(rs.ior);
        set_medium(rs.medium);
    }
};
// clang-format on

namespace vision {
struct GBufferParam {
    uint frame_index{};
    uint camera_jitter{1u};
    BufferDesc<TriangleHit> visibility_buffer;
    BufferDesc<float2> motion_vectors;
    BufferDesc<RayData> rays;
};
}// namespace vision

OC_PARAM_STRUCT(vision, GBufferParam, frame_index, camera_jitter, visibility_buffer, motion_vectors, rays){};

namespace vision {

class GBufferCallback {
public:
    virtual void compute_GBuffer(const RayState &rs, const Interaction &it) noexcept = 0;
};

class FrameBuffer : public Node, public GUIRenderable, public EncodedObject, public Observer {
public:
    static constexpr auto final_result = "FrameBuffer::final_result";

protected:
    template<typename T>
    [[nodiscard]] static size_t device_size(const RegistrableBuffer<T> &buffer) {
        return buffer.size();
    }

    template<typename T>
    [[nodiscard]] static size_t device_size(const RegistrableManaged<T> &buffer) {
        return buffer.device_buffer().size();
    }

protected:
    Shader<void(GBufferParam)> compute_geom_;
    Shader<void(Buffer<TriangleHit>, uint2, uint)> compute_hit_;
    Shader<void(Buffer<float4>, Buffer<float4>, uint)> accumulate_;
    Shader<void(Buffer<float4>, Texture2D, float)> tone_mapping_;

protected:
    string cur_view_{final_result};
    ScreenBuffer::manager_type screen_buffers_;
    Shader<void(Texture2D, Texture2D)> gamma_correct_;

    vector<weak_ptr<GBufferCallback>> gbuffer_callbacks_;

    SP<Visualizer> visualizer_{make_shared<Visualizer>()};

    vector<float4> window_buffer_;

    uint2 resolution_;
    Box2f screen_window_;
    EncodedData<uint> accumulation_;
    TToneMapper tone_mapper_{};
    SP<Upsampler> upsampler_{};
    EncodedData<float> exposure_{};

#define VS_MAKE_BUFFER(Type, buffer_name, count)                            \
protected:                                                                  \
    Type buffer_name##_;                                                    \
                                                                            \
public:                                                                     \
    OC_MAKE_MEMBER_GETTER(buffer_name, &)                                   \
    void prepare_##buffer_name() noexcept {                                 \
        init_buffer(buffer_name##_, "FrameBuffer::" #buffer_name, count);   \
    }                                                                       \
    void reset_##buffer_name() noexcept {                                   \
        reset_buffer(buffer_name##_, "FrameBuffer::" #buffer_name, count);  \
    }                                                                       \
    [[nodiscard]] uint buffer_name##_base() const noexcept {                \
        return buffer_name##_.index().hv();                                 \
    }                                                                       \
    [[nodiscard]] size_t buffer_name##_size() const noexcept {              \
        return device_size(buffer_name##_);                                 \
    }                                                                       \
    void destroy_##buffer_name() noexcept {                                 \
        buffer_name##_ = {};                                                \
    }                                                                       \
    void auto_manage_##buffer_name(bool cond) {                             \
        size_t size = buffer_name##_size();                                 \
        if (cond && size == 0) {                                            \
            prepare_##buffer_name();                                        \
        } else if (!cond && size > 0) {                                     \
            destroy_##buffer_name();                                        \
        }                                                                   \
    }                                                                       \
    [[nodiscard]] ByteBufferView buffer_name##_byte_view() const noexcept { \
        return ByteBufferView(buffer_name##_.view());                       \
    }

    // Host-dispatched kernels need a pointer to the selected half, with offset
    // already applied. The bindless host view loses its slot's byte offset, and
    // CUDA parameter-buffer indexing does not apply a descriptor offset either.
    // Keep each view bounded to one frame so writes cannot alias previous data.
#define VS_MAKE_DOUBLE_BUFFER(Type, buffer_name)                                                                              \
    VS_MAKE_BUFFER(Type, buffer_name, 2)                                                                                      \
    template<typename T>                                                                                                      \
    requires is_integral_expr_v<T>                                                                                            \
    [[nodiscard]] T prev_##buffer_name##_index(const T &frame_index) const noexcept {                                         \
        return prev_index(frame_index) + buffer_name##_base();                                                                \
    }                                                                                                                         \
    template<typename T>                                                                                                      \
    requires is_integral_expr_v<T>                                                                                            \
    [[nodiscard]] T cur_##buffer_name##_index(const T &frame_index) const noexcept {                                          \
        return cur_index(frame_index) + buffer_name##_base();                                                                 \
    }                                                                                                                         \
    [[nodiscard]] auto prev_##buffer_name##_view(uint frame_index) const noexcept {                                           \
        auto view = buffer_name##_.view().subview(prev_index(frame_index) * frame_buffer_size(), frame_buffer_size());        \
        return decltype(view)(view.handle() + view.offset_in_byte(), view.size());                                             \
    }                                                                                                                         \
    [[nodiscard]] auto cur_##buffer_name##_view(uint frame_index) const noexcept {                                            \
        auto view = buffer_name##_.view().subview(cur_index(frame_index) * frame_buffer_size(), frame_buffer_size());         \
        return decltype(view)(view.handle() + view.offset_in_byte(), view.size());                                             \
    }                                                                                                                         \
    [[nodiscard]] auto prev_##buffer_name##_var(const Uint &frame_index) const noexcept {                                     \
        return bindless_array().buffer_var<decltype(buffer_name##_)::element_type>(prev_##buffer_name##_index(frame_index));  \
    }                                                                                                                         \
    [[nodiscard]] auto cur_##buffer_name##_var(const Uint &frame_index) const noexcept {                                      \
        return bindless_array().buffer_var<decltype(buffer_name##_)::element_type>(cur_##buffer_name##_index(frame_index));   \
    }                                                                                                                         \
    [[nodiscard]] auto prev_##buffer_name##_byte_view(uint frame_index) const noexcept {                                      \
        return ByteBufferView(prev_##buffer_name##_view(frame_index));                                                        \
    }                                                                                                                         \
    [[nodiscard]] auto cur_##buffer_name##_byte_view(uint frame_index) const noexcept {                                       \
        return ByteBufferView(cur_##buffer_name##_view(frame_index));                                                         \
    }

    /// save two frames of data , use for ReSTIR
    VS_MAKE_DOUBLE_BUFFER(RegistrableBuffer<SurfaceData>, surfaces)
    VS_MAKE_DOUBLE_BUFFER(RegistrableBuffer<SurfaceExtend>, surface_exts)
    VS_MAKE_BUFFER(RegistrableBuffer<float2>, motion_vectors, 1)
    VS_MAKE_BUFFER(RegistrableBuffer<HitBSDF>, hit_bsdfs, 1)
    VS_MAKE_BUFFER(RegistrableManaged<float4>, rt_buffer, 1)
    VS_MAKE_BUFFER(RegistrableManaged<RadType4>, direct_lighting, 1)
    VS_MAKE_BUFFER(RegistrableManaged<RadType4>, indirect_lighting, 1)
    VS_MAKE_BUFFER(RegistrableManaged<float4>, accumulation_buffer, 1)
    VS_MAKE_BUFFER(RegistrableBuffer<RayData>, rays, 1)
    VS_MAKE_DOUBLE_BUFFER(RegistrableBuffer<TriangleHit>, visibility_buffer)
    /// used for editor
    /// Display in full screen on the screen

    Texture2D view_texture_;

    RegistrableManaged<TriangleHit> hit_buffer_;
    OC_MAKE_MEMBER_GETTER(hit_buffer, &)

#undef VS_MAKE_DOUBLE_BUFFER

#undef VS_MAKE_BUFFER

public:
    using Desc = FrameBufferDesc;

public:
    FrameBuffer() = default;
    explicit FrameBuffer(const FrameBufferDesc &desc);
    VS_HOTFIX_MAKE_RESTORE(Node, cur_view_, surfaces_, surface_exts_, hit_bsdfs_,
                           motion_vectors_, hit_buffer_, screen_buffers_,
                           view_texture_, visualizer_, window_buffer_, rt_buffer_,
                           tone_mapper_, upsampler_,
                           /// shaders
                           compute_geom_, compute_hit_,
                           accumulate_)
    OC_ENCODABLE_FUNC(EncodedObject, accumulation_, tone_mapper_, exposure_)
    VS_MAKE_GUI_STATUS_FUNC(Node, upsampler_, tone_mapper_)
    void prepare() noexcept override;
    [[nodiscard]] Float4 apply_exposure(const Float4 &input) const noexcept;
    [[nodiscard]] Float4 apply_exposure(const Float &exposure,
                                        const Float4 &input) const noexcept;
    [[nodiscard]] virtual RayState custom_generate_ray(const Sensor *sensor,
                                                       const SensorSample &ss) const noexcept = 0;
    void init_hit_buffer() noexcept;
    void update_screen_window() noexcept;
    OC_MAKE_MEMBER_GETTER(screen_window, )
    OC_MAKE_MEMBER_GETTER(tone_mapper, &)
    OC_MAKE_MEMBER_GETTER(upsampler, )
    void prepare_view_texture(uint external_handle = InvalidUI32);
    [[nodiscard]] const Texture2D &view_texture() const noexcept { return view_texture_; }
    void register_callback(const SP<GBufferCallback> &cb) noexcept;
    void deregister_callback(const SP<GBufferCallback> &cb) noexcept;
    void update_runtime_object(const IObjectConstructor *constructor) noexcept override;
    bool render_UI(Widgets *widgets) noexcept override;
    void render_sub_UI(Widgets *widgets) noexcept override;
    [[nodiscard]] bool enable_accumulation() const noexcept { return accumulation_.hv(); }
    void set_enable_accumulation(bool enable) noexcept { accumulation_ = enable; }
    OC_MAKE_MEMBER_GETTER(visualizer, &)
    OC_MAKE_MEMBER_GETTER(window_buffer, &)
    void fill_window_buffer(const Texture2D &input) noexcept;
    void download_final_picture(float4 *data) const noexcept;
    void resize(uint2 res) noexcept;
    virtual void update_resolution(uint2 res) noexcept;
    [[nodiscard]] uint pixel_num() const noexcept;
    [[nodiscard]] uint2 resolution() const noexcept;
    [[nodiscard]] uint pixel_index(uint2 pos) const noexcept;
    [[nodiscard]] BindlessArray &bindless_array() const noexcept;
    [[nodiscard]] const Buffer<float4> &cur_screen_buffer() const noexcept;
    [[nodiscard]] virtual uint2 raytracing_resolution() const noexcept { return resolution_; }
    [[nodiscard]] virtual uint frame_buffer_size() const noexcept {
        uint2 dim = raytracing_resolution();
        return dim.x * dim.y;
    }
    void register_(const SP<ScreenBuffer> &buffer) noexcept;
    void unregister(const SP<ScreenBuffer> &buffer) noexcept;
    void unregister(const string &name) noexcept;
    void init_screen_buffer(const SP<ScreenBuffer> &buffer) noexcept;
    void prepare_screen_buffer(const SP<ScreenBuffer> &buffer) noexcept;

    [[nodiscard]] BindlessArray &bindless_array() noexcept;
    void after_render() noexcept;
    [[nodiscard]] static Float2 compute_motion_vec(const TSensor &camera, const Float2 &p_film, const Float3 &cur_pos,
                                                   const Bool &is_hit) noexcept;
    [[nodiscard]] Float3 compute_motion_vector(const TSensor &camera, const Float2 &p_film, const Uint &frame_index) const noexcept;
    [[nodiscard]] Float3 compute_motion_vector(const TSensor &camera, const Float3 &cur_pos, const Float3 &pre_pos) const noexcept;
    [[nodiscard]] static Uint checkerboard_value(const Uint2 &coord) noexcept;
    [[nodiscard]] static Uint checkerboard_value(const Uint2 &coord, const Uint &frame_index) noexcept;

    virtual void compile() noexcept;
    void compile_compute_geom() noexcept;
    void compile_compute_hit() noexcept;
    void compile_gamma() noexcept;
    void compile_accumulation() noexcept;
    void compile_tone_mapping() noexcept;
    void update_device_data() noexcept override;
    [[nodiscard]] CommandBatch gamma_correct(const Texture2D &input,
                                            const Texture2D &output) const noexcept;
    [[nodiscard]] virtual CommandBatch gamma_correct() const noexcept;
    [[nodiscard]] virtual CommandBatch compute_geom(const GBufferParam &param) const noexcept;
    [[nodiscard]] virtual CommandBatch compute_GBuffer(const GBufferParam &param) const noexcept;
    [[nodiscard]] virtual CommandBatch compute_GBuffer(uint frame_index) const noexcept;
    [[nodiscard]] virtual CommandBatch compute_hit(uint frame_index, uint2 pixel) const noexcept;
    [[nodiscard]] CommandBatch accumulate(BufferView<float4> input, BufferView<float4> output,
                                         uint frame_index) const noexcept;
    [[nodiscard]] CommandBatch tone_mapping(BufferView<float4> input,
                                           const Texture2D &output) const noexcept;
    /// Post-processing hook after path tracing, before accumulation/tone mapping
    /// Override in subclasses (e.g. LightFieldFrameBuffer) for custom post-processing
    [[nodiscard]] virtual CommandBatch post_path_tracing(uint frame_index) const noexcept {
        return {};
    }
    /// Check if this frame buffer requires custom post-processing pipeline
    [[nodiscard]] virtual bool has_custom_post_processing() const noexcept { return false; }
    /// Get the buffer to use for accumulation/tone_mapping after post-processing
    [[nodiscard]] virtual BufferView<float4> post_processed_buffer() const noexcept {
        return rt_buffer_.view();
    }
    /// Linear pre-tonemap source for external display bridges.
    [[nodiscard]] virtual BufferView<float4> display_source_buffer() const noexcept {
        return enable_accumulation() ? accumulation_buffer_.view() : rt_buffer_.view();
    }
    [[nodiscard]] virtual CommandBatch clear_accumulation_history() const noexcept;
    /// Final presentation stage for this frame buffer.
    /// Default: accumulate rt_buffer -> accumulation_buffer, then tone map -> output_buffer.
    /// Override in subclasses (e.g. LightFieldFrameBuffer) when the display resolution or
    /// the post-processing pipeline differs from the default framebuffer resolution().
    [[nodiscard]] virtual CommandBatch render_final(uint frame_index) const noexcept;
    Float3 add_sample(const Uint2 &pixel, Float4 val, const Uint &frame_index) noexcept;
    Float3 add_sample(const Uint2 &pixel, const Float3 &val, const Uint &frame_index) noexcept {
        return add_sample(pixel, make_float4(val, 1.f), frame_index);
    }
    template<typename T>
    void init_buffer_impl(RegistrableBuffer<T> &buffer, bool has_register, const string &desc, uint count = 1) noexcept {
        uint element_num = count * frame_buffer_size();
        buffer.super() = device().create_buffer<T>(element_num, desc);
        vector<T> vec{};
        vec.assign(element_num, T{});
        buffer.upload_immediately(vec.data());
        buffer.set_bindless_array(bindless_array());
        buffer.register_self();
        for (int i = 1; i < count; ++i) {
            if (has_register) {
                buffer.register_view_index(i, frame_buffer_size() * i, frame_buffer_size());
            } else {
                buffer.register_view(frame_buffer_size() * i, frame_buffer_size());
            }
        }
    }

    template<typename T>
    void init_buffer(RegistrableBuffer<T> &buffer, const string &desc, uint count = 1) noexcept {
        init_buffer_impl(buffer, false, desc, count);
    }

    template<typename T>
    void reset_buffer(RegistrableBuffer<T> &buffer, const string &desc, uint count = 1) noexcept {
        if (buffer.size() == 0) {
            return;
        }
        init_buffer_impl(buffer, true, desc, count);
    }

    template<typename T>
    void init_buffer_impl(RegistrableManaged<T> &buffer, bool has_register, const string &desc, uint count = 1) noexcept {
        uint element_num = count * frame_buffer_size();
        buffer.reset_all(device(), element_num, desc);
        buffer.host_buffer().assign(element_num, T{});
        buffer.upload_immediately();
        buffer.set_bindless_array(bindless_array());
        buffer.register_self();
        for (int i = 1; i < count; ++i) {
            if (has_register) {
                buffer.register_view_index(i, frame_buffer_size() * i, frame_buffer_size());
            } else {
                buffer.register_view(frame_buffer_size() * i, frame_buffer_size());
            }
        }
    }

    template<typename T>
    void init_buffer(RegistrableManaged<T> &buffer, const string &desc, uint count = 1) noexcept {
        init_buffer_impl(buffer, false, desc, count);
    }

    template<typename T>
    void reset_buffer(RegistrableManaged<T> &buffer, const string &desc, uint count = 1) noexcept {
        if (buffer.device_buffer().size() == 0) {
            return;
        }
        init_buffer_impl(buffer, true, desc, count);
    }
};

}// namespace vision


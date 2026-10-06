//
// Created by Zero on 2024/2/18.
//

#include "frame_buffer.h"
#include "base/mgr/pipeline.h"
#include "base/mgr/switch_profile.h"

namespace vision {
using namespace ocarina;

void ScreenBuffer::update_resolution(ocarina::uint2 res, Device &device) noexcept {
    super().reset_all(device, res.x * res.y, name_);
    register_self();
}

FrameBuffer::FrameBuffer(const vision::FrameBufferDesc &desc)
    : Node(desc),
      tone_mapper_(desc.tone_mapper),
      resolution_(desc["resolution"].as_uint2(make_uint2(1280, 720))),
      screen_window_(make_float2(-1.f), make_float2(1.f)),
      exposure_(desc["exposure"].as_float(1.f)),
      accumulation_(desc["accumulation"].as_bool(true)),
      upsampler_(Node::create_shared<Upsampler>(desc.upsampler_desc)) {
    visualizer_->init();
    update_screen_window();
    resize(resolution_);
}

void FrameBuffer::prepare() noexcept {
    encode_data();
    datas().reset_device_buffer_immediately(device(), "FrameBuffer::encoded_data");
    datas().register_self();
    datas().upload_immediately();
    auto_manage_accumulation_buffer(accumulation_.hv());
    init_hit_buffer();
    prepare_rt_buffer();
    prepare_rays();
}

void FrameBuffer::init_hit_buffer() noexcept {
    hit_buffer_.reset_all(device(), 1, "FrameBuffer::hit_buffer");
}

void FrameBuffer::prepare_view_texture(uint external_handle) {
    if (external_handle == InvalidUI32) {
        view_texture_ = device().create_texture2d(resolution().xy(), PixelStorage::FLOAT4,
                                                  "FrameBuffer::view_texture_");
    } else {
        view_texture_ = device().create_texture2d_from_external(external_handle, "texture from external");
    }
}

void FrameBuffer::update_runtime_object(const vision::IObjectConstructor *constructor) noexcept {
    std::tuple tp = {addressof(visualizer_), addressof(tone_mapper_.impl()), addressof(upsampler_)};
    HotfixSystem::replace_objects(constructor, tp);
}

bool FrameBuffer::render_UI(Widgets *widgets) noexcept {
    bool ret = widgets->use_folding_header(
        ocarina::format("{} FrameBuffer", impl_type().data()),
        [&] {
            render_sub_UI(widgets);
        });
    ret |= visualizer_->render_UI(widgets);
    return ret;
}

void FrameBuffer::render_sub_UI(Widgets *widgets) noexcept {
    auto show_buffer = [&](Managed<float4> &buffer) {
        if (buffer.device_buffer().size() == 0) {
            return;
        }
        if (widgets->radio_button(buffer.name(), cur_view_ == buffer.name())) {
            cur_view_ = buffer.name();
        }
    };
    changed_ |= widgets->drag_float("exposure", &exposure_.hv(), 0.01f, 0.f, 10.f);
    auto addr = reinterpret_cast<bool *>(&accumulation_.hv());
    changed_ |= widgets->check_box("accumulation", addr);
    auto_manage_accumulation_buffer(accumulation_.hv());
    tone_mapper_->render_UI(widgets);
    for (auto iter = screen_buffers_.begin();
         iter != screen_buffers_.end(); ++iter) {
        show_buffer(*iter->second);
    }
}

Float4 FrameBuffer::apply_exposure(const Float &exposure, const Float4 &input) const noexcept {
    return 1.f - exp(-input * exposure);
}

void FrameBuffer::register_callback(const SP<vision::GBufferCallback> &cb) noexcept {
    gbuffer_callbacks_.push_back(cb);
}

void FrameBuffer::deregister_callback(const SP<vision::GBufferCallback> &cb) noexcept {
    auto iter = std::find_if(gbuffer_callbacks_.begin(), gbuffer_callbacks_.end(), [&](const weak_ptr<GBufferCallback> &elm) {
        return elm.lock().get() == cb.get();
    });
    if (iter != gbuffer_callbacks_.cend()) {
        gbuffer_callbacks_.erase(iter);
    }
}

Float4 FrameBuffer::apply_exposure(const Float4 &input) const noexcept {
    return apply_exposure(*exposure_, input);
}

void FrameBuffer::update_screen_window() noexcept {
    float ratio = resolution_.x * 1.f / resolution_.y;
    if (ratio > 1.f) {
        screen_window_.lower = make_float2(-ratio, -1.f);
        screen_window_.upper = make_float2(ratio, 1.f);
    } else {
        screen_window_.lower = make_float2(-1.f, -1.f / ratio);
        screen_window_.upper = make_float2(1.f, 1.f / ratio);
    }
}

void FrameBuffer::init_screen_buffer(const SP<ScreenBuffer> &buffer) noexcept {
    buffer->reset_all(device(), pixel_num(), buffer->name());
    vector<float4> vec{};
    vec.assign(pixel_num(), float4{});
    buffer->set_bindless_array(bindless_array());
    buffer->register_self();
}

void FrameBuffer::prepare_screen_buffer(const SP<vision::ScreenBuffer> &buffer) noexcept {
    switch_profile::Scope profile{"FrameBuffer::prepare_screen_buffer", "buffers"};
    init_screen_buffer(buffer);
    register_(buffer);
}

void FrameBuffer::compile_accumulation() noexcept {
    switch_profile::Scope profile{"RGBFilm-accumulation.compile", "compile"};
    Kernel kernel = [&](BufferVar<float4> input, BufferVar<float4> output, Uint frame_index) {
        Float4 accum_prev = output.read(dispatch_id());
        Float4 val = input.read(dispatch_id());
        Float a = 1.f / (frame_index + 1);
        val = lerp(make_float4(a), accum_prev, val);
        output.write(dispatch_id(), val);
    };
    accumulate_ = device().compile(kernel, "RGBFilm-accumulation");
}

void FrameBuffer::update_device_data() noexcept {
    if (has_changed()) {
        update_data();
        EncodedObject::upload_immediately();
    }
}

void FrameBuffer::compile_tone_mapping() noexcept {
    switch_profile::Scope profile{"RGBFilm-tone_mapping-tex.compile", "compile"};
    Kernel kernel_tex = [&](BufferVar<float4> input, Texture2DVar output, Float exposure) {
        Float4 val = input.read(dispatch_id());
        val = apply_exposure(exposure, val);
        val = tone_mapper_->apply(val);
        val.w = 1.f;
        output.write(val, dispatch_idx().xy());
    };
    tone_mapping_ = device().compile(kernel_tex, "RGBFilm-tone_mapping-tex");
}

void FrameBuffer::compile_gamma() noexcept {
    switch_profile::Scope profile{"FrameBuffer-gamma_correction-tex.compile", "compile"};
    Kernel kernel_tex = [&](Texture2DVar input, Texture2DVar output) {
        Float4 val = input.read<float4>(dispatch_idx().xy());
        val = linear_to_srgb(val);
        val.w = 1.f;
        output.write(val, dispatch_idx().xy());
    };
    gamma_correct_ = device().compile(kernel_tex, "FrameBuffer-gamma_correction-tex");
}

void FrameBuffer::compile_compute_geom() noexcept {
    TSensor &camera = scene().sensor();
    TSampler &sampler = renderer().sampler();
    TLightSampler &light_sampler = renderer().light_sampler();
    Kernel kernel = [&](Var<GBufferParam> param) {
        auto &frame_index = param.frame_index;
        auto &vbuffer = param.visibility_buffer;
        auto &motion_vectors = param.motion_vectors;
        auto &rays = param.rays;
        RenderEnv render_env;
        render_env.initial(sampler, frame_index, spectrum());
        Uint2 pixel = dispatch_idx().xy();
        sampler->load_data();
        sampler->set_seed(make_uint2(0, 0), frame_index, 0);
        camera->load_data();

        SensorSample ss = sampler->sensor_sample(pixel, camera->filter(), param.camera_jitter != 0u);

        sampler->set_seed(pixel, frame_index, 0);

        RayState rs = custom_generate_ray(camera.get(), ss);
        TriangleHitVar hit = pipeline()->geometry().trace_closest(rs.ray);
        RayDataVar ray_data;
        ray_data->from_ray_state(rs);
        rays.write(dispatch_id(), ray_data);

        // Write hit to visibility buffer
        vbuffer.write(dispatch_id(), hit);

        Float2 motion_vec = make_float2(0.f);

        $if(hit->is_hit()) {
            Interaction it = pipeline()->geometry().compute_surface_interaction(hit, rs.ray, true);
            for (auto &cb : gbuffer_callbacks_) {
                cb.lock()->compute_GBuffer(rs, it);
            }
            motion_vec = compute_motion_vec(camera, ss.p_film, it.pos, true);
        };

        motion_vectors.write(dispatch_id(), motion_vec);
    };
    compute_geom_ = device().compile(kernel, "rt_geom");
}

void FrameBuffer::compile_compute_hit() noexcept {
    TSensor &camera = scene().sensor();
    TSampler &sampler = renderer().sampler();
    Kernel kernel = [&](BufferVar<TriangleHit> hit_buffer, Uint2 pixel, Uint frame_index) {
        camera->load_data();
        sampler->set_seed(pixel, frame_index, 0);
        SensorSample ss = sampler->sensor_sample(pixel, camera->filter());
        RayState rs = camera->generate_ray(ss);
        TriangleHitVar hit = pipeline()->geometry().trace_closest(rs.ray);
        hit_buffer.write(0, hit);
    };
    compute_hit_ = device().compile(kernel, "FrameBuffer_compute_hit_");
}

void FrameBuffer::compile() noexcept {
    compile_gamma();
    compile_compute_geom();
    compile_compute_hit();
    compile_accumulation();
    compile_tone_mapping();
}

CommandBatch FrameBuffer::compute_hit(uint frame_index, uint2 pixel) const noexcept {
    CommandBatch ret;
    ret << compute_hit_(hit_buffer_, pixel, frame_index).dispatch(1);
    return ret;
}

CommandBatch FrameBuffer::compute_geom(const vision::GBufferParam &param) const noexcept {
    CommandBatch ret;
    ret << compute_geom_(param).dispatch(resolution());
    return ret;
}

CommandBatch FrameBuffer::compute_GBuffer(const GBufferParam &param) const noexcept {
    CommandBatch ret;
    ret << compute_geom(param);
    return ret;
}

CommandBatch FrameBuffer::compute_GBuffer(uint frame_index) const noexcept {
    GBufferParam param;

    auto vbuffer = cur_visibility_buffer_view(frame_index).descriptor();

    param.frame_index = frame_index;
    param.camera_jitter = renderer().integrator()->jitter_primary_samples();
    param.visibility_buffer = vbuffer;
    param.motion_vectors = motion_vectors().descriptor();
    param.rays = rays().descriptor();

    return compute_GBuffer(param);
}

CommandBatch FrameBuffer::gamma_correct(const ocarina::Texture2D &input,
                                       const ocarina::Texture2D &output) const noexcept {
    CommandBatch ret;
    ret << gamma_correct_(input,
                              output)
               .dispatch(resolution());
    return ret;
}

CommandBatch FrameBuffer::accumulate(BufferView<float4> input, BufferView<float4> output,
                                    uint frame_index) const noexcept {
    CommandBatch ret;
    ret << accumulate_(input,
                       output,
                       frame_index)
               .dispatch(resolution());
    return ret;
}

CommandBatch FrameBuffer::tone_mapping(BufferView<ocarina::float4> input, const ocarina::Texture2D &output) const noexcept {
    CommandBatch ret;
    ret << tone_mapping_(input,
                             output, exposure_.hv())
               .dispatch(resolution());
    return ret;
}

CommandBatch FrameBuffer::render_final(uint frame_index) const noexcept {
    CommandBatch ret;
    if (accumulation_.hv()) {
        ret << accumulate(rt_buffer_.view(), accumulation_buffer_.view(), frame_index);
        ret << tone_mapping(accumulation_buffer_.view(), view_texture_);
    } else {
        ret << tone_mapping(rt_buffer_.view(), view_texture_);
    }
    return ret;
}

CommandBatch FrameBuffer::clear_accumulation_history() const noexcept {
    CommandBatch ret;
    if (accumulation_buffer_.device_buffer().size() != 0) {
        ret << pipeline()->reset_buffer(accumulation_buffer_.view(), make_float4(0.f),
                                        "FrameBuffer_clear_accumulation_history");
    }
    return ret;
}

CommandBatch FrameBuffer::gamma_correct() const noexcept {
    return gamma_correct(view_texture_, view_texture_);
}

Float3 FrameBuffer::add_sample(const Uint2 &pixel, Float4 val, const Uint &frame_index) noexcept {
    Uint index = dispatch_id(pixel);
    val = Env::instance().zero_if_nan_inf(val);
    rt_buffer_.write(index, val);
    return val.xyz();
}

void FrameBuffer::register_(const SP<ScreenBuffer> &buffer) noexcept {
    auto iter = screen_buffers_.find(buffer->name());
    if (iter != screen_buffers_.end()) {
        OC_ERROR("");
    }
    screen_buffers_.insert(std::make_pair(buffer->name(), buffer));
}

void FrameBuffer::unregister(const SP<ScreenBuffer> &buffer) noexcept {
    unregister(buffer->name());
}

void FrameBuffer::unregister(const std::string &name) noexcept {
    auto iter = screen_buffers_.find(name);
    if (iter == screen_buffers_.end()) {
        OC_ERROR("");
    }
    screen_buffers_.erase(iter);
}

BindlessArray &FrameBuffer::bindless_array() const noexcept {
    return pipeline()->bindless_array();
}

uint FrameBuffer::pixel_index(uint2 pos) const noexcept {
    return pos.y * resolution().x + pos.x;
}

void FrameBuffer::fill_window_buffer(const Texture2D &input) noexcept {
    input.download_immediately(window_buffer_.data());
    visualizer_->draw(window_buffer_.data());
}

void FrameBuffer::download_final_picture(float4 *data) const noexcept {
    view_texture_.download_immediately(data);
}

void FrameBuffer::resize(ocarina::uint2 res) noexcept {
    window_buffer_.resize(res.x * res.y, make_float4(0.f));
    resolution_ = res;
}

void FrameBuffer::update_resolution(ocarina::uint2 res) noexcept {
    OC_INFO_FORMAT("FrameBuffer::update_resolution input=({}, {}), prev=({}, {}), rt_before=({}, {})",
                   res.x, res.y, resolution_.x, resolution_.y,
                   raytracing_resolution().x, raytracing_resolution().y);
    resize(res);
    reset_surfaces();
    reset_surface_exts();
    reset_motion_vectors();
    reset_hit_bsdfs();
    reset_rt_buffer();
    reset_direct_lighting();
    reset_indirect_lighting();
    reset_accumulation_buffer();
    reset_rays();
    reset_visibility_buffer();
    update_screen_window();
    auto_manage_accumulation_buffer(accumulation_.hv());
    for (auto &it : screen_buffers_) {
        it.second->update_resolution(res, device());
    }
    OC_INFO_FORMAT("FrameBuffer::update_resolution done resolution=({}, {}), rt_after=({}, {}), pixel_num={}",
                   resolution_.x, resolution_.y,
                   raytracing_resolution().x, raytracing_resolution().y,
                   pixel_num());
}

uint FrameBuffer::pixel_num() const noexcept {
    return resolution_.x * resolution_.y;
}

uint2 FrameBuffer::resolution() const noexcept {
    return resolution_;
}

BindlessArray &FrameBuffer::bindless_array() noexcept {
    return pipeline()->bindless_array();
}

void FrameBuffer::after_render() noexcept {
    //    fill_window_buffer(view_texture_);
}

const Buffer<float4> &FrameBuffer::cur_screen_buffer() const noexcept {
    return screen_buffers_.at(cur_view_)->device_buffer();
}

Uint FrameBuffer::checkerboard_value(const Uint2 &coord) noexcept {
    return (coord.x & 1) ^ (coord.y & 1);
}

Uint FrameBuffer::checkerboard_value(const Uint2 &coord, const Uint &frame_index) noexcept {
    return checkerboard_value(coord) ^ (frame_index & 1);
}

Float2 FrameBuffer::compute_motion_vec(const TSensor &camera, const Float2 &p_film,
                                       const Float3 &cur_pos, const Bool &is_hit) noexcept {
    Float2 ret = make_float2(0.f);
    $if(is_hit) {
        Float2 raster_coord = camera->prev_raster_coord(cur_pos).xy();
        ret = p_film - raster_coord;
    };
    return ret;
}

Float3 FrameBuffer::compute_motion_vector(const TSensor &camera, const Float2 &p_film,
                                          const Uint &frame_index) const noexcept {
    Uint2 pixel = make_uint2(p_film);
    Uint pixel_index = dispatch_id(pixel);
    SurfaceDataVar cur_surf = cur_surfaces_var(frame_index).read(pixel_index);
    SurfaceDataVar prev_surf = prev_surfaces_var(frame_index).read(pixel_index);
    return compute_motion_vector(camera, cur_surf->position(), prev_surf->position());
}

Float3 FrameBuffer::compute_motion_vector(const TSensor &camera, const Float3 &cur_pos,
                                          const Float3 &pre_pos) const noexcept {
    Float3 cur_coord = camera->raster_coord(cur_pos);
    Float3 prev_coord = camera->raster_coord(pre_pos);
    return prev_coord - cur_coord;
}
}// namespace vision

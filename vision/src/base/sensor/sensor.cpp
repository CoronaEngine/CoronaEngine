//
// Created by Zero on 2023/6/13.
//

#include "sensor.h"
#include "base/mgr/pipeline.h"
#include "GUI/widgets.h"

namespace vision {
using namespace ocarina;

static float4x4 origin_matrix;

Sensor::Sensor(const SensorDesc &desc)
    : Photosensory(desc) {
    init(desc);
    update_resolution(frame_buffer().resolution());
}

void Sensor::init(const SensorDesc &desc) noexcept {
    velocity_ = desc["velocity"].as_float(10.f);
    sensitivity_ = desc["sensitivity"].as_float(0.5f);
    set_fov_y(desc["fov_y"].as_float(20.f));
    update_mat(desc.transform_desc.mat);
    origin_matrix = desc.transform_desc.mat;
}

void Sensor::render_sub_UI(Widgets *widgets) noexcept {
    Photosensory::render_sub_UI(widgets);
    widgets->button_click("reset view", [&] {
        changed_ = true;
        update_mat(origin_matrix);
    });
    changed_ |= widgets->drag_float3("position", &position_, 0.05, 0, 0);
    bool changed_fov = widgets->drag_float("fov y", &fov_y_, 0.05, fov_min, fov_max);
    if (changed_fov) {
        set_fov_y(fov_y_);
    }
    changed_ |= changed_fov;
    changed_ |= widgets->drag_float("yaw", &yaw_, 0.05, 0, 0);
    changed_ |= widgets->drag_float("pitch", &pitch_, 0.05, -pitch_max, pitch_max);
    widgets->drag_float("velocity", &velocity_, 0.1, 0, 0);
}

RayState Sensor::generate_ray(const SensorSample &ss) const noexcept {
    RayVar ray = generate_ray_in_local_space(ss);
    // Clip against camera-space Z planes after the concrete camera (e.g. thin
    // lens) has generated its ray. Using near/far as ray distances would clip
    // oblique rays differently from rasterization.
    Float dz = max(ray->direction().z, 1e-8f);
    ray.org_min.w = max(ray->t_min(), ((*primary_clip_planes_).x - ray->origin().z) / dz);
    ray.dir_max.w = min(ray->t_max(), ((*primary_clip_planes_).y - ray->origin().z) / dz);
    Float4x4 c2w = *c2w_;
    ray = transform_ray(c2w, ray);
    return {.ray = ray, .ior = 1.f, .medium = *medium_id_};
}

RayVar Sensor::generate_ray_in_local_space(const vision::SensorSample &ss) const noexcept {
    Float3 p_film = make_float3(ss.p_film, 0.f);
    Float3 p_sensor = transform_point(*raster_to_sensor_, p_film);
    RayVar ray = make_ray(make_float3(0.f), normalize(p_sensor));
    return ray;
}

void Sensor::update_resolution(uint2 res) noexcept {
    if (external_camera_) {
        // Corona uses Vulkan NDC: raster (0,0) maps to (-1,-1), and its
        // projection already flips Y. No aspect-dependent screen window.
        raster_to_screen_ = translation<H>(-1.f, -1.f, 0.f) *
                            scale<H>(2.f / res.x, 2.f / res.y, 1.f);
        raster_to_sensor_ = inverse(camera_to_screen_) * raster_to_screen_;
        return;
    }
    Box2f scrn = frame_buffer().screen_window();
    float2 span = scrn.span();
    float4x4 screen_to_raster = transform::scale<H>(res.x, res.y, 1u) *
                                transform::scale<H>(1 / span.x, 1 / -span.y, 1.f) *
                                transform::translation<H>(-scrn.lower.x, -scrn.upper.y, 0.f);
    raster_to_screen_ = inverse(screen_to_raster);
    update_raster();
}

void Sensor::update_raster() noexcept {
    if (!external_camera_) {
        camera_to_screen_ = transform::perspective<H>(fov_y(), z_near, z_far);
    }
    raster_to_sensor_ = inverse(camera_to_screen_) * raster_to_screen_;
}

void Sensor::update_mat(float4x4 m) noexcept {
    external_camera_ = false;
    primary_clip_planes_ = make_float2(0.f, ray_t_max);
    pitch_ = degrees(std::atan2(m[1][2], m[1][1]));
    yaw_ = degrees(std::atan2(m[2][0], m[0][0]));
    position_ = make_float3(m[3]);
    c2w_ = camera_to_world();
}

void Sensor::set_mat(ocarina::float4x4 m) noexcept {
    c2w_ = m;
    position_ = make_float3(m[3]);
}

Sensor::CameraChange Sensor::set_external_camera(float4x4 camera_to_world,
                                                float4x4 camera_to_screen,
                                                uint2 resolution, float fov_y,
                                                float near_plane, float far_plane) noexcept {
    const auto differs = [](const float4x4 &a, const float4x4 &b) {
        for (uint col = 0; col < 4; ++col) {
            for (uint row = 0; row < 4; ++row) {
                if (std::abs(a[col][row] - b[col][row]) > 1e-6f) return true;
            }
        }
        return false;
    };
    const auto raster_to_screen = translation<H>(-1.f, -1.f, 0.f) *
                                  scale<H>(2.f / resolution.x, 2.f / resolution.y, 1.f);
    CameraChange change{
        !external_camera_ || differs(c2w_.hv(), camera_to_world),
        !external_camera_ || differs(camera_to_screen_, camera_to_screen) ||
            differs(raster_to_screen_, raster_to_screen) ||
            primary_clip_planes_.hv().x != near_plane || primary_clip_planes_.hv().y != far_plane};
    if (change.pose || change.projection) ++external_camera_revision_;
    if (change.projection) ++external_projection_revision_;
    external_camera_ = true;
    c2w_ = camera_to_world;
    position_ = make_float3(camera_to_world[3]);
    camera_to_screen_ = camera_to_screen;
    raster_to_screen_ = raster_to_screen;
    raster_to_sensor_ = inverse(camera_to_screen_) * raster_to_screen_;
    fov_y_ = fov_y;
    tan_fov_y_over_2_ = tan(radians(fov_y_) * 0.5f);
    primary_clip_planes_ = make_float2(near_plane, far_plane);
    return change;
}

void Sensor::after_render() noexcept {
    store_prev_data();
}

void Sensor::store_prev_data() noexcept {
    prev_s2r_ = inverse(raster_to_sensor_.hv());
    prev_w2s_ = inverse(c2w_.hv());
    prev_pos_ = position();
}

Float3 Sensor::prev_raster_coord(Float3 pos) const noexcept {
    pos = transform_point(*prev_w2s_, pos);
    pos /= pos.z;
    Float3 ret = transform_point(*prev_s2r_, pos);
    return ret;
}

Float3 Sensor::raster_coord(ocarina::Float3 pos) const noexcept {
    Float4x4 w2c = inverse(device_c2w());
    pos = transform_point(w2c, pos);
    pos /= pos.z;
    Float4x4 c2r = inverse(raster_to_sensor_.dv());
    Float3 ret = transform_point(c2r, pos);
    return ret;
}

float3 Sensor::raster_coord(float3 pos) const noexcept {
    float4x4 w2c = inverse(c2w_.hv());
    pos = transform_point<H>(w2c, pos);
    pos /= pos.z;
    float4x4 c2r = inverse(raster_to_sensor_.hv());
    float3 ret = transform_point<H>(c2r, pos);
    return ret;
}

LineSegment Sensor::clipping(vision::LineSegment ls) const noexcept {
    float3 p0 = ls.p0;
    float3 p1 = ls.p1;
    float4x4 w2c = inverse(c2w_.hv());
    p0 = transform_point<H>(w2c, p0);
    p1 = transform_point<H>(w2c, p1);
    float3 sp0 = transform_point<H>(camera_to_screen_, p0);
    float3 sp1 = transform_point<H>(camera_to_screen_, p1);
    if (sp1.z > sp0.z) {
        float t = (sp1.z - z_near) / (sp1.z - sp0.z);
        t = ocarina::clamp(t, 0.f, 1.f);
        ls.p0 = ocarina::lerp(t, ls.p1, ls.p0);
    } else {
        float t = (sp0.z - z_near) / (sp0.z - sp1.z);
        t = ocarina::clamp(t, 0.f, 1.f);
        ls.p1 = ocarina::lerp(t, ls.p0, ls.p1);
    }
    return ls;
}

void Sensor::update_device_data() noexcept {
    c2w_ = camera_to_world();
    Photosensory::update_data();
    upload_immediately();
}

void Sensor::prepare() noexcept {
    Photosensory::prepare();
    prepare_data();
    upload_immediately();
    store_prev_data();
}

float4x4 Sensor::camera_to_world_rotation() const noexcept {
    if (external_camera_) {
        auto rotation = c2w_.hv();
        rotation[3] = make_float4(0.f, 0.f, 0.f, 1.f);
        return rotation;
    }
    float4x4 horizontal = rotation_y<H>(yaw());
    float4x4 vertical = rotation_x<H>(-pitch());
    return scale<H>(1, 1, -1) * horizontal * vertical;
}

float4x4 Sensor::camera_to_world() const noexcept {
    if (external_camera_) return c2w_.hv();
    float4x4 translate = translation<H>(position_);
    return translate * camera_to_world_rotation();
}

float3 Sensor::forward() const noexcept {
    return c2w_.hv()[2].xyz();
}

float3 Sensor::up() const noexcept {
    return c2w_.hv()[1].xyz();
}

float3 Sensor::right() const noexcept {
    return c2w_.hv()[0].xyz();
}

Float4x4 Sensor::device_c2w() const noexcept {
    return *c2w_;
}

void Sensor::set_device_c2w(const ocarina::Float4x4 &mat) noexcept {
    c2w_.dv() = mat;
}

Float4x4 Sensor::device_w2c() const noexcept {
    return inverse(device_c2w());
}

float4x4 Sensor::host_c2w() const noexcept {
    return c2w_.hv();
}

Float Sensor::linear_depth(const Float3 &world_pos) const noexcept {
    Float3 c_pos = transform_point(device_w2c(), world_pos);
    return c_pos.z;
}

Float3 Sensor::device_forward() const noexcept {
    return (*c2w_)[2].xyz();
}

Float3 Sensor::device_up() const noexcept {
    return (*c2w_)[1].xyz();
}

Float3 Sensor::device_right() const noexcept {
    return (*c2w_)[0].xyz();
}

Float3 Sensor::device_position() const noexcept {
    return (*c2w_)[3].xyz();
}
}// namespace vision

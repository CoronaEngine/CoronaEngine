//
// Created by Zero on 14/07/2025.
//

#include "base/sensor/frame_buffer.h"
#include "base/sensor/light_field_types.h"
#include "rhi/resources/shader.h"
#include "base/mgr/scene.h"
#include "base/mgr/pipeline.h"

namespace vision {
using namespace ocarina;

// ============================================================================
// LightFieldFrameBuffer Implementation
// ============================================================================

class LightFieldFrameBuffer : public FrameBuffer, public ILightFieldFrameBuffer {
private:
    // ========== Parameters ==========
    LenticularParams lenticular_;
    LightFieldGeometry geometry_;
    float4x4 local_to_world_{make_float4x4(1.f)};             // Light field local coords -> world coords
    mutable float4x4 prev_local_to_world_{make_float4x4(1.f)};// Previous frame's l2w for motion vector (mutable for const methods)

    // ========== Shaders ==========
    // Shader declaration uses host types: Buffer<T>, host structs, float4x4
    Shader<void(Buffer<RayData>, LenticularParams, LightFieldGeometry, float4x4)> generate_rays_;
    Shader<void(Buffer<float4>, Buffer<float4>, LenticularParams)> encode_;
    // compute_geom_lf_ now takes both current and previous l2w for motion vector computation
    Shader<void(GBufferParam, LenticularParams, LightFieldGeometry, float4x4, float4x4)> compute_geom_lf_;
    Shader<void(Buffer<TriangleHit>, LenticularParams, LightFieldGeometry, uint2, float4x4)> compute_hit_lf_;

    // ========== Additional Buffers ==========
    RegistrableManaged<float4> encoded_buffer_;
    // Pixel-sized accumulation buffer for encoded light-field image.
    // NOTE: Base FrameBuffer::accumulation_buffer_ is sized by frame_buffer_size(),
    // which is subpixel-sized in light field mode (res_w * res_h * 3).
    RegistrableManaged<float4> encoded_accum_buffer_;

    // Pixel-sized SSAT diagnostic output. It is allocated only after a camera
    // requests FinalView, so the default interlaced path keeps its old memory
    // footprint and data flow.
    RegistrableManaged<float4> viewer_buffer_;
    LightFieldViewerMode viewer_mode_{LightFieldViewerMode::Interlaced};
    std::uint32_t viewer_view_index_{0u};

public:
    LightFieldFrameBuffer() = default;
    explicit LightFieldFrameBuffer(const FrameBufferDesc &desc)
        : FrameBuffer(desc) {

        // Read parameters from description (optional)
        if (desc.contains("lenticular")) {
            ParameterSet lent = desc["lenticular"];
            lenticular_.pe = lent["pe"].as_float(19.1813f);
            lenticular_.angle = lent["angle"].as_float(0.2305f);
            lenticular_.offset = lent["offset"].as_float(14.1171f);
            lenticular_.num_views = lent["num_views"].as_float(60.f);
            lenticular_.res_w = lent["res_w"].as_float(32.f);
            lenticular_.res_h = lent["res_h"].as_float(32.f);
        }
        if (desc.contains("geometry")) {
            ParameterSet geom = desc["geometry"];
            geometry_.d_f = geom["d_f"].as_float(20.f);
            geometry_.fov_h_deg = geom["fov_h_deg"].as_float(45.f);
            geometry_.aspect = geom["aspect"].as_float(1.77f);
            geometry_.array_angle_deg = geom["array_angle_deg"].as_float(30.f);
        }
        compute_derived_params();
    }
    VS_MAKE_PLUGIN_NAME_FUNC

    // ========== Derived Parameter Computation ==========

    void compute_derived_params() noexcept {
        float fov_rad = radians(geometry_.fov_h_deg);
        geometry_.W_f = 2.f * geometry_.d_f * std::tan(fov_rad / 2.f);
        geometry_.H_f = geometry_.W_f / geometry_.aspect;
    }

    // ========== Size Calculations ==========

    [[nodiscard]] uint total_subpixels() const noexcept {
        // Use integer arithmetic to avoid float precision loss for large resolutions
        return static_cast<uint>(lenticular_.res_w) * static_cast<uint>(lenticular_.res_h) * 3u;
    }

    [[nodiscard]] uint total_pixels() const noexcept {
        return static_cast<uint>(lenticular_.res_w) * static_cast<uint>(lenticular_.res_h);
    }

    [[nodiscard]] uint2 subpixel_dispatch_dim() const noexcept {
        return make_uint2(static_cast<uint>(lenticular_.res_w) * 3u, static_cast<uint>(lenticular_.res_h));
    }

    [[nodiscard]] uint2 pixel_dispatch_dim() const noexcept {
        return make_uint2(static_cast<uint>(lenticular_.res_w), static_cast<uint>(lenticular_.res_h));
    }

    [[nodiscard]] uint2 raytracing_resolution() const noexcept override {
        return subpixel_dispatch_dim();
    }

    [[nodiscard]] uint frame_buffer_size() const noexcept override {
        return total_subpixels();
    }

    // ========== Custom Ray Generation (does not use camera) ==========

    [[nodiscard]] RayState custom_generate_ray(const Sensor *sensor,
                                               const SensorSample &ss) const noexcept override {
        // Light field rendering does not use traditional camera, this function should not be called
        // Return default value to satisfy interface requirements
        return sensor->generate_ray(ss);
    }

    // ========== Compile Shaders ==========

    void compile() noexcept override {
        compile_gamma();
        compile_accumulation();
        compile_tone_mapping();
        compile_compute_geom_lf();
        compile_generate_rays();
        compile_encode();
        compile_hit_lf();
    }

    void compile_compute_geom_lf() noexcept {
        TSensor &camera = scene().sensor();
        TSampler &sampler = renderer().sampler();
        TLightSampler &light_sampler = renderer().light_sampler();
        Kernel kernel = [&](Var<GBufferParam> param,
                            Var<LenticularParams> lent,
                            Var<LightFieldGeometry> geom,
                            Float4x4 l2w,
                            Float4x4 prev_l2w) {
            auto &frame_index = param.frame_index;
            auto &vbuffer = param.visibility_buffer;
            auto &motion_vectors = param.motion_vectors;
            auto &rays = param.rays;

            RenderEnv render_env;
            render_env.initial(sampler, frame_index, spectrum());
            Uint2 pixel = dispatch_idx().xy();

            sampler->load_data();
            sampler->set_seed(pixel, frame_index, 0);
            camera->load_data();

            // Decode subpixel index: pixel.x = x*3 + k, pixel.y = y
            Uint dx = pixel.x;
            Uint dy = pixel.y;
            Uint x = dx / 3u;
            Uint k = dx % 3u;
            Uint y = dy;

            // ----- Lenticular view selection (same as generate_rays) -----
            Float D = 3.f * cast<float>(x) +
                      3.f * cast<float>(y) * tan(lent.angle) +
                      cast<float>(k) + lent.offset;
            Float pe_val = lent.pe;
            Float A = D - floor(D / pe_val) * pe_val;// positive mod in [0, pe)
            Float num_views_f = lent.num_views;
            Uint view_id = cast<uint>(floor(A / (pe_val / num_views_f)));
            // Flip view ID: 0 -> N-1, 1 -> N-2, ..., N-1 -> 0
            view_id = cast<uint>(num_views_f - 1.f) - view_id;

            // ----- Focal plane point (subpixel) -----
            Float pw = geom.W_f / lent.res_w;
            Float ph = geom.H_f / lent.res_h;
            Float subpixel_offset = (cast<float>(k) + 0.5f) * (pw / 3.f);
            Float focal_x = -geom.W_f / 2.f + cast<float>(x) * pw + subpixel_offset;
            Float focal_y = geom.H_f / 2.f - (cast<float>(y) + 0.5f) * ph;
            Float3 focal_local = make_float3(focal_x, focal_y, geom.d_f);

            // ----- Camera position on array plane -----
            Float array_angle_rad = radians(geom.array_angle_deg);
            Float d_f = geom.d_f;
            Float u = select(num_views_f > 1.f,
                             cast<float>(view_id) / (num_views_f - 1.f),
                             0.5f);
            Float angle_i = (u - 0.5f) * array_angle_rad;
            Float3 cam_local = make_float3(d_f * tan(angle_i), 0.f, 0.f);

            Float3 focal_world = transform_point(l2w, focal_local);
            Float3 cam_world = transform_point(l2w, cam_local);
            Float3 direction = normalize(focal_world - cam_world);

            RayVar ray = make_ray(cam_world, direction);
            RayState rs = RayState::create(ray, 1.f, InvalidUI32);

            TriangleHitVar hit = pipeline()->geometry().trace_closest(rs.ray);
            RayDataVar ray_data;
            ray_data->from_ray_state(rs);
            rays.write(dispatch_id(), ray_data);

            // Write hit to visibility buffer
            vbuffer.write(dispatch_id(), hit);

            // Defaults
            Float2 motion_vec = make_float2(0.f);

            const SampledWavelengths &swl = render_env.sampled_wavelengths();

            $if(hit->is_hit()) {
                Interaction it = pipeline()->geometry().compute_surface_interaction(hit, rs.ray, true);

                // ----- Motion Vector Computation (in subpixel space) -----
                // Reproject hit point using previous frame's transformation
                Float4x4 prev_w2l = inverse(prev_l2w);
                // Use individual params to avoid Var<Struct> copy issues in $if
                Float2 prev_pixel = reproject_to_lightfield_dsl(
                    it.pos, prev_w2l,
                    lent.res_w, lent.res_h,
                    geom.d_f, geom.W_f, geom.H_f);

                // Current subpixel coordinates (dx, dy) with center offset
                // dx = x*3 + k, dy = y
                Float2 current_subpixel_center = make_float2(
                    cast<float>(dx) + 0.5f,  // dx is already in subpixel space
                    cast<float>(dy) + 0.5f   // dy is already in subpixel space
                );

                // Previous subpixel coordinates: convert pixel space to subpixel space
                // For the previous frame, we need to determine which subpixel channel (k) it corresponds to
                // Since we don't know the exact k for the previous frame, we use the current k
                // This is an approximation but maintains subpixel-level motion tracking
                Float prev_subpixel_x = prev_pixel.x * 3.f + cast<float>(k) + 0.5f;
                Float prev_subpixel_y = prev_pixel.y + 0.5f;
                Float2 prev_subpixel_center = make_float2(prev_subpixel_x, prev_subpixel_y);

                // Motion vector in subpixel space (directly usable by denoiser)
                motion_vec = current_subpixel_center - prev_subpixel_center;
            };

            // Write motion vectors in subpixel space
            // This allows denoiser to use motion vectors directly without coordinate conversion
            motion_vectors.write(dispatch_id(), motion_vec);
        };
        compute_geom_lf_ = device().compile(kernel, "lightfield_rt_geom");
    }

    void compile_hit_lf() noexcept {
        // Hit buffer for editor picking: trace a representative ray for each display pixel.
        // We use k=1 (G subpixel) as the representative ray.
        Kernel kernel = [&](BufferVar<TriangleHit> hit_buffer,
                            Var<LenticularParams> lent,
                            Var<LightFieldGeometry> geom,
                            Uint2 pixel,
                            Float4x4 l2w) {
            Uint x = pixel.x;
            Uint y = pixel.y;
            Uint k = 1u;

            // View ID (same as generate_rays)
            Float D = 3.f * cast<float>(x) +
                      3.f * cast<float>(y) * tan(lent.angle) +
                      cast<float>(k) + lent.offset;
            Float pe_val = lent.pe;
            Float A = D - floor(D / pe_val) * pe_val;// positive mod in [0, pe)
            Float num_views_f = lent.num_views;
            Uint view_id = cast<uint>(floor(A / (pe_val / num_views_f)));
            // Flip view ID: 0 -> N-1, 1 -> N-2, ..., N-1 -> 0
            view_id = cast<uint>(num_views_f - 1.f) - view_id;

            // Focal plane point (k=1)
            Float pw = geom.W_f / lent.res_w;
            Float ph = geom.H_f / lent.res_h;
            Float subpixel_offset = (cast<float>(k) + 0.5f) * (pw / 3.f);
            Float focal_x = -geom.W_f / 2.f + cast<float>(x) * pw + subpixel_offset;
            Float focal_y = geom.H_f / 2.f - (cast<float>(y) + 0.5f) * ph;
            Float3 focal_local = make_float3(focal_x, focal_y, geom.d_f);

            // Camera position on array plane
            Float array_angle_rad = radians(geom.array_angle_deg);
            Float d_f = geom.d_f;
            Float u = select(num_views_f > 1.f,
                             cast<float>(view_id) / (num_views_f - 1.f),
                             0.5f);
            Float angle_i = (u - 0.5f) * array_angle_rad;
            Float3 cam_local = make_float3(d_f * tan(angle_i), 0.f, 0.f);

            Float3 focal_world = transform_point(l2w, focal_local);
            Float3 cam_world = transform_point(l2w, cam_local);
            Float3 direction = normalize(focal_world - cam_world);

            RayVar ray = make_ray(cam_world, direction);
            TriangleHitVar hit = pipeline()->geometry().trace_closest(ray);
            hit_buffer.write(0, hit);
        };
        compute_hit_lf_ = device().compile(kernel, "lightfield_compute_hit");
    }

    void compile_generate_rays() noexcept {
        // Kernel lambda uses DSL types: BufferVar<T>, Var<Struct>, Float4x4
        // TODO : move this logic to custom_generate_ray
        Kernel kernel = [&](BufferVar<RayData> rays,
                            Var<LenticularParams> lent,
                            Var<LightFieldGeometry> geom,
                            Float4x4 l2w) {
            Uint2 didx = dispatch_idx().xy();
            Uint dx = didx.x;
            Uint dy = didx.y;

            // ===== 1. Decode subpixel index =====
            Uint x = dx / 3u;
            Uint k = dx % 3u;
            Uint y = dy;

            // ===== 2. Compute view ID (lenticular interlacing) =====
            // From JS: getViewId(x, y, k)
            // D = 3*x + 3*y*tan(angle) + k + offset
            Float D = 3.f * cast<float>(x) +
                      3.f * cast<float>(y) * tan(lent.angle) +
                      cast<float>(k) + lent.offset;
            // NOTE: CUDA backend currently does not implement CallOp::FMOD codegen.
            // Use A = D - floor(D / pe) * pe to compute a positive modulus in [0, pe).
            Float pe_val = lent.pe;
            Float A = D - floor(D / pe_val) * pe_val;
            Float num_views_f = lent.num_views;
            Uint view_id = cast<uint>(floor(A / (pe_val / num_views_f)));
            // Flip view ID: 0 -> N-1, 1 -> N-2, ..., N-1 -> 0
            view_id = cast<uint>(num_views_f - 1.f) - view_id;

            // ===== 3. Compute subpixel position on focal plane (local coords) =====
            // From JS: getSubPixelPosition(x, y, k)
            Float res_w_f = lent.res_w;
            Float res_h_f = lent.res_h;
            Float W_f = geom.W_f;
            Float H_f = geom.H_f;

            Float pw = W_f / res_w_f;// Pixel width
            Float ph = H_f / res_h_f;// Pixel height

            // Subpixel X offset (3 subpixels per pixel: R=0, G=1, B=2)
            Float subpixel_offset = (cast<float>(k) + 0.5f) * (pw / 3.f);

            // Local coordinates (focal plane at z=d_f, camera array plane at z=0)
            // X: left to right, [-W_f/2, W_f/2]
            // Y: top to bottom, [H_f/2, -H_f/2] (pixel y=0 at top)
            Float focal_x = -W_f / 2.f + cast<float>(x) * pw + subpixel_offset;
            Float focal_y = H_f / 2.f - (cast<float>(y) + 0.5f) * ph;
            Float focal_z = geom.d_f;
            Float3 focal_local = make_float3(focal_x, focal_y, focal_z);

            // ===== 4. Compute camera position (local coords) =====
            // From JS: camera array generation logic
            Float array_angle_rad = radians(geom.array_angle_deg);
            Float d_f = geom.d_f;

            // Normalized position u in [0, 1]
            Float u = select(num_views_f > 1.f,
                             cast<float>(view_id) / (num_views_f - 1.f),
                             0.5f);

            // Angular offset: (u - 0.5) * array_angle
            Float angle_i = (u - 0.5f) * array_angle_rad;

            // Camera position (on z = d_f plane)
            Float cam_x = d_f * tan(angle_i);
            Float cam_y = 0.f;
            Float cam_z = 0.f;
            Float3 cam_local = make_float3(cam_x, cam_y, cam_z);

            // ===== 5. Transform to world coordinates =====
            Float3 focal_world = transform_point(l2w, focal_local);
            Float3 cam_world = transform_point(l2w, cam_local);

            // ===== 6. Compute ray =====
            Float3 direction = normalize(focal_world - cam_world);

            // ===== 7. Write to rays buffer =====
            RayDataVar ray_data;
            ray_data->set_origin(cam_world);
            ray_data->set_direction(direction);
            ray_data->set_ior(1.f);
            ray_data->set_medium(InvalidUI32);

            rays.write(dispatch_id(), ray_data);
        };

        generate_rays_ = device().compile(kernel, "lightfield_generate_rays");
    }

    void compile_encode() noexcept {
        // Encode subpixel rendering results into RGB image via interlacing
        // Kernel lambda uses DSL types
        Kernel kernel = [&](BufferVar<float4> rendered,// Input: subpixel colors [total_subpixels]
                            BufferVar<float4> encoded, // Output: pixel RGB [total_pixels]
                            Var<LenticularParams> lent) {
            Uint2 pixel = dispatch_idx().xy();
            Uint x = pixel.x;
            Uint y = pixel.y;

            // Compute indices of 3 subpixels for this pixel in rendered buffer
            // Rendered buffer layout: linear_idx = dy * (res_w * 3) + dx
            // where dx = x * 3 + k, dy = y
            // Note: res_w is float, cast to uint
            Uint res_w_uint = cast<uint>(lent.res_w);
            Uint res_w_3 = res_w_uint * 3u;

            Uint idx_r = y * res_w_3 + x * 3u + 0u;// R subpixel
            Uint idx_g = y * res_w_3 + x * 3u + 1u;// G subpixel
            Uint idx_b = y * res_w_3 + x * 3u + 2u;// B subpixel

            Float4 color_r = rendered.read(idx_r);
            Float4 color_g = rendered.read(idx_g);
            Float4 color_b = rendered.read(idx_b);

            // ===== Interlacing encode =====
            // Each subpixel contributes to its corresponding channel
            // R subpixel result -> final image R channel
            // G subpixel result -> final image G channel
            // B subpixel result -> final image B channel
            //
            // Option 1: Take corresponding channel value
            Float r = color_r.x;
            Float g = color_g.y;
            Float b = color_b.z;

            // Option 2: Take luminance (if rendering is full color)
            // Float r = 0.299f * color_r.x + 0.587f * color_r.y + 0.114f * color_r.z;
            // Float g = 0.299f * color_g.x + 0.587f * color_g.y + 0.114f * color_g.z;
            // Float b = 0.299f * color_b.x + 0.587f * color_b.y + 0.114f * color_b.z;

            Uint out_idx = y * res_w_uint + x;
            encoded.write(out_idx, make_float4(r, g, b, 1.f));
        };

        encode_ = device().compile(kernel, "lightfield_encode");
    }

    // ========== Reprojection Helpers ==========

    /// Reproject a world-space 3D point to light field pixel coordinates
    /// Returns Float2(x, y) in pixel space (not subpixel)
    /// The reprojection finds where a 3D point projects onto the focal plane
    /// Overload with individual parameters to avoid Var<Struct> copy issues
    static Float2 reproject_to_lightfield_dsl(const Float3 &world_pos,
                                              const Float4x4 &w2l,
                                              Float res_w, Float res_h,
                                              Float d_f, Float W_f, Float H_f) {
        // Transform world position to light field local coordinates
        Float3 local_pos = (w2l * make_float4(world_pos, 1.f)).xyz();

        // Project onto focal plane (z = d_f)
        // If point is behind camera (z <= 0), clamp to avoid division issues
        Float z_safe = max(local_pos.z, 0.001f);
        Float scale = d_f / z_safe;
        Float focal_x = local_pos.x * scale;
        Float focal_y = local_pos.y * scale;

        // Convert focal plane coordinates to pixel coordinates
        // focal_x is in [-W_f/2, W_f/2], we need to map to [0, res_w)
        // focal_y is in [H_f/2, -H_f/2] (top to bottom), map to [0, res_h)
        Float pw = W_f / res_w;
        Float ph = H_f / res_h;

        Float pixel_x = (focal_x + W_f / 2.f) / pw;
        Float pixel_y = (H_f / 2.f - focal_y) / ph;

        return make_float2(pixel_x, pixel_y);
    }

    /// Reproject a world-space 3D point to light field pixel coordinates (convenience overload)
    static Float2 reproject_to_lightfield_dsl(const Float3 &world_pos,
                                              const Float4x4 &w2l,
                                              Var<LenticularParams> lent,
                                              Var<LightFieldGeometry> geom) {
        return reproject_to_lightfield_dsl(world_pos, w2l,
                                           lent.res_w, lent.res_h,
                                           geom.d_f, geom.W_f, geom.H_f);
    }

    // ========== Prepare Buffers ==========

    void prepare() noexcept override {
        OC_INFO_FORMAT("LightFieldFrameBuffer::prepare pixel_res=({}, {}), subpixel_res=({}, {})",
                       pixel_dispatch_dim().x, pixel_dispatch_dim().y,
                       subpixel_dispatch_dim().x, subpixel_dispatch_dim().y);
        // Ensure the framebuffer/display resolution matches the final pixel resolution.
        // raytracing_resolution() remains subpixel_dispatch_dim().
        resize(pixel_dispatch_dim());
        update_screen_window();

        compute_derived_params();

        // Encode data
        encode_data();
        datas().reset_device_buffer_immediately(device(), "LightFieldFrameBuffer::encoded_data");
        datas().register_self();
        datas().upload_immediately();

        // Prepare light field specific buffers
        prepare_rays_buffer();
        prepare_rt_buffer_lf();
        prepare_encoded_buffer();
        prepare_encoded_accum_buffer();
        auto_manage_accumulation_buffer(accumulation_.hv());
//        prepare_screen_buffer(output_buffer_);
        init_hit_buffer();

        // Initialize prev_l2w to current l2w so frame 0 doesn't see a stale identity matrix
        update_prev_transform();
    }

    void update_resolution(uint2 res) noexcept override {
        OC_INFO_FORMAT("LightFieldFrameBuffer::update_resolution input=({}, {}), prev_pixel=({}, {}), prev_subpixel=({}, {})",
                       res.x, res.y,
                       pixel_dispatch_dim().x, pixel_dispatch_dim().y,
                       subpixel_dispatch_dim().x, subpixel_dispatch_dim().y);
        // Treat GUI resolution change as the final pixel resolution.
        lenticular_.res_w = static_cast<float>(res.x);
        lenticular_.res_h = static_cast<float>(res.y);
        // The focal plane must match the final interlaced output/display panel.
        geometry_.aspect = lightfield_aspect_from_resolution(res);
        compute_derived_params();

        // Let base class reset all built-in buffers according to the new size.
        // NOTE: frame_buffer_size() depends on raytracing_resolution() (subpixel),
        // while resolution_ is the final pixel resolution.
        FrameBuffer::update_resolution(res);
        // Recreate light-field specific buffers
        prepare_encoded_buffer();
        prepare_encoded_accum_buffer();
        if (viewer_buffer_.device_buffer().size() != 0) {
            prepare_viewer_buffer();
        }
        OC_INFO_FORMAT("LightFieldFrameBuffer::update_resolution done pixel_res=({}, {}), subpixel_res=({}, {}), total_subpixels={}",
                       pixel_dispatch_dim().x, pixel_dispatch_dim().y,
                       subpixel_dispatch_dim().x, subpixel_dispatch_dim().y,
                       total_subpixels());

    }

    void prepare_rays_buffer() noexcept {
        uint size = total_subpixels();
        rays_.super() = device().create_buffer<RayData>(size, "LightField::rays");
        vector<RayData> vec(size, RayData{});
        rays_.upload_immediately(vec.data());
        rays_.set_bindless_array(bindless_array());
        rays_.register_self();
    }

    void prepare_rt_buffer_lf() noexcept {
        uint size = total_subpixels();
        rt_buffer_.reset_all(device(), size, "LightField::rt_buffer");
        rt_buffer_.host_buffer().assign(size, float4{});
        rt_buffer_.upload_immediately();
        rt_buffer_.set_bindless_array(bindless_array());
        rt_buffer_.register_self();
    }

    void prepare_encoded_buffer() noexcept {
        uint size = total_pixels();
        encoded_buffer_.reset_all(device(), size, "LightField::encoded");
        encoded_buffer_.host_buffer().assign(size, float4{});
        encoded_buffer_.upload_immediately();
        encoded_buffer_.set_bindless_array(bindless_array());
        encoded_buffer_.register_self();
    }

    void prepare_encoded_accum_buffer() noexcept {
        uint size = total_pixels();
        encoded_accum_buffer_.reset_all(device(), size, "LightField::encoded_accum");
        encoded_accum_buffer_.host_buffer().assign(size, float4{});
        encoded_accum_buffer_.upload_immediately();
        encoded_accum_buffer_.set_bindless_array(bindless_array());
        encoded_accum_buffer_.register_self();
    }

    void prepare_viewer_buffer() noexcept {
        const uint size = total_pixels();
        viewer_buffer_.reset_all(device(), size, "LightField::viewer");
        viewer_buffer_.host_buffer().assign(size, float4{});
        viewer_buffer_.upload_immediately();
        viewer_buffer_.set_bindless_array(bindless_array());
        viewer_buffer_.register_self();
    }

    // ========== Execution Functions ==========

    /// Update previous frame's transformation matrix (call after compute_geom)
    void update_prev_transform() const noexcept {
        float4x4 cam_l2w = scene().sensor()->camera_to_world();
        prev_local_to_world_ = cam_l2w * local_to_world_;
    }

    /// Get current combined l2w matrix (ILightFieldFrameBuffer interface)
    [[nodiscard]] float4x4 get_current_l2w() const noexcept override {
        float4x4 cam_l2w = scene().sensor()->camera_to_world();
        return cam_l2w * local_to_world_;
    }
    
    /// Get previous frame's l2w matrix (ILightFieldFrameBuffer interface)
    [[nodiscard]] float4x4 get_prev_l2w() const noexcept override {
        return prev_local_to_world_;
    }

    [[nodiscard]] CommandBatch compute_geom(const GBufferParam &param) const noexcept override {
        CommandBatch ret;
        // Bind lightfield local space to current camera transform so mouse camera control works.
        float4x4 cam_l2w = scene().sensor()->camera_to_world();
        float4x4 l2w = cam_l2w * local_to_world_;
        // Pass both current and previous l2w for motion vector computation
        ret << compute_geom_lf_(param, lenticular_, geometry_, l2w, prev_local_to_world_)
                   .dispatch(raytracing_resolution());
        return ret;
    }

    [[nodiscard]] CommandBatch compute_GBuffer(const GBufferParam &param) const noexcept override {
        CommandBatch ret;
        ret << compute_geom(param);
        return ret;
    }

    [[nodiscard]] CommandBatch compute_GBuffer(uint frame_index) const noexcept override {
        GBufferParam param;
        param.frame_index = frame_index;
        param.rays = rays().descriptor();
        param.visibility_buffer = cur_visibility_buffer_view(frame_index).descriptor();
        param.motion_vectors = motion_vectors().descriptor();

        return compute_GBuffer(param);
    }

    [[nodiscard]] CommandBatch encode_lightfield() const noexcept {
        CommandBatch ret;
        // Pass buffer views and host struct value
        ret << encode_(rt_buffer_.view(), encoded_buffer_.view(), lenticular_)
                   .dispatch(pixel_dispatch_dim());
        return ret;
    }

    /// Accumulate encoded image (uses pixel resolution)
    [[nodiscard]] CommandBatch accumulate_encoded(uint frame_index) const noexcept {
        CommandBatch ret;
        ret << accumulate_(encoded_buffer_.view(), encoded_accum_buffer_.view(), frame_index)
                   .dispatch(pixel_dispatch_dim());
        return ret;
    }

    /// Tone mapping for encoded image (uses pixel resolution)
    [[nodiscard]] CommandBatch tone_mapping_encoded() const noexcept {
        CommandBatch ret;
        ret << tone_mapping_(encoded_buffer_.view(), view_texture_, exposure_.hv())
                   .dispatch(pixel_dispatch_dim());
        return ret;
    }

    /// Complete light field post-processing pipeline: encode -> tone_mapping
    [[nodiscard]] CommandBatch post_render(uint frame_index) const noexcept {
        CommandBatch ret;
        // 1. Interlacing encode
        ret << encode_lightfield();
        // 2. Final presentation
        ret << render_final(frame_index);
        return ret;
    }

    // ========== Virtual Overrides for Custom Post-Processing ==========

    /// Post-processing hook: perform interlacing encode after path tracing
    [[nodiscard]] CommandBatch post_path_tracing(uint frame_index) const noexcept override {
        CommandBatch ret;
        // 1. Interlacing encode
        ret << encode_lightfield();
        // 2. Update previous frame's transformation for next frame's motion vector
        update_prev_transform();
        return ret;
    }

    [[nodiscard]] CommandBatch compute_hit(uint /*frame_index*/, uint2 pixel) const noexcept override {
        CommandBatch ret;
        float4x4 cam_l2w = scene().sensor()->camera_to_world();
        float4x4 l2w = cam_l2w * local_to_world_;
        ret << compute_hit_lf_(hit_buffer_.view(), lenticular_, geometry_, pixel, l2w)
                   .dispatch(1);
        return ret;
    }

    /// Light field uses custom post-processing pipeline
    [[nodiscard]] bool has_custom_post_processing() const noexcept override { return true; }

    /// Return encoded buffer for accumulation/tone_mapping
    [[nodiscard]] BufferView<float4> post_processed_buffer() const noexcept override {
        return encoded_buffer_.view();
    }

    [[nodiscard]] BufferView<float4> display_source_buffer() const noexcept override {
        if (viewer_mode_ == LightFieldViewerMode::FinalView && viewer_output_ready()) {
            return viewer_buffer_.view();
        }
        return enable_accumulation() ? encoded_accum_buffer_.view() : encoded_buffer_.view();
    }

    [[nodiscard]] CommandBatch clear_accumulation_history() const noexcept override {
        CommandBatch ret;
        ret << FrameBuffer::clear_accumulation_history();
        if (encoded_accum_buffer_.device_buffer().size() != 0) {
            ret << pipeline()->reset_buffer(encoded_accum_buffer_.view(), make_float4(0.f),
                                            "LightField::clear_encoded_accum_history");
        }
        return ret;
    }

    /// Final presentation stage for light field:
    /// post_path_tracing() has already executed encode_lightfield() into encoded_buffer_.
    /// Here we optionally accumulate (pixel-sized) then tone-map into output buffer.
    [[nodiscard]] CommandBatch render_final(uint frame_index) const noexcept override {
        CommandBatch ret;
        if (viewer_mode_ == LightFieldViewerMode::FinalView && viewer_output_ready()) {
            ret << tone_mapping_(viewer_buffer_.view(), view_texture_, exposure_.hv())
                       .dispatch(pixel_dispatch_dim());
            return ret;
        }
        if (enable_accumulation()) {
            ret << accumulate_encoded(frame_index);
            ret << tone_mapping_(encoded_accum_buffer_.view(), view_texture_, exposure_.hv())
                       .dispatch(pixel_dispatch_dim());
        } else {
            ret << tone_mapping_(encoded_buffer_.view(), view_texture_, exposure_.hv())
                       .dispatch(pixel_dispatch_dim());
        }
        return ret;
    }

    // ========== Public Interface ==========

    [[nodiscard]] const RegistrableManaged<float4> &encoded_buffer() const noexcept {
        return encoded_buffer_;
    }

    [[nodiscard]] RegistrableManaged<float4> &encoded_buffer() noexcept {
        return encoded_buffer_;
    }

    void set_transform(const float4x4 &l2w) noexcept {
        local_to_world_ = l2w;
    }

    void set_lenticular_params(float pe, float angle, float offset,
                               uint num_views, uint res_w, uint res_h) noexcept {
        lenticular_.pe = pe;
        lenticular_.angle = angle;
        lenticular_.offset = offset;
        lenticular_.num_views = num_views;
        lenticular_.res_w = res_w;
        lenticular_.res_h = res_h;
    }

    void set_geometry(float d_f, float fov_h_deg, float aspect, float array_angle_deg) noexcept {
        geometry_.d_f = d_f;
        geometry_.fov_h_deg = fov_h_deg;
        geometry_.aspect = aspect;
        geometry_.array_angle_deg = array_angle_deg;
        compute_derived_params();
    }

    /// Get light field parameters (ILightFieldFrameBuffer interface)
    [[nodiscard]] const LenticularParams &lenticular_params() const noexcept override { return lenticular_; }
    [[nodiscard]] const LightFieldGeometry &geometry_params() const noexcept override { return geometry_; }

    void set_viewer_state(LightFieldViewerMode mode,
                          std::uint32_t view_index) noexcept override {
        viewer_mode_ = mode;
        const auto count = lightfield_view_count(lenticular_.num_views);
        viewer_view_index_ = lightfield_effective_view_index(view_index, count);
        if (viewer_mode_ == LightFieldViewerMode::FinalView && !viewer_output_ready()) {
            prepare_viewer_buffer();
        }
    }

    [[nodiscard]] LightFieldViewerMode viewer_mode() const noexcept override {
        return viewer_mode_;
    }

    [[nodiscard]] std::uint32_t viewer_view_index() const noexcept override {
        return viewer_view_index_;
    }

    [[nodiscard]] BufferView<float4> viewer_output_buffer() const noexcept override {
        return viewer_buffer_.view();
    }

    [[nodiscard]] bool viewer_output_ready() const noexcept override {
        return viewer_buffer_.device_buffer().size() == total_pixels();
    }

    /// Get output resolution (pixel resolution, not subpixel)
    [[nodiscard]] uint2 output_resolution() const noexcept {
        return make_uint2(lenticular_.res_w, lenticular_.res_h);
    }

    // ========== UI ==========
    void render_sub_UI(Widgets *widgets) noexcept override {
        FrameBuffer::render_sub_UI(widgets);
        widgets->use_tree("LightField FrameBuffer", [&] {
            // Lenticular parameters
            widgets->use_tree("Lenticular Params", [&] {
                changed_ |= widgets->drag_float("pe", &lenticular_.pe, 0.01f, 1.f, 100.f);
                changed_ |= widgets->drag_float("angle", &lenticular_.angle, 0.001f, 0.f, 1.57f);
                changed_ |= widgets->drag_float("offset", &lenticular_.offset, 0.1f, 0.f, 50.f);
            });

            // Geometry parameters
            widgets->use_tree("Geometry", [&] {
                changed_ |= widgets->drag_float("d_f", &geometry_.d_f, 0.1f, 1.f, 100.f);
                changed_ |= widgets->drag_float("fov_h_deg", &geometry_.fov_h_deg, 1.f, 10.f, 120.f);
                changed_ |= widgets->drag_float("aspect", &geometry_.aspect, 0.01f, 0.5f, 3.f);
                changed_ |= widgets->drag_float("array_angle_deg", &geometry_.array_angle_deg, 1.f, 5.f, 90.f);

                // Display derived parameters
                widgets->text(ocarina::format("W_f: {:.3f}", geometry_.W_f));
                widgets->text(ocarina::format("H_f: {:.3f}", geometry_.H_f));
            });

            // Statistics
            widgets->use_tree("Stats", [&] {
                widgets->text(ocarina::format("Pixels: {}x{} = {}",
                                              lenticular_.res_w, lenticular_.res_h, total_pixels()));
                widgets->text(ocarina::format("Subpixels: {} (rays)", total_subpixels()));
                widgets->text(ocarina::format("Views: {}", lenticular_.num_views));
            });

            if (changed_) {
                compute_derived_params();
            }
        });
    }
};

}// namespace vision

VS_MAKE_CLASS_CREATOR_HOTFIX(vision, LightFieldFrameBuffer)

// Keep the renderer DSL headers separate from Corona's host math headers.
#define VISION_PLUGIN_NAME "SVGF"
#define VISION_CATEGORY "denoiser"
#include "render_core/denoiser/SVGF/svgf.h"
#include "base/sensor/sensor.h"
#include "base/sampler.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstdlib>

// Illumination storage and validation are independent per layer. Coverage then
// integrates the complete footprint, including complementary endpoints.
template<typename Input, typename Filter>
void layered_history_regression(vision::Pipeline& pipeline, Filter& denoiser) {
    using namespace vision;
    if constexpr (!requires(Input input, Filter& filter) {
        input.layer_index; input.layer_count; input.stable_planes; input.prev_stable_planes;
        filter.svgf_buffer_cur(0u, 1u);
    }) {
        throw std::runtime_error("missing layered SVGF history contract");
    } else {
        struct Env {
            const char* name; std::string old;
            static void set(const char* name, const char* value) {
#ifdef _WIN32
                _putenv_s(name, value);
#else
                if (value[0]) setenv(name, value, 1); else unsetenv(name);
#endif
            }
            Env(const char* n, const char* v) : name(n), old(std::getenv(n) ? std::getenv(n) : "") { set(n, v); }
            ~Env() { set(name, old.c_str()); }
        } domain{"VISION_SVGF_RADIANCE_DOMAIN", "1"}, prefilter{"VISION_SVGF_SKIP_PREFILTER", "1"},
          atrous{"VISION_SVGF_SKIP_ATROUS", "1"}, resolve{"VISION_SVGF_SKIP_RESOLVE", "1"},
          variance{"VISION_SVGF_SKIP_VARIANCE", "0"};
        const uint n = pipeline.pixel_num();
        const uint center = n / 2u + pipeline.resolution().x / 2u;
        auto visibility = pipeline.frame_buffer()->cur_visibility_buffer_view(pipeline.frame_index() - 1u);
        auto current = pipeline.device().create_buffer<StablePlaneData>(n, "layered_test_current");
        auto previous = pipeline.device().create_buffer<StablePlaneData>(n, "layered_test_previous");
        auto direct = pipeline.device().create_buffer<RadType4>(n, "layered_test_direct");
        auto indirect = pipeline.device().create_buffer<RadType4>(n, "layered_test_indirect");
        Kernel make_guides = [&](BufferVar<TriangleHit> visible, BufferVar<StablePlaneData> out) {
            auto hit = visible.read(center);
            auto it = pipeline.geometry().compute_surface_interaction(hit, false);
            StablePlaneDataVar plane;
            plane.valid = 1u; plane.branch_sequence = 5u; plane.instance_hash = 37u;
            plane.depth = 1u; plane.material_id = it.material_id();
            plane.surface.hit = hit; plane.surface.is_replaced = true;
            plane.surface.stable_branch = 37u;
            plane.surface.virtual_position = it.pos;
            plane.surface.virtual_normal = it.ng;
            plane.surface.virtual_geometric_normal = it.ng;
            plane.surface.denoiser_albedo = make_float3(1.f);
            plane.depth_position = it.pos;
            out.write(dispatch_id(), plane);
        };
        auto guide_shader = pipeline.device().compile(make_guides, "layered_history_fixture");
        std::vector<StablePlaneData> guides(n);
        pipeline.stream() << guide_shader(visibility, current).dispatch(n) << current.download(guides.data())
            << synchronize() << commit();
        if (guides[center].surface.hit.inst_id == InvalidUI32) throw std::runtime_error("layer fixture needs geometry");
        auto baseline = guides;
        std::vector<RadType4> samples(n), output(n);
        std::vector<svgf::SVGFDataDual> history(n);
        Input input;
        input.resolution = pipeline.resolution(); input.layer_count = StablePlaneCount;
        input.use_stable_planes = true;
        input.stable_planes = current.view(); input.prev_stable_planes = previous.view();
        input.direct = direct.view(); input.indirect = indirect.view();
        input.visibility = input.prev_visibility = visibility;
        input.motion_vec = pipeline.frame_buffer()->motion_vectors();
        auto eye = pipeline.scene().sensor()->position();
        input.camera_pos = input.prev_camera_pos = {eye.x, eye.y, eye.z};
        input.channel_kind = Input::ChannelKind::DirectIndirect;
        auto run = [&](uint frame, uint layer, RadType4 color) {
            input.frame_index = frame; input.layer_index = layer;
            std::fill(samples.begin(), samples.end(), color);
            pipeline.stream() << current.upload(guides.data()) << previous.upload(baseline.data())
                << direct.upload(samples.data()) << indirect.upload(samples.data())
                << denoiser.dispatch(input) << direct.download(output.data())
                << denoiser.svgf_buffer_cur(frame, layer).download(history.data()) << synchronize() << commit();
        };
        const RadType4 colors[]{RadType4{1,0,0,1}, RadType4{0,1,0,1}, RadType4{0,0,1,1}};
        for (uint frame = 0; frame < 2; ++frame) for (uint layer = 0; layer < StablePlaneCount; ++layer) {
            run(frame, layer, colors[layer]);
            auto value = history[center].illumi_direct;
            if (std::abs(float(value.x)-float(colors[layer].x)) > 1e-5f ||
                std::abs(float(value.y)-float(colors[layer].y)) > 1e-5f ||
                std::abs(float(value.z)-float(colors[layer].z)) > 1e-5f ||
                float(history[center].moments_direct.z) < float(frame+1u)-0.01f)
                throw std::runtime_error("layer illumination and moments must retain independent colors and ages");
        }
        run(2u, 1u, RadType4{0,0,0,1});
        if (float(history[center].illumi_direct.y) < 0.5f || float(history[center].moments_direct.z) < 2.5f)
            throw std::runtime_error("valid zero samples must retain unbiased layer history");
        for (uint change = 0; change < 6; ++change) {
            guides = baseline; run(0u, 2u, colors[2]);
            for (auto& plane : guides) {
                if (change == 0) plane.branch_sequence ^= 1u;
                if (change == 1) plane.instance_hash ^= 1u;
                if (change == 2) plane.depth += 1u;
                if (change == 3) plane.material_id ^= 1u;
                if (change == 4) plane.valid = 0u;
                if (change == 5) plane.surface.hit.inst_id = InvalidUI32;
            }
            run(1u, 2u, colors[0]);
            const float age = float(history[center].moments_direct.z);
            if ((change < 4 && (age != 1.f || float(history[center].illumi_direct.z) != 0.f)) ||
                (change >= 4 && (age != 0.f || float(output[center].x) != 0.f)))
                throw std::runtime_error("changed or absent layer must reject and clear incompatible history");
        }
        Env::set("VISION_SVGF_SKIP_VARIANCE", "1");
        Env::set("VISION_SVGF_SKIP_RESOLVE", "0");
        auto all_current = pipeline.device().create_buffer<StablePlaneData>(n * StablePlaneCount, "coverage_all_current");
        auto all_previous = pipeline.device().create_buffer<StablePlaneData>(n * StablePlaneCount, "coverage_all_previous");
        auto primary = pipeline.device().create_buffer<TriangleHit>(n, "coverage_primary");
        std::vector<TriangleHit> primary_hits(n, baseline[center].surface.hit);
        const auto primary_hit = baseline[center].surface.hit;
        std::vector<StablePlaneData> all_planes(n * StablePlaneCount), last_planes(all_planes.size());
        auto coverage_run = [&](uint frame, float value, uint emission, bool colored = false) {
            input.frame_index = frame; input.layer_index = 0u;
            guides = baseline;
            for (uint i = 0u; i < n; ++i) {
                guides[i].branch_sequence = 5u + ((i + frame) & 1u);
                if (emission == 1u) guides[i].valid = 0u;
                if (emission == 2u) guides[i].surface.hit.inst_id = InvalidUI32;
                all_planes[i] = guides[i];
                all_planes[n + i] = guides[i];
                if (colored) {
                    const uint side = (i % input.resolution.x) >= input.resolution.x / 2u;
                    all_planes[i].branch_sequence = all_planes[n + i].branch_sequence = 5u + side;
                    all_planes[i].motion = make_float2(2.f, 0.f);
                    all_planes[n + i].motion = make_float2(-2.f, 0.f);
                }
                // Primary geometry is constant for the glass-internal edge.
                // Emission-only coverage has no valid illumination layer.
                primary_hits[i] = primary_hit;
                if (emission == 1u && (i & 1u)) primary_hits[i].inst_id = InvalidUI32;
            }
            if constexpr (requires(Input in) { in.composed_coverage; in.coverage_planes; in.prev_coverage_planes; }) {
                input.composed_coverage = true;
                input.layer_count = 1u; input.use_stable_planes = false;
                input.coverage_planes = all_current.view(); input.prev_coverage_planes = all_previous.view();
            }
            input.visibility = input.prev_visibility = primary.view();
            std::fill(samples.begin(), samples.end(), RadType4{value,value,value,1.f});
            if (colored) for (uint i = 0u; i < n; ++i) {
                const bool right = (i % input.resolution.x) >= input.resolution.x / 2u;
                samples[i] = colors[(right ^ (frame != 0u)) ? 2u : 0u];
            }
            pipeline.stream() << current.upload(guides.data()) << previous.upload(baseline.data())
                << all_current.upload(all_planes.data()) << all_previous.upload(last_planes.data())
                << primary.upload(primary_hits.data()) << direct.upload(samples.data()) << indirect.upload(samples.data())
                << denoiser.dispatch(input) << direct.download(output.data()) << synchronize() << commit();
            last_planes = all_planes;
            baseline = guides;
            return float(output[center].x);
        };
        auto original_guides = baseline;
        float internal = 0.f, emission = 0.f;
        for (uint frame = 0; frame < 16u; ++frame) internal = coverage_run(frame, float(frame & 1u), false);
        baseline = original_guides;
        for (uint frame = 0; frame < 16u; ++frame) emission = coverage_run(frame, float(frame & 1u), true);
        std::cout << "composed complementary coverage internal=" << internal << " emission=" << emission << '\n';
        if (std::abs(internal - 0.5f) > 1e-5f || std::abs(emission - 0.5f) > 1e-5f)
            throw std::runtime_error("complete coverage must average complementary endpoints and deterministic radiance exactly once");
        baseline = original_guides;
        float environment = 0.f;
        for (uint frame = 0; frame < 16u; ++frame) environment = coverage_run(frame, float(frame & 1u), 2u);
        if (std::abs(environment - 0.5f) > 1e-5f)
            throw std::runtime_error("reflected and refracted environment coverage must retain valid miss metadata without requiring illumination hits");
        baseline = original_guides;
        coverage_run(0u, 0.f, false);
        auto* sensor = pipeline.scene().sensor().get();
        const auto old_eye = sensor->position();
        sensor->set_position(old_eye + make_float3(0.1f, 0.f, 0.f)); sensor->update_device_data();
        const float moving_composite = coverage_run(1u, 1.f, false);
        if (std::abs(moving_composite - 1.f) > 1e-5f)
            throw std::runtime_error("translated multi-branch coverage must not reuse primary-only composite history");
        sensor->set_position(old_eye); sensor->update_device_data();
        coverage_run(0u, 0.f, false, true);
        sensor->set_position(old_eye + make_float3(0.1f, 0.f, 0.f)); sensor->update_device_data();
        coverage_run(1u, 0.f, false, true);
        const auto translated = output;
        denoiser.set_enabled(false); denoiser.set_enabled(true);
        coverage_run(1u, 0.f, false, true);
        float translated_error = 0.f;
        uint complementary_pixels = 0u;
        for (const auto& pixel : output)
            complementary_pixels += float(pixel.x) > 0.f && float(pixel.z) > 0.f;
        for (uint i = 0u; i < n; ++i) for (uint c = 0u; c < 3u; ++c) {
            if (!std::isfinite(float(translated[i][c])) || !std::isfinite(float(output[i][c])))
                throw std::runtime_error("translated internal colored boundary must remain finite");
            translated_error = std::max(translated_error, std::abs(float(translated[i][c]) - float(output[i][c])));
        }
        std::cout << "composed translated colored boundary versus current-only maxabs=" << translated_error
                  << " complementary pixels=" << complementary_pixels << '\n';
        // Individual jittered tent footprints can have no opposite-side sample;
        // the vertical boundary as a whole must reconstruct both target colors.
        if (translated_error > 1e-5f || complementary_pixels == 0u)
            throw std::runtime_error("translated internal colored boundary must reconstruct current complementary footprint without stale branches");
        sensor->set_position(old_eye); sensor->update_device_data();
        if constexpr (requires(Input in) { in.composed_coverage; }) input.composed_coverage = false;
        input.layer_count = StablePlaneCount; input.use_stable_planes = true;
        input.visibility = input.prev_visibility = visibility;
        baseline = original_guides;
        guides = baseline;
        Env::set("VISION_SVGF_SKIP_VARIANCE", "0");
        Env::set("VISION_SVGF_SKIP_RESOLVE", "1");
        run(0u, 1u, colors[1]); run(1u, 1u, colors[1]);
        run(4u, 1u, colors[0]);
        if (float(history[center].moments_direct.z) != 1.f || float(history[center].illumi_direct.y) != 0.f)
            throw std::runtime_error("resuming a skipped layer must not revive stale temporal history");
        for (uint toggle = 0; toggle < 4; ++toggle) {
            run(0u, 2u, colors[2]); run(1u, 2u, colors[2]);
            denoiser.set_enabled(false); denoiser.set_enabled(true);
            run(2u, 2u, colors[0]);
            if (float(history[center].moments_direct.z) != 1.f || float(history[center].illumi_direct.z) != 0.f)
                throw std::runtime_error("denoiser toggles must reset every layer's temporal validity");
        }
        // Different virtual projection geometry can still have matching optical
        // depth. Motion must come from the layer, not the primary-surface buffer.
        auto original = baseline;
        for (auto& plane : baseline) plane.surface.approximate = 1u;
        guides = baseline; run(0u, 1u, colors[1]);
        for (auto& plane : guides) plane.surface.virtual_position.z += 10.f;
        run(1u, 1u, colors[1]);
        if (float(history[center].moments_direct.z) < 1.9f)
            throw std::runtime_error("approximate temporal depth must use optical depth independently of projection position");
        for (auto& plane : guides) plane.depth_position.z += 20.f;
        run(2u, 1u, colors[0]);
        if (float(history[center].moments_direct.z) != 1.f)
            throw std::runtime_error("optical depth disocclusion must reject layer history");
        baseline = original; guides = baseline;
        run(0u, 1u, colors[1]);
        for (auto& plane : guides) plane.motion = make_float2(1000.f);
        run(1u, 1u, colors[0]);
        if (float(history[center].moments_direct.z) != 1.f)
            throw std::runtime_error("layer motion must override primary surface motion");
        guides = baseline;
        Env::set("VISION_SVGF_SKIP_PREFILTER", "0");
        Env::set("VISION_SVGF_SKIP_ATROUS", "0");
        Env::set("VISION_SVGF_SKIP_RESOLVE", "1");
        for (bool separated : {true, false}) {
            guides = baseline;
            std::fill(samples.begin(), samples.end(), colors[1]);
            samples[center] = colors[0];
            if (separated) for (uint i = 0u; i < n; ++i) if (i != center) guides[i].branch_sequence ^= 1u;
            input.frame_index = 0u; input.layer_index = 1u;
            pipeline.stream() << current.upload(guides.data()) << previous.upload(baseline.data())
                << direct.upload(samples.data()) << indirect.upload(samples.data())
                << denoiser.dispatch(input) << direct.download(output.data()) << synchronize() << commit();
            if (separated ? (std::abs(float(output[center].x) - 1.f) > 1e-5f || float(output[center].y) != 0.f)
                          : float(output[center].x) > 0.99f)
                throw std::runtime_error("spatial and coverage taps must filter matching paths while excluding neighboring branch colors");
        }
        Env::set("VISION_SVGF_SKIP_PREFILTER", "1");
        Env::set("VISION_SVGF_SKIP_ATROUS", "1");
        Env::set("VISION_SVGF_SKIP_RESOLVE", "1");
        guides = baseline;
        run(0u, 1u, colors[1]); run(1u, 1u, colors[1]);
        denoiser.update_resolution(input.resolution);
        run(2u, 1u, colors[0]);
        if (float(history[center].moments_direct.z) != 1.f || float(history[center].illumi_direct.y) != 0.f)
            throw std::runtime_error("resized buffers must not retain layer history or stale descriptors");
        pipeline.invalidate();
        std::cout << "PASS: layered SVGF colors, moments, zero samples, identity and stationary resolve isolation\n";
    }
}

void check_svgf_layered_history(vision::Pipeline& pipeline) {
    pipeline.activate_global_context();
    auto* illumination = dynamic_cast<vision::IlluminationIntegrator*>(pipeline.renderer().integrator().get());
    auto* denoiser = illumination ? dynamic_cast<vision::svgf::SVGF*>(illumination->denoiser()) : nullptr;
    if (!denoiser) throw std::runtime_error("layered history regression requires SVGF");
    layered_history_regression<vision::RealTimeDenoiseInput>(pipeline, *denoiser);
}

void check_svgf_shading_guide(vision::Pipeline& pipeline) {
    using namespace vision;
    pipeline.activate_global_context();
    // Reuse the normal-mapped substrate fixture prepared by the sampling test.
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    const auto visibility = pipeline.frame_buffer()->cur_visibility_buffer_view(pipeline.frame_index() - 1u);
    std::vector<TriangleHit> hits(pipeline.pixel_num());
    pipeline.stream() << visibility.subview(0, hits.size()).download(hits.data()) << synchronize() << commit();
    if (hits[8u * 16u + 8u].inst_id == InvalidUI32) {
        throw std::runtime_error("normal guide fixture must have visible geometry at its center");
    }
    bool& individual = MaterialRegistry::instance().individual_ns();
    struct RestoreMode { bool& mode; bool value; ~RestoreMode() { mode = value; } } restore{individual, individual};
    for (bool enabled : {true, false}) {
        individual = enabled;
        // Exercise the same visibility-buffer compute path as SVGF.
        Kernel kernel = [&](BufferVar<TriangleHit> visible, BufferVar<float4> out) {
            Float3 eye = make_float3(0.f, 0.f, 3.f);
            auto hit = visible.read(8u * 16u + 8u);
            auto it = pipeline.geometry().compute_surface_interaction(hit, eye);
            Float3 normal = svgf::PixelStateUtils::query_shading_normal(&pipeline, hit, eye);
            out.write(0u, make_float4(normal, dot(normal, it.shading.normal())));
        };
        auto shader = pipeline.device().compile(kernel, enabled ? "svgf_mapped_normal_guide" : "svgf_shared_normal_guide");
        auto out = pipeline.device().create_buffer<float4>(1, "svgf_normal_guide_test");
        float4 normal{};
        pipeline.stream() << shader(visibility, out).dispatch(1u) << out.download(&normal) << synchronize() << commit();
        const float norm2 = normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
        if (!std::isfinite(norm2) || std::abs(norm2 - 1.f) > 1e-4f ||
            (enabled ? normal.w > 0.9f : std::abs(normal.w - 1.f) > 1e-4f)) {
            throw std::runtime_error("SVGF guide must be normalized and follow the active material normal mode");
        }
    }
    std::cout << "PASS: SVGF guides follow material normals and shared-frame mode\n";
}

void check_svgf_spatial_bypass(vision::Pipeline& pipeline) {
    using namespace vision;
    pipeline.activate_global_context();
    auto* illumination = dynamic_cast<IlluminationIntegrator*>(pipeline.renderer().integrator().get());
    auto* denoiser = illumination ? dynamic_cast<svgf::SVGF*>(illumination->denoiser()) : nullptr;
    if (!denoiser) throw std::runtime_error("spatial bypass regression requires SVGF");
    struct EnvRestore {
        const char* name;
        std::string previous;
        bool present;
        static void set(const char* key, const char* value) {
#ifdef _WIN32
            _putenv_s(key, value ? value : "");
#else
            if (value) setenv(key, value, 1); else unsetenv(key);
#endif
        }
        EnvRestore(const char* key, const char* value) : name(key),
            previous(std::getenv(key) ? std::getenv(key) : ""), present(std::getenv(key) != nullptr) { set(key, value); }
        ~EnvRestore() { set(name, present ? previous.c_str() : nullptr); }
    } domain{"VISION_SVGF_RADIANCE_DOMAIN", "1"}, temporal{"VISION_SVGF_SKIP_VARIANCE", "1"},
      prefilter{"VISION_SVGF_SKIP_PREFILTER", "1"}, atrous{"VISION_SVGF_SKIP_ATROUS", "0"},
      resolve{"VISION_SVGF_SKIP_RESOLVE", "1"};
    const uint count = pipeline.pixel_num();
    std::vector<RadType4> samples(count);
    for (uint i = 0; i < count; ++i) {
        const float value = ((i % 16u + i / 16u) & 1u) ? 0.7f : 0.3f;
        samples[i] = RadType4{value, value, value, 1.f};
    }
    auto direct = pipeline.device().create_buffer<RadType4>(count, "svgf_bypass_direct");
    auto indirect = pipeline.device().create_buffer<RadType4>(count, "svgf_bypass_indirect");
    std::vector<svgf::SVGFDataDual> empty(count);
    RealTimeDenoiseInput input;
    input.resolution = pipeline.resolution();
    input.frame_index = pipeline.frame_index() - 1u;
    input.direct = direct.view(); input.indirect = indirect.view();
    input.visibility = pipeline.frame_buffer()->cur_visibility_buffer_view(input.frame_index);
    const auto eye = pipeline.scene().sensor()->position();
    input.camera_pos = {eye.x, eye.y, eye.z};
    pipeline.stream() << direct.upload(samples.data()) << indirect.upload(samples.data())
        << denoiser->svgf_data.view().upload(empty.data()) << denoiser->svgf_data2.view().upload(empty.data())
        << illumination->denoiser()->dispatch(input) << direct.download(samples.data()) << synchronize() << commit();
    const float center = static_cast<float>(samples[8u * 16u + 8u].x);
    if (!std::isfinite(center) || center < 0.35f || center > 0.65f) {
        throw std::runtime_error("bypassing temporal estimation must still spatially filter noisy geometry with empty guide caches");
    }
    std::cout << "PASS: spatial filtering survives the temporal debug bypass\n";

    // Isolate the post-filter coverage resolve from illumination accumulation.
    // A moving geometric edge must blend valid prior coverage, while interiors,
    // newly exposed surfaces, off-screen history and invalidation use only the
    // current spatial coverage estimate.
    EnvRestore resolve_enabled{"VISION_SVGF_SKIP_RESOLVE", "0"},
        atrous_disabled{"VISION_SVGF_SKIP_ATROUS", "1"};
    std::vector<TriangleHit> geometry(count);
    pipeline.stream() << input.visibility.download(geometry.data()) << synchronize() << commit();
    const uint edge_pixel = 8u * 16u + 8u;
    if (geometry[edge_pixel].inst_id == InvalidUI32) throw std::runtime_error("coverage fixture must hit geometry");
    const auto receiver_hit = geometry[edge_pixel];
    for (uint y = 0; y < 16u; ++y)
        for (uint x = 9u; x < 16u; ++x) geometry[y * 16u + x].inst_id = InvalidUI32;
    auto current_visibility = pipeline.device().create_buffer<TriangleHit>(count, "coverage_current_visibility");
    auto previous_visibility = pipeline.device().create_buffer<TriangleHit>(count, "coverage_previous_visibility");
    auto motion = pipeline.device().create_buffer<float2>(count, "coverage_motion");
    std::vector<float2> offsets(count, make_float2(-0.25f, 0.f));
    input.visibility = current_visibility.view(); input.prev_visibility = previous_visibility.view();
    input.channel_kind = RealTimeDenoiseInput::ChannelKind::DirectIndirect;
    input.motion_vec = motion.view();
    input.prev_camera_pos = input.camera_pos;
    pipeline.stream() << current_visibility.upload(geometry.data()) << previous_visibility.upload(geometry.data())
        << motion.upload(offsets.data()) << synchronize() << commit();
    auto resolve_sample = [&](uint frame, float value) {
        for (auto& sample : samples) sample = RadType4{value, value, value, 1.f};
        if (frame == 0u && value < 0.3f) {
            for (uint i = 0; i < count; ++i)
                if (i % 16u >= 9u) samples[i] = RadType4{0.6f, 0.6f, 0.6f, 1.f};
        }
        samples[edge_pixel - 1u] = RadType4{0.2f, 0.2f, 0.2f, 1.f};
        input.frame_index = frame;
        pipeline.scene().sensor()->set_yaw(float(frame) * 0.1f);
        pipeline.scene().sensor()->update_device_data();
        pipeline.stream() << direct.upload(samples.data()) << indirect.upload(samples.data())
            << denoiser->dispatch(input) << direct.download(samples.data()) << synchronize() << commit();
        return float(samples[edge_pixel].x);
    };
    resolve_sample(0u, 0.2f);
    const float moving_edge = resolve_sample(1u, 0.8f);
    const float moving_interior = float(samples[8u * 16u + 4u].x);
    input.channel_kind = RealTimeDenoiseInput::ChannelKind::DiffuseSpecular;
    resolve_sample(0u, 0.2f);
    const float pt_moving = resolve_sample(1u, 0.8f);
    input.channel_kind = RealTimeDenoiseInput::ChannelKind::DirectIndirect;
    const float invalidated = resolve_sample(0u, 0.8f);
    for (auto& hit : geometry) hit.inst_id = InvalidUI32;
    pipeline.stream() << previous_visibility.upload(geometry.data()) << synchronize() << commit();
    const float disoccluded = resolve_sample(1u, 0.8f);
    denoiser->set_enabled(false);
    denoiser->set_enabled(true);
    const float current_only = resolve_sample(1u, 0.8f);
    pipeline.stream() << current_visibility.download(geometry.data()) << synchronize() << commit();
    // Keep the in-bounds part valid: rejection must come from the incomplete
    // footprint, not geometry mismatch or the >=32px motion weight becoming 1.
    geometry[8u * 16u] = receiver_hit;
    pipeline.stream() << previous_visibility.upload(geometry.data()) << synchronize() << commit();
    resolve_sample(0u, 0.2f);
    for (auto& offset : offsets) offset = make_float2(8.25f, 0.f);
    pipeline.stream() << motion.upload(offsets.data()) << synchronize() << commit();
    const float offscreen = resolve_sample(1u, 0.8f);
    std::cout << "Coverage resolve moving=" << moving_edge << " interior=" << moving_interior
              << " reset=" << invalidated << " disoccluded=" << disoccluded << " offscreen=" << offscreen
              << " pt_moving=" << pt_moving << '\n';
    // Current coverage depends on this frame's film sample; valid history must
    // still damp the transition, and absent/offscreen history must match a
    // freshly invalidated resolve at exactly the same sample phase.
    if (!std::isfinite(moving_edge) || moving_edge < 0.2f || moving_edge >= current_only - 0.05f)
        throw std::runtime_error("moving geometric edges need bounded reprojected coverage history");
    if (!std::isfinite(moving_interior) || !std::isfinite(invalidated) ||
        !std::isfinite(disoccluded) || !std::isfinite(offscreen) || !std::isfinite(pt_moving) ||
        std::abs(moving_interior - 0.8f) > 1e-5f || invalidated < 0.2f || invalidated > 0.8f ||
        std::abs(disoccluded - current_only) > 1e-5f || std::abs(offscreen - current_only) > 1e-5f ||
        std::abs(pt_moving - 0.8f) > 1e-5f)
        throw std::runtime_error("coverage resolve must preserve moving interiors and reject invalid history");
    std::cout << "PASS: moving coverage is stable without freezing interiors or disocclusions\n";

    // A subpixel dark part can be the sole geometry sample in a bright pixel
    // footprint (e.g. the radio grille). Same-instance-only reconstruction
    // leaves it solid black even when the surrounding footprint is bright.
    for (auto& hit : geometry) hit.inst_id = InvalidUI32;
    geometry[edge_pixel] = receiver_hit;
    pipeline.stream() << current_visibility.upload(geometry.data()) << synchronize() << commit();
    // Irregular samples preserve constants after normalization; a single
    // sample no longer represents exactly one pixel of integrated area.
    // Read actual film positions and independently reconstruct its footprint.
    auto* camera = pipeline.scene().sensor().get();
    auto* film_sampler = pipeline.renderer().sampler().get();
    Kernel film_positions = [&](BufferVar<float2> output) {
        camera->load_data();
        film_sampler->load_data();
        Uint2 pixel = dispatch_idx().xy();
        film_sampler->set_seed(pixel, 0u, Dimension::Camera);
        Float2 offset = camera->filter()->sample(film_sampler->next_2d()).p;
        output.write(pixel.y * 16u + pixel.x, make_float2(pixel) + offset);
    };
    auto positions_shader = pipeline.device().compile(film_positions, "coverage_film_positions");
    auto positions_buffer = pipeline.device().create_buffer<float2>(count, "coverage_film_positions");
    std::vector<float2> positions(count);
    pipeline.stream() << positions_shader(positions_buffer).dispatch(input.resolution)
        << positions_buffer.download(positions.data()) << synchronize() << commit();
    std::vector<float> expected_coverage(count, 0.f);
    for (uint i = 0; i < count; ++i) {
        float weight_sum = 0.f, feature_weight = 0.f;
        for (uint j = 0; j < count; ++j) {
            const float weight = std::max(0.f, 1.f - std::abs(positions[j].x - float(i % 16u))) *
                                 std::max(0.f, 1.f - std::abs(positions[j].y - float(i / 16u)));
            weight_sum += weight;
            if (j == edge_pixel) feature_weight = weight;
        }
        expected_coverage[i] = feature_weight / std::max(weight_sum, 1e-6f);
    }
    auto isolated_coverage = [&](bool invert) {
        for (uint i = 0; i < count; ++i) {
            float value = i == edge_pixel ? 0.f : 1.f;
            if (invert) value = 1.f - value;
            samples[i] = RadType4{value, value, value, 0.f};
        }
        input.frame_index = 0u; // no temporal information is available
        pipeline.stream() << direct.upload(samples.data()) << indirect.upload(samples.data())
            << denoiser->dispatch(input) << direct.download(samples.data()) << synchronize() << commit();
        for (uint i = 0; i < count; ++i) {
            const float contribution = invert ? float(samples[i].x) : 1.f - float(samples[i].x);
            if (!std::isfinite(contribution))
                throw std::runtime_error("coverage reconstruction must remain finite");
            if (std::abs(contribution - expected_coverage[i]) > 1e-4f)
                throw std::runtime_error("coverage must match normalized reconstruction at actual film positions");
            if ((std::abs(int(i % 16u) - 8) > 1 || std::abs(int(i / 16u) - 8) > 1) &&
                std::abs(contribution) > 1e-6f)
                throw std::runtime_error("coverage reconstruction must not spread a subpixel feature beyond its one-pixel footprint");
        }
        return float(samples[edge_pixel].x);
    };
    const float dark_coverage = isolated_coverage(false);
    const float light_coverage = isolated_coverage(true);
    std::cout << "Isolated coverage dark=" << dark_coverage << " light=" << light_coverage << '\n';
    if (!(dark_coverage > 0.1f && dark_coverage < 0.9f) ||
        std::abs(dark_coverage + light_coverage - 1.f) > 1e-5f)
        throw std::runtime_error("subpixel coverage must include neighbouring surfaces and preserve complementary edge energy");

    // A noise-free, low-contrast lighting edge on one plane should survive the
    // wide wavelet passes. Fixed radiance floors at every step flatten it.
    EnvRestore filter_enabled{"VISION_SVGF_SKIP_ATROUS", "0"},
        resolve_disabled{"VISION_SVGF_SKIP_RESOLVE", "1"};
    for (auto& hit : geometry) hit = receiver_hit;
    for (uint i = 0; i < count; ++i) {
        const float value = i % 16u < 8u ? 0.1f : 0.16f;
        samples[i] = RadType4{value, value, value, 0.f};
    }
    input.channel_kind = RealTimeDenoiseInput::ChannelKind::DirectIndirect;
    input.frame_index = 0u;
    pipeline.stream() << current_visibility.upload(geometry.data())
        << direct.upload(samples.data()) << indirect.upload(samples.data())
        << denoiser->dispatch(input) << direct.download(samples.data()) << synchronize() << commit();
    const float contrast = float(samples[8u * 16u + 11u].x) - float(samples[8u * 16u + 4u].x);
    std::cout << "ReSTIR low-variance shadow contrast=" << contrast << " (input=0.06)\n";
    if (!std::isfinite(contrast) || contrast < 0.03f || contrast > 0.06f)
        throw std::runtime_error("wide ReSTIR SVGF passes must preserve at least half of a stable shadow edge's contrast");
    std::cout << "PASS: wide ReSTIR filtering preserves low-variance lighting detail\n";

    // A mature but noisy history must pass its measured variance to a-trous.
    // Using variance as lerp's first argument extrapolates for variance > 1,
    // turns it negative and silently replaces it with the minimum floor.
    EnvRestore prefilter_enabled{"VISION_SVGF_SKIP_PREFILTER", "0"},
        atrous_off{"VISION_SVGF_SKIP_ATROUS", "1"};
    std::vector<svgf::SVGFDataDual> noisy_history(count);
    for (auto& h : noisy_history) {
        h.illumi_direct = RadType4{1.f, 1.f, 1.f, 6.f};
        h.illumi_indirect = RadType4{1.f, 1.f, 1.f, 105.f};
        h.moments_direct = RadType4{1.f, 7.f, 32.f, 32.f};
        h.moments_indirect = RadType4{1.f, 106.f, 1.f, 1.f};
    }
    auto& variance_history = (input.frame_index & 1u) ? denoiser->svgf_data2 : denoiser->svgf_data;
    pipeline.stream() << variance_history.view().upload(noisy_history.data())
        << denoiser->dispatch(input) << direct.download(samples.data()) << synchronize() << commit();
    const float variance_direct = float(samples[edge_pixel].w);
    pipeline.stream() << indirect.download(samples.data()) << synchronize() << commit();
    const float variance_indirect = float(samples[edge_pixel].w);
    std::cout << "Mature prefilter variance direct=" << variance_direct << " indirect=" << variance_indirect << '\n';
    if (!std::isfinite(variance_direct) || !std::isfinite(variance_indirect) ||
        std::abs(variance_direct - 6.f) > 1e-4f || std::abs(variance_indirect - 105.f) > 1e-4f)
        throw std::runtime_error("prefilter must preserve mature high variance instead of classifying it as noise-free");
    std::cout << "PASS: mature prefilter preserves measured high variance\n";

    // The Gaussian variance prefilter widens the edge-stopping guide only.
    // Rejected sky taps cannot add uncertainty to the filtered surface signal.
    EnvRestore prefilter_off{"VISION_SVGF_SKIP_PREFILTER", "1"},
        atrous_on{"VISION_SVGF_SKIP_ATROUS", "0"};
    for (auto& hit : geometry) hit.inst_id = InvalidUI32;
    geometry[edge_pixel] = receiver_hit;
    for (auto& sample : samples) sample = RadType4{0.f, 0.f, 0.f, 100.f};
    samples[edge_pixel] = RadType4{1.f, 1.f, 1.f, 0.25f};
    pipeline.stream() << current_visibility.upload(geometry.data())
        << direct.upload(samples.data()) << indirect.upload(samples.data())
        << denoiser->dispatch(input) << direct.download(samples.data()) << synchronize() << commit();
    const float isolated_variance = float(samples[edge_pixel].w);
    std::cout << "Isolated surface propagated variance=" << isolated_variance << '\n';
    if (!std::isfinite(isolated_variance) || std::abs(isolated_variance - 0.25f) > 1e-4f ||
        std::abs(float(samples[edge_pixel].x) - 1.f) > 1e-5f)
        throw std::runtime_error("a-trous must not propagate Gaussian guide variance across rejected geometry boundaries");
    std::cout << "PASS: rejected neighbours cannot contaminate propagated variance\n";
}

void check_substrate_sample_classification(vision::Pipeline& pipeline) {
    using namespace vision;
    pipeline.activate_global_context();
    MaterialDesc desc;
    desc.init(ParameterSet{DataWrap::parse(R"({
        "type":"substrate","name":"substrate-routing-test",
        "param":{"color":[0.8,0.5,0.2],"spec":[0.05,0.05,0.05],"roughness":0.3,
                 "normal":{"channels":"xyz","node":"guide-normal"}},
        "node_tab":{"guide-normal":{"type":"number","param":{"value":[0.6,0.0,0.8]}}}
    })")});
    auto material = Material::create_root(desc);
    pipeline.scene().add_material(material);
    pipeline.scene().instances()[0]->set_material(material);
    pipeline.scene().prepare();
    pipeline.upload_data();
    // This fixture replaces material storage outside the scene-import path.
    // Publish the new bindless slot before any shader reads normal-map data.
    pipeline.upload_scene_bindless_array();
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    auto& sampler = pipeline.renderer().sampler();
    Kernel kernel = [&](BufferVar<uint> out) {
        sampler->load_data();
        sampler->set_seed(make_uint2(0u), dispatch_id(), Dimension::PathTracing);
        auto ray = make_ray(Float3{make_float3(0.f, 0.f, 3.f)}, Float3{make_float3(0.f, 0.f, -1.f)});
        auto hit = pipeline.geometry().trace_closest(ray);
        auto it = pipeline.geometry().compute_surface_interaction(hit, ray);
        auto swl = pipeline.renderer().integrator()->spectrum()->sample_wavelength(sampler);
        MaterialEvaluator evaluator(it, swl);
        pipeline.scene().materials().dispatch(it.material_id(), [&](const Material* current) {
            current->build_evaluator(evaluator, it, swl);
        });
        auto sampled = evaluator.sample(it.wo, sampler);
        out.write(dispatch_id(), sampled.eval.flags);
    };
    auto shader = pipeline.device().compile(kernel, "substrate_sample_classification");
    std::vector<uint> flags(32);
    auto out = pipeline.device().create_buffer<uint>(flags.size(), "substrate_flags");
    pipeline.stream() << shader(out).dispatch(static_cast<uint>(flags.size()))
                      << out.download(flags.data()) << synchronize() << commit();
    for (auto value : flags) {
        if ((value & BxDFFlag::GlossyRefl) != BxDFFlag::GlossyRefl) {
            throw std::runtime_error("substrate sampled reflection must retain the glossy flag used by its direct-light and albedo split");
        }
    }
    std::cout << "PASS: substrate samples retain glossy reflection classification\n";
}

void check_motion_visibility_history(vision::Pipeline& pipeline) {
    auto* sensor = pipeline.scene().sensor().get();
    sensor->set_position(vision::make_float3(0.f, 0.f, 3.f));
    sensor->set_yaw(0.f); sensor->set_pitch(0.f); sensor->set_fov_y(45.f);
    sensor->update_device_data();
    pipeline.invalidate();
    pipeline.upload_data();
    pipeline.display(1.0 / 60.0);
    sensor->set_position(vision::make_float3(100.f, 0.f, 3.f));
    sensor->update_device_data();
    pipeline.upload_data();
    pipeline.display(1.0 / 60.0);
    std::vector<vision::TriangleHit> current(pipeline.pixel_num()), previous(current.size());
    const auto rendered_frame = pipeline.frame_index() - 1u;
    // Bound downloads even on the old, incorrectly oversized half-zero view.
    const auto cur = pipeline.frame_buffer()->cur_visibility_buffer_view(rendered_frame).subview(0, current.size());
    const auto prev = pipeline.frame_buffer()->prev_visibility_buffer_view(rendered_frame).subview(0, previous.size());
    pipeline.stream() << cur.download(current.data()) << prev.download(previous.data())
                      << vision::synchronize() << vision::commit();
    const auto center = 8u * 16u + 8u;
    std::cout << "Visibility after camera cut: current=" << current[center].inst_id
              << " previous=" << previous[center].inst_id << '\n';
    if (current[center].inst_id != vision::InvalidUI32 || previous[center].inst_id == vision::InvalidUI32) {
        throw std::runtime_error("current GBuffer writes must preserve the actual previous frame geometry for disocclusion");
    }
}

void check_camera_dolly_reprojection(vision::Pipeline& pipeline) {
    auto expect = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    auto* illumination = dynamic_cast<vision::IlluminationIntegrator*>(pipeline.renderer().integrator().get());
    auto* svgf = illumination ? dynamic_cast<vision::svgf::SVGF*>(illumination->denoiser()) : nullptr;
    expect(svgf != nullptr, "dolly regression must inspect the actual SVGF history");
    auto* sensor = pipeline.scene().sensor().get();
    sensor->set_position(vision::make_float3(0.f, 0.f, 3.f));
    sensor->set_yaw(0.f); sensor->set_pitch(0.f); sensor->set_fov_y(45.f);
    sensor->update_device_data();
    pipeline.invalidate();
    pipeline.upload_data();
    for (int i = 0; i < 16; ++i) {
        pipeline.upload_data();
        pipeline.display(1.0 / 60.0);
    }
    float center_specular_history = 0.f;
    auto center_history = [&]() {
        std::vector<vision::svgf::SVGFDataDual> history(pipeline.pixel_num());
        std::vector<vision::TriangleHit> visibility(pipeline.pixel_num());
        const auto resolution = pipeline.resolution();
        const auto center_index = (resolution.y / 2u) * resolution.x + resolution.x / 2u;
        const auto rendered_frame = pipeline.frame_index() - 1;
        auto& buffer = (rendered_frame & 1u) == 0u ? svgf->svgf_data : svgf->svgf_data2;
        pipeline.stream() << buffer.view().download(history.data())
            << pipeline.frame_buffer()->cur_visibility_buffer_view(rendered_frame).subview(0, visibility.size()).download(visibility.data())
            << vision::synchronize() << vision::commit();
        expect(visibility[center_index].inst_id != vision::InvalidUI32,
               "dolly regression center must hit the triangle, not stale sky history");
        center_specular_history = static_cast<float>(history[center_index].moments_direct.w);
        return static_cast<float>(history[center_index].moments_direct.z);
    };
    expect(center_history() > 1.5f, "dolly regression needs valid initial history");
    // A jittered boundary can miss this surface at exactly the reprojected
    // pixel while adjacent previous pixels still contain valid same-surface
    // illumination. Exercise that hole without accepting unrelated geometry.
    const auto center = 8u * 16u + 8u;
    auto previous_visibility = pipeline.frame_buffer()->cur_visibility_buffer_view(pipeline.frame_index() - 1u);
    vision::TriangleHit missing{};
    missing.inst_id = vision::InvalidUI32;
    pipeline.stream() << previous_visibility.subview(center, 1).upload(&missing)
                      << vision::synchronize() << vision::commit();
    pipeline.upload_data();
    pipeline.display(1.0 / 60.0);
    expect(center_history() > 1.5f,
           "a rejected bilinear tap must recover valid same-surface history from adjacent previous pixels");
    previous_visibility = pipeline.frame_buffer()->cur_visibility_buffer_view(pipeline.frame_index() - 1u);
    // Remove all support, including the shifted jitter footprint and fallback.
    std::vector<vision::TriangleHit> disoccluded_visibility(pipeline.pixel_num(), missing);
    pipeline.stream() << previous_visibility.upload(disoccluded_visibility.data());
    pipeline.stream() << vision::synchronize() << vision::commit();
    pipeline.upload_data();
    pipeline.display(1.0 / 60.0);
    expect(center_history() == 1.f,
           "a disoccluded surface with no matching previous neighbours must still reject history");
    expect(center_specular_history == 1.f,
           "disocclusion must also reset independent specular history");
    pipeline.invalidate();
    for (int i = 0; i < 16; ++i) {
        pipeline.upload_data();
        pipeline.display(1.0 / 60.0);
    }
    // The same stationary triangle remains visible. Moving the eye by 0.2
    // changes current-eye distance by > 3%, but not previous-eye depth.
    sensor->set_position(vision::make_float3(0.f, 0.f, 2.8f));
    sensor->update_device_data();
    pipeline.upload_data();
    pipeline.display(1.0 / 60.0);
    const float reprojected_history = center_history();
    std::cout << "Dolly center history=" << reprojected_history << '\n';
    expect(reprojected_history > 1.5f,
           "dolly movement must compare both surface depths from the previous camera, retaining valid history");

    // This diffuse surface has view-independent lighting: a valid lateral
    // reprojection must not discard its accumulated samples just to track gloss.
    sensor->set_position(vision::make_float3(0.4f, 0.f, 2.8f));
    sensor->update_device_data();
    pipeline.upload_data();
    pipeline.display(1.0 / 60.0);
    const float moving_history = center_history();
    expect(moving_history > 12.f,
           "valid moving diffuse surfaces must retain accumulated illumination independently of specular responsiveness");
    expect(center_specular_history > 1.5f && center_specular_history < 12.f,
           "moving specular history must still match its responsive effective alpha");
    const float moving_specular_history = center_specular_history;
    for (int i = 0; i < 16; ++i) {
        pipeline.upload_data();
        pipeline.display(1.0 / 60.0);
    }
    const float stopped_history = center_history();
    std::cout << "Motion effective history=" << moving_history << " stopped=" << stopped_history << '\n';
    expect(stopped_history > moving_history + 8.f,
           "stationary history must recover after camera movement");
    expect(center_specular_history > moving_specular_history + 8.f,
           "specular history must independently recover after camera movement");

    // Turning at a fixed eye moves pixels without changing the outgoing
    // direction at a reprojected world-space surface point.
    sensor->set_position(vision::make_float3(0.f, 0.f, 3.f));
    sensor->set_yaw(0.f);
    sensor->update_device_data();
    pipeline.invalidate();
    for (int i = 0; i < 16; ++i) {
        pipeline.upload_data();
        pipeline.display(1.0 / 60.0);
    }
    sensor->set_yaw(6.f);
    sensor->update_device_data();
    pipeline.upload_data();
    pipeline.display(1.0 / 60.0);
    center_history();
    std::cout << "Rotation specular history=" << center_specular_history << '\n';
    expect(center_specular_history > 14.f,
           "pure camera rotation must retain valid specular history when the world-space view direction is unchanged");

    // The sensor's FOV spans the shorter dimension. Swapping width/height
    // must not change the angular response at the same surface point.
    // Center the 0.4-unit translation about the triangle so the entire
    // independently jittered center-pixel footprint stays inside its edges.
    float landscape_history = 0.f;
    for (bool portrait : {false, true}) {
        pipeline.change_resolution(portrait ? vision::make_uint2(16u, 24u)
                                            : vision::make_uint2(24u, 16u));
        sensor->set_position(vision::make_float3(-0.2f, 0.f, 3.f));
        sensor->set_yaw(0.f);
        sensor->update_device_data();
        pipeline.invalidate();
        for (int i = 0; i < 16; ++i) {
            pipeline.upload_data();
            pipeline.display(1.0 / 60.0);
        }
        sensor->set_position(vision::make_float3(0.2f, 0.f, 3.f));
        sensor->update_device_data();
        pipeline.upload_data();
        pipeline.display(1.0 / 60.0);
        center_history();
        expect(center_specular_history > 1.5f && center_specular_history < 12.f,
               "both image orientations must remain responsive to view parallax");
        std::cout << "Aspect specular history portrait=" << portrait << " history="
                  << center_specular_history << '\n';
        if (!portrait) landscape_history = center_specular_history;
        else expect(std::abs(center_specular_history - landscape_history) < landscape_history * 0.1f,
                    "portrait and landscape views with the same short side must use the same angular history response");
    }
    // The following spatial fixture uses a 16x16 visibility buffer.
    pipeline.change_resolution(vision::make_uint2(16u, 16u));
    sensor->set_position(vision::make_float3(0.f, 0.f, 3.f));
    sensor->update_device_data();
    pipeline.invalidate();
    pipeline.upload_data();
    pipeline.display(1.0 / 60.0);

}

void check_svgf_restir_motion_history(vision::Pipeline& pipeline) {
    using namespace vision;
    pipeline.activate_global_context();
    auto* illumination = dynamic_cast<IlluminationIntegrator*>(pipeline.renderer().integrator().get());
    auto* denoiser = illumination ? dynamic_cast<svgf::SVGF*>(illumination->denoiser()) : nullptr;
    if (!denoiser) throw std::runtime_error("ReSTIR temporal regression requires SVGF");
    auto* sensor = pipeline.scene().sensor().get();
    const uint count = pipeline.pixel_num();
    const uint center = (pipeline.resolution().y / 2u) * pipeline.resolution().x + pipeline.resolution().x / 2u;
    auto direct = pipeline.device().create_buffer<RadType4>(count, "restir_temporal_direct");
    auto indirect = pipeline.device().create_buffer<RadType4>(count, "restir_temporal_indirect");
    std::vector<RadType4> signal(count, RadType4{1.f, 1.f, 1.f, 1.f});
    std::vector<svgf::SVGFDataDual> history(count);
    for (auto& h : history) {
        h.illumi_direct = h.illumi_indirect = RadType4{1.f, 1.f, 1.f, 0.f};
        h.moments_direct = RadType4{1.f, 1.f, 16.f, 16.f};
        h.moments_indirect = RadType4{1.f, 1.f, 1.f, 1.f};
    }
    auto measure = [&](bool translate, bool disoccluded, bool moving_thin_edge = false) {
        sensor->set_position(make_float3(0.f, 0.f, 3.f));
        sensor->set_yaw(0.f);
        sensor->update_device_data();
        pipeline.invalidate();
        pipeline.upload_data();
        pipeline.display(1.0 / 60.0);
        const auto prev_eye = sensor->position();
        if (translate) sensor->set_position(make_float3(0.4f, 0.f, 3.f));
        else sensor->set_yaw(6.f);
        sensor->update_device_data();
        pipeline.upload_data();
        pipeline.display(1.0 / 60.0);
        RealTimeDenoiseInput input;
        input.frame_index = pipeline.frame_index() - 1u;
        input.resolution = pipeline.resolution();
        input.channel_kind = RealTimeDenoiseInput::ChannelKind::DirectIndirect;
        input.direct = direct.view(); input.indirect = indirect.view();
        input.visibility = pipeline.frame_buffer()->cur_visibility_buffer_view(input.frame_index);
        input.prev_visibility = pipeline.frame_buffer()->prev_visibility_buffer_view(input.frame_index);
        input.motion_vec = pipeline.frame_buffer()->motion_vectors();
        const auto eye = sensor->position();
        input.camera_pos = {eye.x, eye.y, eye.z};
        input.prev_camera_pos = {prev_eye.x, prev_eye.y, prev_eye.z};
        std::vector<TriangleHit> missing(count);
        for (auto& hit : missing) hit.inst_id = InvalidUI32;
        if (disoccluded) pipeline.stream() << input.prev_visibility.upload(missing.data());
        auto synthetic_motion = pipeline.device().create_buffer<float2>(count, "thin_edge_motion");
        std::vector<float2> offsets(count, make_float2(0.5f));
        if (moving_thin_edge) {
            TriangleHit receiver;
            pipeline.stream() << input.visibility.subview(center, 1).download(&receiver)
                << synchronize() << commit();
            // All four bilinear taps miss the thin surface. A neighbour on the
            // same plane is still visible, one pixel beyond that footprint.
            missing[center + 1u] = receiver;
            pipeline.stream() << input.prev_visibility.upload(missing.data())
                << synthetic_motion.upload(offsets.data());
            input.motion_vec = synthetic_motion.view();
        }
        std::vector<svgf::SVGFDataDual> result(count);
        auto& current_history = (input.frame_index & 1u) == 0u ? denoiser->svgf_data : denoiser->svgf_data2;
        pipeline.stream() << direct.upload(signal.data()) << indirect.upload(signal.data())
            << denoiser->svgf_data.view().upload(history.data())
            << denoiser->svgf_data2.view().upload(history.data())
            << denoiser->dispatch(input)
            << current_history.view().download(result.data())
            << synchronize() << commit();
        return make_float2(float(result[center].moments_direct.z), float(result[center].moments_direct.w));
    };
    const auto rotation = measure(false, false);
    const auto translation = measure(true, false);
    const auto disocclusion = measure(false, true);
    const auto thin_edge = measure(false, false, true);
    std::cout << "ReSTIR SVGF rotation history=" << rotation.x << ',' << rotation.y
              << " translation=" << translation.x << ',' << translation.y
              << " disocclusion=" << disocclusion.x << ',' << disocclusion.y
              << " moving_thin_edge=" << thin_edge.x << ',' << thin_edge.y << '\n';
    if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) || rotation.x < 14.f || rotation.y < 14.f)
        throw std::runtime_error("rotating at a fixed eye must retain both ReSTIR illumination histories");
    if (!std::isfinite(translation.x) || !std::isfinite(translation.y) ||
        translation.x < 1.5f || translation.x > 12.f || translation.y < 1.5f || translation.y > 12.f)
        throw std::runtime_error("ReSTIR mixed lighting channels must still respond to view parallax");
    if (disocclusion.x != 1.f || disocclusion.y != 1.f)
        throw std::runtime_error("ReSTIR disocclusion must reject both histories");
    if (!std::isfinite(thin_edge.x) || !std::isfinite(thin_edge.y) || thin_edge.x < 8.f || thin_edge.y < 8.f)
        throw std::runtime_error("moving thin surfaces must recover consistent nearby history when bilinear taps miss");
    std::cout << "PASS: ReSTIR SVGF preserves rotation history and rejects parallax/disocclusion\n";

    // Raw histories live at irregular film samples. Use projected GPU hit
    // positions as an independent CPU interpolation oracle; unlike a regular
    // bilinear grid, normalized irregular tent weights need not reproduce a
    // linear signal exactly. They must still preserve a constant signal.
    sensor->set_position(make_float3(0.f, 0.f, 3.f));
    sensor->set_yaw(0.f);
    sensor->update_device_data();
    pipeline.invalidate();
    for (uint i = 0; i < 2u; ++i) {
        pipeline.upload_data();
        pipeline.display(1.0 / 60.0);
    }
    RealTimeDenoiseInput input;
    input.frame_index = pipeline.frame_index() - 1u;
    input.resolution = pipeline.resolution();
    input.channel_kind = RealTimeDenoiseInput::ChannelKind::DirectIndirect;
    input.direct = direct.view(); input.indirect = indirect.view();
    input.visibility = pipeline.frame_buffer()->cur_visibility_buffer_view(input.frame_index);
    input.prev_visibility = pipeline.frame_buffer()->prev_visibility_buffer_view(input.frame_index);
    input.motion_vec = pipeline.frame_buffer()->motion_vectors();
    input.camera_pos = input.prev_camera_pos = {0.f, 0.f, 3.f};
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    Kernel linear_signal = [&](BufferVar<TriangleHit> visible, BufferVar<float4> output) {
        sensor->load_data();
        auto hit = visible.read(dispatch_id());
        Float value = 0.f;
        Float2 film = make_float2(0.f);
        $if(hit->is_hit()) {
            auto it = pipeline.geometry().compute_surface_interaction(hit, false);
            value = 2.f + 0.1f * it.pos.x + 0.07f * it.pos.y;
            film = sensor->raster_coord(it.pos).xy();
        };
        output.write(dispatch_id(), make_float4(value, film, ocarina::select(hit->is_hit(), 1.f, 0.f)));
    };
    auto signal_shader = pipeline.device().compile(linear_signal, "svgf_jitter_linear_signal");
    auto signal_output = pipeline.device().create_buffer<float4>(count, "svgf_jitter_signal");
    std::vector<float4> previous_signal(count), current_signal(count);
    pipeline.stream() << signal_shader(input.prev_visibility, signal_output).dispatch(input.resolution)
        << signal_output.download(previous_signal.data()) << synchronize() << commit();
    pipeline.stream() << signal_shader(input.visibility, signal_output).dispatch(input.resolution)
        << signal_output.download(current_signal.data()) << synchronize() << commit();
    auto &previous_history = (input.frame_index & 1u) ? denoiser->svgf_data : denoiser->svgf_data2;
    auto &current_history = (input.frame_index & 1u) ? denoiser->svgf_data2 : denoiser->svgf_data;
    for (bool constant : {false, true}) {
        float weighted = 0.f, weights = 0.f;
        for (uint i = 0; i < count; ++i) {
            const float value = constant ? 3.25f : previous_signal[i].x;
            history[i].illumi_direct = history[i].illumi_indirect = RadType4{value, value, value, 0.f};
            history[i].moments_direct = RadType4{value, value * value, 16.f, 16.f};
            history[i].moments_indirect = RadType4{value, value * value, 1.f, 1.f};
            signal[i] = RadType4{0.f, 0.f, 0.f, 1.f};
            const float wx = std::max(0.f, 1.f - std::abs(previous_signal[i].y - current_signal[center].y));
            const float wy = std::max(0.f, 1.f - std::abs(previous_signal[i].z - current_signal[center].z));
            const float weight = wx * wy * previous_signal[i].w;
            // Match the production tap acceptance, not its coordinate/RNG code.
            if (weight > 0.001f) { weighted += value * weight; weights += weight; }
        }
        if (weights <= 0.01f) throw std::runtime_error("irregular history fixture has no supported samples");
        pipeline.stream() << previous_history.view().upload(history.data())
            << direct.upload(signal.data()) << indirect.upload(signal.data())
            << denoiser->dispatch(input) << current_history.view().download(history.data())
            << synchronize() << commit();
        const float expected = (constant ? 3.25f : weighted / weights) * (16.f / 17.f);
        const float actual = float(history[center].illumi_direct.x);
        std::cout << "Irregular planar history constant=" << constant << " expected=" << expected << " actual=" << actual << '\n';
        if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-4f)
            throw std::runtime_error("SVGF must interpolate at actual film positions and preserve constants");
    }
    std::cout << "PASS: SVGF reconstructs irregular surface history and preserves constants\n";
}

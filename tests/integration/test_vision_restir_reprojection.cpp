// Compile and execute the real ReSTIR validation helpers without tracing rays.
// This translation unit is separate from the host-side engine math headers.
#define VISION_PLUGIN_NAME "ReSTIR"
#define VISION_CATEGORY "integrator"
#include "render_core/integrator/ReSTIR/direct.h"
#include "render_core/integrator/ReSTIR/indirect.h"
#include "base/sensor/sensor.h"
#include "render_core/denoiser/SVGF/svgf.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

void check_restir_reprojection(vision::Pipeline& pipeline) {
    using namespace vision;
    pipeline.activate_global_context();
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};

    ReservoirHistory history;
    struct HistoryCase { uint frame, jitter; bool valid; };
    const HistoryCase history_cases[]{
        {0,0,false}, {1,0,true}, {2,1,false}, {3,1,true},
        {4,0,false}, {5,0,true}, {0,0,false}, {1,0,true},
    };
    for (const auto& test : history_cases) {
        if (history.begin_frame(test.frame, test.jitter) != test.valid) {
            throw std::runtime_error("ReSTIR must reject history for the first frame after a jitter mode change or reset");
        }
    }

    auto surface = [](float z, float depth, float diffuse = 1.f,
                      float normal_z = 1.f, bool replaced = false) {
        SurfaceData result;
        // The validation helpers only inspect hit validity, not scene geometry.
        result.hit.inst_id = 0u;
        result.hit.prim_id = 0u;
        result.normal_depth = make_float4(0.f, 0.f, normal_z, depth);
        result.pos_diff = make_float4(0.f, 0.f, z, diffuse);
        result.is_replaced = replaced;
        return result;
    };
    struct Case {
        const char* name;
        SurfaceData current;
        SurfaceData previous;
        uint temporal_expected;
        uint spatial_expected;
    };
    // The old camera is at z=0, the current camera at z=1, both looking +Z.
    // Temporal inputs store depth in their own camera spaces. Spatial checks
    // intentionally continue to compare the supplied depths, not world poses.
    const std::vector<Case> cases{
        {"dolly_same_world_point", surface(2.f, 1.f), surface(2.f, 2.f), 1u, 0u},
        {"equal_stored_depth_different_surface", surface(2.f, 1.f), surface(1.f, 1.f), 0u, 1u},
        {"opposite_normals", surface(2.f, 1.f), surface(2.f, 1.f, 1.f, -1.f), 0u, 0u},
        {"two_zero_diffuse_factors", surface(2.f, 1.f, 0.f), surface(2.f, 1.f, 0.f), 1u, 1u},
        {"current_replaced", surface(2.f, 1.f, 1.f, 1.f, true), surface(2.f, 1.f), 0u, 0u},
        {"previous_replaced", surface(2.f, 1.f), surface(2.f, 1.f, 1.f, 1.f, true), 0u, 0u},
        {"both_replaced", surface(2.f, 1.f, 1.f, 1.f, true), surface(2.f, 1.f, 1.f, 1.f, true), 0u, 0u},
        {"ordinary_matching_surface", surface(2.f, 1.f), surface(2.f, 1.f), 1u, 1u},
        {"negative_depth_separation", surface(0.f, -1.f), surface(-3.f, -4.f), 0u, 0u},
    };
    auto current_camera = make_float4x4(1.f);
    current_camera[3][2] = 1.f;
    auto& camera = pipeline.scene().sensor();
    DIParam di{};
    GIParam gi{};
    auto set_thresholds = [](auto& param) {
        param.max_age = 30u;
        param.diff_factor = 0.3f;
        param.t_dot = 0.8f;
        param.t_depth = 0.3f;
        param.s_dot = 0.8f;
        param.s_depth = 0.3f;
    };
    set_thresholds(di);
    set_thresholds(gi);
    // OC_PARAM_STRUCT values must enter the DSL as kernel arguments.
    Kernel kernel = [&](BufferVar<uint> output, BufferVar<SurfaceData> surfaces,
                        Var<DIParam> di_param, Var<GIParam> gi_param) {
        camera->load_data();
        // This changes the kernel-local decoded camera only, not host state.
        camera->set_device_c2w(Float4x4{current_camera});
        for (uint index = 0; index < cases.size(); ++index) {
            SurfaceDataVar current = surfaces.read(index * 2u);
            SurfaceDataVar previous = surfaces.read(index * 2u + 1u);
            output.write(index * 4u, cast<uint>(ReSTIRDI::is_temporal_valid(current, previous, di_param, nullptr)));
            output.write(index * 4u + 1u, cast<uint>(ReSTIRGI::is_temporal_valid(current, previous, gi_param, nullptr)));
            output.write(index * 4u + 2u, cast<uint>(ReSTIRDI::is_valid_neighbor(current, previous, di_param)));
            output.write(index * 4u + 3u, cast<uint>(ReSTIRGI::is_valid_neighbor(current, previous, gi_param)));
        }
    };
    auto shader = pipeline.device().compile(kernel, "restir_surface_reprojection_regression");
    std::vector<uint> results(cases.size() * 4u);
    auto output = pipeline.device().create_buffer<uint>(results.size(), "restir_reprojection_results");
    std::vector<SurfaceData> surface_data;
    for (const auto& test : cases) {
        surface_data.push_back(test.current);
        surface_data.push_back(test.previous);
    }
    auto surfaces = pipeline.device().create_buffer<SurfaceData>(surface_data.size(), "restir_reprojection_surfaces");
    pipeline.stream() << surfaces.upload(surface_data.data())
                      << shader(output, surfaces, di, gi).dispatch(1u) << output.download(results.data())
                      << synchronize() << commit();

    const char* checks[]{"DI temporal", "GI temporal", "DI spatial", "GI spatial"};
    unsigned failures = 0;
    for (size_t index = 0; index < cases.size(); ++index) {
        const auto& test = cases[index];
        for (unsigned check = 0; check < 4; ++check) {
            const auto expected = check < 2 ? test.temporal_expected : test.spatial_expected;
            const auto actual = results[index * 4u + check];
            if (actual != expected) {
                ++failures;
                std::cerr << "FAIL: ReSTIR " << checks[check] << ' ' << test.name
                          << " expected=" << expected << " actual=" << actual << '\n';
            }
        }
    }
    if (failures) {
        throw std::runtime_error(std::to_string(failures) + " ReSTIR surface reprojection checks failed");
    }
    std::cout << "PASS: ReSTIR validates temporal and spatial receiver guides ("
              << cases.size() * 4u << " checks)\n";

    struct PixelCase {
        const char* name;
        float2 film, motion;
        int2 expected;
        int valid;
    };
    // Independent raster examples, including subpixel excursions off all sides.
    const std::vector<PixelCase> pixel_cases{
        {"inside_cell", {10.9f,20.1f}, {0,0}, {10,20}, 1},
        {"crossed_cell", {11.1f,19.8f}, {0,0}, {11,19}, 1},
        {"camera_motion", {10.9f,20.1f}, {2,-3}, {8,23}, 1},
        {"raw_pixel_centre", {10.5f,20.5f}, {0,0}, {10,20}, 1},
        {"left_offscreen", {.5f,10.5f}, {.7f,0}, {-1,10}, 0},
        {"top_offscreen", {10.5f,.5f}, {0,.7f}, {10,-1}, 0},
        {"right_offscreen", {31.5f,10.5f}, {-.7f,0}, {32,10}, 0},
        {"bottom_offscreen", {10.5f,31.5f}, {0,-.7f}, {10,32}, 0},
        {"jittered_first_pixel", {-.2f,10.5f}, {0,0}, {-1,10}, 0},
        {"fallback_negative_pixel", {-2.2f,10.5f}, {0,0}, {-3,10}, 0},
    };
    Kernel pixel_kernel = [&](BufferVar<int> output) {
        for (uint i = 0; i < pixel_cases.size(); ++i) {
            const auto& test = pixel_cases[i];
            Int2 pixel = reservoir_pixel(previous_reservoir_coord(
                Float2{test.film}, Float2{test.motion}));
            output.write(i * 3u, pixel.x);
            output.write(i * 3u + 1u, pixel.y);
            output.write(i * 3u + 2u, cast<int>(in_screen(pixel, make_int2(32))));
        }
    };
    auto pixel_shader = pipeline.device().compile(pixel_kernel, "restir_jittered_grid_regression");
    std::vector<int> pixel_results(pixel_cases.size() * 3u);
    auto pixel_output = pipeline.device().create_buffer<int>(pixel_results.size(), "restir_pixel_results");
    pipeline.stream() << pixel_shader(pixel_output).dispatch(1u) << pixel_output.download(pixel_results.data())
                      << synchronize() << commit();
    for (size_t i = 0; i < pixel_cases.size(); ++i) {
        const auto& test = pixel_cases[i];
        if (pixel_results[i * 3u] != test.expected.x || pixel_results[i * 3u + 1u] != test.expected.y ||
            pixel_results[i * 3u + 2u] != test.valid) {
            throw std::runtime_error(std::string("ReSTIR previous reservoir coordinate failed: ") + test.name);
        }
    }
    std::cout << "PASS: ReSTIR maps jittered history and rejects offscreen reservoir pixels ("
              << pixel_cases.size() << " cases)\n";
}

void check_restir_material_reuse(vision::Pipeline& pipeline, const std::vector<vision::SurfaceData>& surfaces) {
    using namespace vision;
    pipeline.activate_global_context();
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    struct Case { const char* name; uint a, b, expected; };
    const Case cases[]{
        {"same_diffuse", 0, 0, 1}, {"different_color", 0, 1, 0},
        {"similar_color", 0, 2, 1}, {"different_reflectivity", 3, 4, 0},
        {"different_plastic_roughness", 5, 6, 0}, {"same_black", 7, 7, 1},
        {"same_glossy", 3, 3, 1}, {"glass_reflectivity", 8, 9, 0},
        {"same_normal_mapped_material_different_view", 14, 15, 1},
        {"same_layered_material_different_view", 10, 11, 1}, {"same_hero_material_different_frame", 12, 13, 1},
    };
    DIParam di{}; GIParam gi{};
    auto thresholds = [](auto& p) {
        p.max_age = 30u; p.diff_factor = 0.3f;
        p.s_dot = p.t_dot = 0.8f; p.s_depth = p.t_depth = 0.1f;
    };
    thresholds(di); thresholds(gi);
    auto& camera = pipeline.scene().sensor();
    Kernel kernel = [&](BufferVar<SurfaceData> data, BufferVar<uint> out, Var<DIParam> dp, Var<GIParam> gp) {
        camera->load_data();
        for (uint i = 0; i < std::size(cases); ++i) {
            for (uint reverse = 0; reverse < 2u; ++reverse) {
                auto a = data.read(reverse ? cases[i].b : cases[i].a);
                auto b = data.read(reverse ? cases[i].a : cases[i].b);
                const uint base = i * 8u + reverse * 4u;
                out.write(base, cast<uint>(ReSTIRDI::is_valid_neighbor(a, b, dp)));
                out.write(base + 1u, cast<uint>(ReSTIRGI::is_valid_neighbor(a, b, gp)));
                out.write(base + 2u, cast<uint>(ReSTIRDI::is_temporal_valid(a, b, dp, nullptr)));
                out.write(base + 3u, cast<uint>(ReSTIRGI::is_temporal_valid(a, b, gp, nullptr)));
            }
        }
    };
    auto shader = pipeline.device().compile(kernel, "restir_material_reuse_regression");
    auto data = pipeline.device().create_buffer<SurfaceData>(surfaces.size(), "material_surfaces");
    std::vector<uint> results(std::size(cases) * 8u);
    auto output = pipeline.device().create_buffer<uint>(results.size(), "material_reuse_results");
    auto material_only = surfaces;
    for (auto& surface : material_only) {
        // Isolate material acceptance from the deliberately changed camera pose.
        surface.normal_depth = surfaces.back().normal_depth;
        surface.pos_diff.x = surfaces.back().pos_diff.x;
        surface.pos_diff.y = surfaces.back().pos_diff.y;
        surface.pos_diff.z = surfaces.back().pos_diff.z;
    }
    pipeline.stream() << data.upload(material_only.data()) << shader(data, output, di, gi).dispatch(1u)
        << output.download(results.data()) << synchronize() << commit();
    uint failures = 0;
    for (uint i = 0; i < std::size(cases); ++i) {
        for (uint j = 0; j < 8u; ++j) {
            if (results[i * 8u + j] != cases[i].expected) {
                ++failures;
                std::cerr << "FAIL: material reuse " << cases[i].name << " check=" << j
                          << " expected=" << cases[i].expected << " actual=" << results[i * 8u + j] << '\n';
            }
        }
    }
    if (failures) throw std::runtime_error("ReSTIR material boundary checks failed: " + std::to_string(failures));
    std::cout << "PASS: real material guides preserve similar surfaces and reject different colors, reflectivity and roughness\n";
}

// Exercise producer, ReSTIR validation, virtual reprojection and SVGF history
// together on the real mirror scene prepared by the host-side test.
void check_glass_svgf(vision::Pipeline& pipeline) {
    using namespace vision;
    pipeline.activate_global_context();
    auto* illumination = dynamic_cast<IlluminationIntegrator*>(pipeline.renderer().integrator().get());
    auto* denoiser = illumination ? dynamic_cast<svgf::SVGF*>(illumination->denoiser()) : nullptr;
    if (!denoiser || !denoiser->enabled()) throw std::runtime_error("glass SVGF fixture requires enabled denoiser");
    const uint n = pipeline.pixel_num(), center = 136u;
    std::vector<svgf::SVGFDataDual> history(n);
    std::vector<float4> stable(n), final(n), direct(n), indirect(n);
    auto render = [&] { pipeline.upload_data(); pipeline.display(1.0 / 60.0); };
    auto age = [&](uint layer) {
        pipeline.stream() << denoiser->svgf_buffer_cur(pipeline.frame_index() - 1u, layer).download(history.data())
            << synchronize() << commit();
        return float(history[center].moments_direct.z);
    };
    auto sum = [&] {
        auto& fb = *pipeline.frame_buffer();
        pipeline.stream() << fb.stable_radiance().download(stable.data()) << fb.rt_buffer().view().download(final.data())
            << synchronize() << commit();
        for (uint layer = 0u; layer < StablePlaneCount; ++layer) {
            pipeline.stream() << fb.stable_direct_view(layer).download(direct.data())
                << fb.stable_indirect_view(layer).download(indirect.data()) << synchronize() << commit();
            for (uint i = 0u; i < n; ++i) for (uint c = 0; c < 3; ++c) stable[i][c] += direct[i][c] + indirect[i][c];
        }
        for (uint i = 0u; i < n; ++i) for (uint c = 0; c < 3; ++c)
            if (!std::isfinite(final[i][c]) || (!denoiser->enabled() && std::abs(stable[i][c] - final[i][c]) > 1e-5f))
                throw std::runtime_error("raw composition must sum each owned layer exactly once; composed coverage must remain finite");
    };
    render();
    if (age(1u) != 1.f || age(2u) != 1.f)
        throw std::runtime_error("new camera pipeline must start independent reflection/transmission histories");
    for (uint frame = 0; frame < 24; ++frame) render();
    if (age(1u) < 2.f || age(2u) < 2.f)
        throw std::runtime_error("real glass reflection and transmission must both accumulate SVGF history");
    for (uint layer : {1u, 2u}) {
        pipeline.stream() << pipeline.frame_buffer()->stable_direct_view(layer).download(direct.data())
            << synchronize() << commit();
        double red = 0, blue = 0;
        for (const auto& pixel : direct) { red += pixel.x; blue += pixel.z; }
        if ((layer == 1u && red <= 2 * blue) || (layer == 2u && blue <= 2 * red))
            throw std::runtime_error("filtered glass layers must preserve independent red transmission and blue reflection");
    }
    sum();
    auto& camera = pipeline.scene().sensor();
    camera->set_position(make_float3(0.1f, 0.f, 0.f)); camera->update_device_data();
    render();
    if (age(1u) <= 1.f || age(2u) <= 1.f)
        throw std::runtime_error("translated glass must retain compatible independent layer histories");
    sum();
    pipeline.set_output_denoise(false); render(); sum();
    pipeline.set_output_denoise(true); render(); sum();
    const float resumed_t = age(1u), resumed_r = age(2u);
    std::cout << "glass reenabled frame=" << pipeline.frame_index() << " enabled=" << denoiser->enabled()
        << " history=" << resumed_t << ',' << resumed_r << '\n';
    if (resumed_t != 1.f || resumed_r != 1.f)
        throw std::runtime_error("reenabled glass denoiser must cold-start all layer histories");
    for (bool enabled : {false, true, false, true}) {
        illumination->set_stable_planes_enabled(enabled); render();
        if (enabled && (age(1u) != 1.f || age(2u) != 1.f))
            throw std::runtime_error("glass stable-plane toggle must reset both child histories");
        if (enabled) sum();
    }
    std::cout << "PASS: real glass SVGF layer colors/history/motion, exact raw+filtered composition, toggles and camera isolation\n";
}

void check_stable_plane(vision::Pipeline& pipeline, bool tinted_mirror) {
    using namespace vision;
    pipeline.activate_global_context();
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    auto fail = [](bool okay, const char* message) { if (!okay) throw std::runtime_error(message); };
    auto* illumination = dynamic_cast<IlluminationIntegrator*>(pipeline.renderer().integrator().get());
    auto* denoiser = illumination ? dynamic_cast<svgf::SVGF*>(illumination->denoiser()) : nullptr;
    fail(denoiser && denoiser->enabled(), "stable plane fixture must run the actual SVGF pipeline");
    std::vector<SurfaceData> surfaces(pipeline.pixel_num());
    std::vector<TriangleHit> primary(pipeline.pixel_num());
    std::vector<svgf::SVGFDataDual> history(pipeline.pixel_num());
    const uint center = 8u * 16u + 8u;
    auto read = [&] {
        const uint frame = pipeline.frame_index() - 1u;
        pipeline.stream() << pipeline.frame_buffer()->cur_surfaces_view(frame).download(surfaces.data())
            << pipeline.frame_buffer()->cur_visibility_buffer_view(frame).download(primary.data())
            << ((frame & 1u) == 0u ? denoiser->svgf_data : denoiser->svgf_data2).view().download(history.data()) << synchronize() << commit();
    };
    for (int i = 0; i < 6; ++i) { pipeline.upload_data(); pipeline.display(1.0 / 60.0); }
    read();
    const auto a = surfaces[center];
    fail(a.stable_branch != 0u && a.stable_branch != InvalidUI32, "mirror must produce a valid stable branch");
    fail(a.hit.inst_id != primary[center].inst_id, "stable endpoint must not overwrite primary visibility");
    fail(std::abs(a.virtual_position.z + 3.f) < 1e-4f && a.virtual_geometric_normal.z > 0.999f,
         "mirror virtual position and normal must be reflected into camera space");
    if (tinted_mirror) fail(std::abs(a.denoiser_albedo.x - 0.48f) < 0.01f && std::abs(a.denoiser_albedo.y - 0.16f) < 0.01f,
         "mirror demodulation guide must contain receiver albedo times prefix throughput");
    fail(float(history[center].moments_direct.z) > 2.f, "stable mirror must accumulate SVGF history");
    auto& camera = pipeline.scene().sensor();
    camera->set_position(make_float3(0.15f, 0.f, 0.f));
    camera->update_device_data();
    pipeline.upload_data(); pipeline.display(1.0 / 60.0);
    read();
    fail(float(history[center].moments_direct.z) > 1.5f, "camera translation must preserve matching reflected SVGF history");
    std::cout << "stable mirror moving_history=" << float(history[center].moments_direct.z) << '\n';

    DIParam di{}; GIParam gi{};
    auto thresholds = [](auto& p) { p.max_age=30u; p.diff_factor=0.3f; p.s_dot=p.t_dot=0.8f; p.s_depth=p.t_depth=0.1f; };
    thresholds(di); thresholds(gi);
    Kernel kernel = [&](BufferVar<SurfaceData> data, BufferVar<float4> output, Var<DIParam> dp, Var<GIParam> gp) {
        camera->load_data();
        auto current = data.read(center);
        auto other = current;
        output.write(0u, make_float4(cast<float>(ReSTIRDI::is_valid_neighbor(current, other, dp)),
            cast<float>(ReSTIRGI::is_valid_neighbor(current, other, gp)),
            cast<float>(ReSTIRDI::is_temporal_valid(current, other, dp, nullptr)),
            cast<float>(ReSTIRGI::is_temporal_valid(current, other, gp, nullptr))));
        other.stable_branch = current.stable_branch ^ 1u;
        output.write(1u, make_float4(cast<float>(ReSTIRDI::is_valid_neighbor(current, other, dp))));
        other.stable_branch = InvalidUI32;
        output.write(2u, make_float4(cast<float>(ReSTIRGI::is_valid_neighbor(current, other, gp))));
        // Analytical virtual point projection and the actual produced motion
        // must agree; using the primary mirror depth would differ by 3x.
        Float2 expected_prev = camera->prev_raster_coord(current.virtual_position).xy();
        Float2 expected_cur = camera->raster_coord(current.virtual_position).xy();
        Float2 actual = pipeline.frame_buffer()->motion_vectors().read(center);
        output.write(3u, make_float4(actual, expected_cur - expected_prev));
    };
    auto shader = pipeline.device().compile(kernel, "stable_plane_reuse_and_motion");
    auto result = pipeline.device().create_buffer<float4>(4u, "stable_plane_checks");
    float4 values[4]{};
    pipeline.stream() << shader(pipeline.frame_buffer()->cur_surfaces_view(pipeline.frame_index()-1u), result, di, gi).dispatch(1u)
        << result.download(values) << synchronize() << commit();
    fail(values[0].x == 1.f && values[0].y == 1.f && values[0].z == 1.f && values[0].w == 1.f,
         "matching stable endpoints must allow DI/GI temporal and spatial reuse");
    fail(values[1].x == 0.f && values[2].x == 0.f, "different or unsupported reflection branches must be rejected");
    fail(std::abs(values[3].x-values[3].z) < 1e-3f && std::abs(values[3].y-values[3].w) < 1e-3f,
         "reflection motion must project the virtual image rather than the mirror");
    // Toggle the actual retained renderer without rebuilding it. Both guide
    // generation and SVGF history must switch on the very next frame.
    auto& integrator = pipeline.renderer().integrator();
    for (bool enabled : {false, true, false, true}) {
        integrator->set_stable_planes_enabled(enabled);
        fail(integrator->stable_planes_enabled() == enabled && pipeline.frame_index() == 0u,
             "stable plane toggle must reset the active renderer history");
        pipeline.upload_data(); pipeline.display(1.0 / 60.0);
        read();
        const auto& surface = surfaces[center];
        fail(surface.is_replaced && (enabled ? surface.stable_branch != 0u && surface.stable_branch != InvalidUI32
                                            : surface.stable_branch == InvalidUI32),
             "runtime toggle must select the requested stable branch path");
        fail(std::abs(surface.pos_diff.z - (enabled ? 1.f : -1.f)) < 1e-4f &&
             std::abs(surface.normal_depth.w - (enabled ? 3.f : 1.f)) < 1e-4f,
             "runtime toggle must select virtual endpoint or primary mirror guides");
        fail(float(history[center].moments_direct.z) <= 1.f,
             "stable plane toggle must not inherit previous SVGF history");
        const auto frame_before_noop = pipeline.frame_index();
        integrator->set_stable_planes_enabled(enabled);
        fail(pipeline.frame_index() == frame_before_noop,
             "reapplying the same stable plane preference must preserve history");
        for (int i = 0; i < 3; ++i) { pipeline.upload_data(); pipeline.display(1.0 / 60.0); }
    }
    std::cout << "PASS: stable plane runtime on/off toggles and history reset\n";
    std::cout << "PASS: stable plane endpoint, branch rejection, virtual motion and SVGF history\n";
}

// Deterministic mirror emission has no eligible reservoir endpoint. Coverage
// must consume the stable plane's geometry/motion even when legacy exports clear.
void check_stable_emitter_coverage(vision::Pipeline& pipeline) {
    using namespace vision;
    pipeline.activate_global_context();
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    auto& fb = *pipeline.frame_buffer();
    auto& camera = pipeline.scene().sensor();
    auto* illumination = dynamic_cast<IlluminationIntegrator*>(pipeline.renderer().integrator().get());
    auto* denoiser = dynamic_cast<svgf::SVGF*>(illumination->denoiser());
    const uint n = pipeline.pixel_num(), center = 136u;
    auto result = pipeline.device().create_buffer<float4>(n, "mirror_emitter_coverage_checks");
    Kernel kernel = [&](Var<svgf::ResolveParam> param, BufferVar<float4> out) {
        camera->load_data();
        Uint idx = dispatch_id();
        svgf::CoverageGeometryGuide guide(param, idx, param.visibility.read(idx));
        svgf::CoverageGeometryGuide previous(param, idx, param.prev_visibility.read(idx), true);
        auto plane = param.coverage_planes.read(idx);
        auto old = param.prev_coverage_planes.read(idx);
        auto motion_of = [&]<typename Guide>(const Guide& value) {
            if constexpr (requires { value.motion; }) return value.motion;
            else return svgf::stable_motion(param, idx);
        };
        Float2 expected = camera->raster_coord(plane.surface.virtual_position).xy() -
                          camera->prev_raster_coord(plane.surface.virtual_position).xy();
        Bool current_ok = guide.valid && !guide.ambiguous && guide.replaced &&
            guide.hit.inst_id == plane.surface.hit.inst_id &&
            length(guide.position - plane.surface.virtual_position) < 1e-5f &&
            length(guide.normal - plane.surface.virtual_geometric_normal) < 1e-5f;
        Bool previous_ok = previous.valid && !previous.ambiguous && previous.replaced &&
            previous.hit.inst_id == old.surface.hit.inst_id &&
            length(previous.position - old.surface.virtual_position) < 1e-5f &&
            length(previous.normal - old.surface.virtual_geometric_normal) < 1e-5f;
        out.write(idx, make_float4(cast<float>(current_ok), cast<float>(previous_ok),
            length(motion_of(guide) - plane.motion), length(motion_of(guide) - expected)));
    };
    auto shader = pipeline.device().compile(kernel, "mirror_emitter_composed_guide");
    std::vector<float4> values(n);
    std::vector<StablePlaneData> current(n * StablePlaneCount), previous(current.size());
    std::vector<SurfaceData> exported(n);
    std::vector<uint> dominant(n);
    bool guides_ok = true;
    for (uint pose = 0u; pose < 3u; ++pose) {
        if (pose == 1u) camera->set_position(make_float3(0.03f, 0.f, 0.f));
        if (pose == 2u) camera->set_yaw(1.f);
        camera->update_device_data();
        pipeline.upload_data(); pipeline.display(1.0 / 60.0);
        if (pose == 0u) { pipeline.upload_data(); pipeline.display(1.0 / 60.0); }
        uint frame = pipeline.frame_index() - 1u;
        svgf::ResolveParam param;
        param.composed_coverage = param.use_stable_planes = 1u;
        param.coverage_planes = fb.cur_stable_planes_view(frame).descriptor();
        param.prev_coverage_planes = fb.prev_stable_planes_view(frame).descriptor();
        param.stable_surfaces = fb.cur_surfaces_view(frame).descriptor();
        param.prev_stable_surfaces = fb.prev_surfaces_view(frame).descriptor();
        param.visibility = fb.cur_visibility_buffer_view(frame).descriptor();
        param.prev_visibility = fb.prev_visibility_buffer_view(frame).descriptor();
        param.motion_vectors = fb.motion_vectors().descriptor();
        pipeline.stream() << shader(param, result).dispatch(pipeline.resolution())
            << result.download(values.data()) << fb.cur_stable_planes_view(frame).download(current.data())
            << fb.prev_stable_planes_view(frame).download(previous.data())
            << fb.cur_surfaces_view(frame).download(exported.data()) << fb.stable_dominant().download(dominant.data())
            << synchronize() << commit();
        auto p = current[center];
        if (!p.valid || p.reservoir_eligible || dominant[center] != InvalidUI32 || exported[center].is_replaced ||
            std::abs(p.surface.virtual_position.z + 3.f) > 1e-4f || p.surface.hit.inst_id == InvalidUI32)
            throw std::runtime_error("mirror emitter fixture must have a valid exact virtual endpoint and no reservoir export");
        auto v = values[center];
        std::cout << "mirror emitter pose=" << pose << " current/previous=" << v.x << '/' << v.y
                  << " motion plane/analytic error=" << v.z << '/' << v.w << '\n';
        guides_ok &= v.x == 1.f && v.y == 1.f && std::isfinite(v.z) && std::isfinite(v.w) && v.z < 1e-4f && v.w < 1e-3f;
    }
    // Real produced endpoint guides feed the actual coverage shader. Two runs
    // differ only in legacy reservoir exports and primary motion; their complete
    // pixel history must agree and must differ from cold current reconstruction.
    auto planes = pipeline.device().create_buffer<StablePlaneData>(current.size(), "emitter_coverage_planes");
    auto old_planes = pipeline.device().create_buffer<StablePlaneData>(previous.size(), "emitter_coverage_old_planes");
    auto surfaces = pipeline.device().create_buffer<SurfaceData>(n, "emitter_coverage_export");
    auto old_surfaces = pipeline.device().create_buffer<SurfaceData>(n, "emitter_coverage_old_export");
    auto motions = pipeline.device().create_buffer<float2>(n, "emitter_coverage_primary_motion");
    auto direct = pipeline.device().create_buffer<RadType4>(n, "emitter_coverage_signal");
    auto indirect = pipeline.device().create_buffer<RadType4>(n, "emitter_coverage_indirect");
    std::vector<SurfaceData> virtual_surfaces(n), old_virtual_surfaces(n), empty(n);
    std::vector<float2> virtual_motion(n), primary_motion(n, make_float2(1000.f));
    std::vector<RadType4> signal(n), black(n, RadType4{0,0,0,1}), actual(n), reference(n), cold(n);
    for (uint i = 0u; i < n; ++i) {
        current[i].branch_sequence = previous[i].branch_sequence = 5u + (i & 1u);
        virtual_surfaces[i] = current[i].surface; old_virtual_surfaces[i] = previous[i].surface;
        virtual_motion[i] = current[i].motion;
    }
    const uint frame = pipeline.frame_index() - 1u;
    RealTimeDenoiseInput input;
    input.composed_coverage = input.use_stable_planes = true;
    input.channel_kind = RealTimeDenoiseInput::ChannelKind::DirectIndirect;
    input.resolution = pipeline.resolution();
    input.direct = direct.view(); input.indirect = indirect.view();
    input.stable_surfaces = surfaces.view(); input.prev_stable_surfaces = old_surfaces.view();
    input.coverage_planes = planes.view(); input.prev_coverage_planes = old_planes.view();
    input.visibility = fb.cur_visibility_buffer_view(frame); input.prev_visibility = fb.prev_visibility_buffer_view(frame);
    input.motion_vec = motions.view();
    input.camera_pos = input.prev_camera_pos = {0.03f, 0.f, 0.f};
    auto render = [&](bool legacy_virtual, bool history, auto& output) {
        denoiser->set_enabled(false); denoiser->set_enabled(true);
        pipeline.stream() << surfaces.upload(legacy_virtual ? virtual_surfaces.data() : empty.data())
            << old_surfaces.upload(legacy_virtual ? old_virtual_surfaces.data() : empty.data())
            << motions.upload(legacy_virtual ? virtual_motion.data() : primary_motion.data())
            << planes.upload(current.data()) << old_planes.upload(previous.data());
        if (history) {
            camera->set_yaw(0.f); camera->update_device_data();
            input.frame_index = 0u;
            std::fill(signal.begin(), signal.end(), RadType4{0.25f,0.25f,0.25f,1.f});
            pipeline.stream() << direct.upload(signal.data()) << indirect.upload(black.data()) << denoiser->dispatch(input)
                << synchronize() << commit();
        }
        camera->set_yaw(1.f); camera->update_device_data(); input.frame_index = 1u;
        for (uint i = 0u; i < n; ++i) { float v = float(i & 1u); signal[i] = RadType4{v,v,v,1.f}; }
        pipeline.stream() << direct.upload(signal.data()) << indirect.upload(black.data()) << denoiser->dispatch(input)
            << direct.download(output.data()) << synchronize() << commit();
    };
    render(false, true, actual); render(true, true, reference); render(true, false, cold);
    float error = 0.f, history_effect = 0.f;
    for (uint i = 0u; i < n; ++i) {
        if (!std::isfinite(float(actual[i].x)) || !std::isfinite(float(reference[i].x)) || !std::isfinite(float(cold[i].x)))
            throw std::runtime_error("mirror emitter coverage output must stay finite");
        error = std::max(error, std::abs(float(actual[i].x) - float(reference[i].x)));
        history_effect = std::max(history_effect, std::abs(float(reference[i].x) - float(cold[i].x)));
    }
    std::cout << "mirror emitter rendered export independence=" << error << " history effect=" << history_effect << '\n';
    if (!guides_ok || error > 1e-5f || history_effect < 1e-3f)
        throw std::runtime_error("exact mirror emitter coverage must use current/previous virtual endpoint and motion without reservoir eligibility");
}

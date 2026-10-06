// Compile and execute the real ReSTIR validation helpers without tracing rays.
// This translation unit is separate from the host-side engine math headers.
#define VISION_PLUGIN_NAME "ReSTIR"
#define VISION_CATEGORY "integrator"
#include "render_core/integrator/ReSTIR/direct.h"
#include "render_core/integrator/ReSTIR/indirect.h"
#include "base/sensor/sensor.h"

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
        float2 film, motion, previous_offset;
        int2 expected;
        int valid;
    };
    // Independent raster examples, including subpixel excursions off all sides.
    const std::vector<PixelCase> pixel_cases{
        {"same_jitter", {10.9f,20.1f}, {0,0}, {.4f,-.4f}, {10,20}, 1},
        {"opposite_jitter", {11.1f,19.8f}, {0,0}, {-.6f,.7f}, {11,19}, 1},
        {"camera_motion", {10.9f,20.1f}, {2,-3}, {.4f,-.4f}, {8,23}, 1},
        {"raw_pixel_centre", {10.5f,20.5f}, {0,0}, {0,0}, {10,20}, 1},
        {"left_offscreen", {.5f,10.5f}, {.7f,0}, {0,0}, {-1,10}, 0},
        {"top_offscreen", {10.5f,.5f}, {0,.7f}, {0,0}, {10,-1}, 0},
        {"right_offscreen", {31.5f,10.5f}, {-.7f,0}, {0,0}, {32,10}, 0},
        {"bottom_offscreen", {10.5f,31.5f}, {0,-.7f}, {0,0}, {10,32}, 0},
        {"jittered_first_pixel", {-.2f,10.5f}, {0,0}, {-.8f,0}, {0,10}, 1},
        {"fallback_negative_pixel", {-2.2f,10.5f}, {0,0}, {0,0}, {-3,10}, 0},
    };
    Kernel pixel_kernel = [&](BufferVar<int> output) {
        for (uint i = 0; i < pixel_cases.size(); ++i) {
            const auto& test = pixel_cases[i];
            Int2 pixel = reservoir_pixel(previous_reservoir_coord(
                Float2{test.film}, Float2{test.motion}, Float2{test.previous_offset}));
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

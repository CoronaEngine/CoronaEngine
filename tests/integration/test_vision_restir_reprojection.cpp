// Compile and execute the real ReSTIR validation helpers without tracing rays.
// This translation unit is separate from the host-side engine math headers.
#define VISION_PLUGIN_NAME "ReSTIR"
#define VISION_CATEGORY "integrator"
#include "render_core/integrator/ReSTIR/direct.h"
#include "render_core/integrator/ReSTIR/indirect.h"
#include "base/sensor/sensor.h"
#include "render_core/denoiser/SVGF/svgf.h"

#include <iostream>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
// Removing the snapshot, moving it after an extension writer, retaining a
// parity offset, truncating a float, or failing to rebind on resize must fail.
template<typename FrameBufferType>
void check_extension_history(vision::Pipeline& pipeline, FrameBufferType& frame_buffer) {
    using namespace vision;
    static_assert(sizeof(SurfaceExtend) == 44u);
    if constexpr (!requires { frame_buffer.save_surface_exts_history(1u); }) {
        throw std::runtime_error("ReSTIR must retain an ordered FP32 receiver-position history");
    } else {
        const uint2 original_resolution = pipeline.resolution();
        const auto original_position = pipeline.scene().sensor()->position();
        auto* integrator = pipeline.renderer().integrator().get();
        const bool original_planes = integrator->stable_planes_enabled();
        const uint patterns[]{0u, 0x80000000u, 1u, 0x80000001u, 0x3f800001u,
            0xbf800001u, 0x7f7fffffu, 0xff7fffffu, 0x7fc12345u, 0xffc54321u,
            0x7f800000u, 0xff800000u};
        constexpr uint current_words = sizeof(SurfaceExtend) / sizeof(uint);
        constexpr uint position_offset = offsetof(SurfaceExtend, view_pos) / sizeof(uint);
        Kernel read_history = [&](BufferVar<uint> output, Uint frame) {
            auto previous = frame_buffer.prev_surface_exts_var(frame).read(dispatch_id());
            for (uint component = 0u; component < 3u; ++component) {
                output.write(dispatch_id() * 3u + component, as<uint>(previous.view_pos[component]));
            }
        };
        auto read = pipeline.device().compile(read_history, "restir_extension_history_readback");
        for (const uint2 resolution : {make_uint2(9u, 7u), make_uint2(13u, 11u), original_resolution}) {
            pipeline.stream() << synchronize() << commit();
            pipeline.change_resolution(resolution);
            const uint pixels = frame_buffer.frame_buffer_size();
            auto current = frame_buffer.cur_surface_exts_view(0u);
            auto previous = frame_buffer.prev_surface_exts_view(0u);
            if (current.size_in_byte() != pixels * 44u || previous.size_in_byte() != pixels * 12u) {
                throw std::runtime_error("ReSTIR extension storage must keep full current records and 12-byte previous positions after resize");
            }
            std::vector<uint> history(pixels * 3u), expected(history.size());
            std::vector<uint> source(pixels * current_words), unchanged(source.size());
            std::vector<uint> poison(source.size(), 0xdeadbeefu);
            auto output = pipeline.device().create_buffer<uint>(history.size(), "restir_extension_history_results");
            pipeline.stream() << previous.template view_as<uint>().download(history.data())
                              << synchronize() << commit();
            if (history != std::vector<uint>(history.size(), 0u)) {
                throw std::runtime_error("ReSTIR compact extension history must reset on resize");
            }
            for (const uint frame : {1u, 2u, 3u, 4u, 7u, 8u}) {
                for (uint pixel = 0u; pixel < pixels; ++pixel) {
                    for (uint word = 0u; word < current_words; ++word) {
                        source[pixel * current_words + word] = 0x3f000000u + pixel * current_words + word;
                    }
                    for (uint component = 0u; component < 3u; ++component) {
                        const uint bits = patterns[(pixel * 3u + component + frame) % std::size(patterns)];
                        source[pixel * current_words + position_offset + component] = bits;
                        expected[pixel * 3u + component] = bits;
                    }
                }
                // Uploads, the real snapshot, overwrite and bindless reads share
                // one command batch: a delayed or aliased copy sees poison.
                pipeline.stream() << current.template view_as<uint>().upload(source.data())
                                  << frame_buffer.save_surface_exts_history(frame)
                                  << current.template view_as<uint>().download(unchanged.data())
                                  << current.template view_as<uint>().upload(poison.data())
                                  << read(output, frame).dispatch(pixels)
                                  << output.download(history.data()) << synchronize() << commit();
                if (history != expected || unchanged != source) {
                    throw std::runtime_error("ReSTIR extension history must preserve all FP32 bits, frame parity and complete current records");
                }
            }
            // Frame zero invalidates reuse, and must not publish stale current
            // extension state as history after an accumulation/camera reset.
            const auto before_reset = history;
            pipeline.stream() << frame_buffer.save_surface_exts_history(0u)
                              << read(output, 0u).dispatch(pixels)
                              << output.download(history.data()) << synchronize() << commit();
            if (history != before_reset) {
                throw std::runtime_error("ReSTIR extension history must skip the invalid first frame");
            }
            for (const bool planes : {false, true, false}) {
                integrator->set_stable_planes_enabled(planes);
                pipeline.invalidate();
                pipeline.upload_data();
                // Settle the mode transition and frame-zero invalidation.
                pipeline.display(1.0 / 60.0);
                for (uint step = 0u; step < 2u; ++step) {
                    std::vector<SurfaceExtend> receivers(pixels);
                    for (uint pixel = 0u; pixel < pixels; ++pixel) {
                        receivers[pixel].view_pos = {float(pixel) + 0.25f, -float(step + 1u), float(planes) + 0.5f};
                    }
                    pipeline.stream() << frame_buffer.cur_surface_exts_view(pipeline.frame_index()).upload(receivers.data())
                                      << synchronize() << commit();
                    auto camera_position = original_position;
                    camera_position.x += float(step + 1u) * 0.01f;
                    pipeline.scene().sensor()->set_position(camera_position);
                    pipeline.upload_data();
                    pipeline.display(1.0 / 60.0);
                    std::vector<array_float3> positions(pixels);
                    pipeline.stream() << frame_buffer.prev_surface_exts_view(pipeline.frame_index() - 1u)
                                          .template view_as<array_float3>().download(positions.data())
                                      << synchronize() << commit();
                    for (uint pixel = 0u; pixel < pixels; ++pixel) {
                        for (uint component = 0u; component < 3u; ++component) {
                            if (positions[pixel][component] != receivers[pixel].view_pos[component]) {
                                throw std::runtime_error("ReSTIR render must snapshot receiver history before stable-plane/DI writers in both SP modes and camera motion");
                            }
                        }
                    }
                }
            }
        }
        pipeline.scene().sensor()->set_position(original_position);
        integrator->set_stable_planes_enabled(original_planes);
        pipeline.invalidate();
        pipeline.upload_data();
        pipeline.display(1.0 / 60.0);
        std::cout << "PASS: ReSTIR compact FP32 extension history preserves bit patterns, current records, parity, resize, SP transitions and camera motion\n";
    }
}
} // namespace


// Optional process-to-process oracle for the production SP builder. The test
// binary exposes only a capture destination; production has no added knob.
void check_stable_plane_store_oracle(vision::Pipeline& pipeline, const char* label) {
    using namespace vision;
    const char* directory = std::getenv("CORONA_SP_STORE_ORACLE_DIR");
    if (!directory || !*directory) return;
    const auto out = std::filesystem::path(directory) / label;
    std::filesystem::create_directories(out);
    auto fail = [](bool okay, const char* message) { if (!okay) throw std::runtime_error(message); };
    auto& fb = *pipeline.frame_buffer();
    const auto original_resolution = pipeline.resolution();
    const bool original_denoise = pipeline.output_desc().denoise;
    const bool original_accumulation = fb.enable_accumulation();
    constexpr uint words = sizeof(StablePlaneData) / sizeof(uint);
    constexpr uint valid_word = offsetof(StablePlaneData, valid) / sizeof(uint);
    static_assert(words == 64u && valid_word == 61u);
    StablePlaneData defaults{};
    std::array<uint, words> default_words{};
    std::memcpy(default_words.data(), &defaults, sizeof(defaults));
    auto draw = [&] { pipeline.upload_data(); pipeline.display(1.0 / 60.0); };
    auto write_words = [&](const std::string& name, const std::vector<uint>& data) {
        std::ofstream file(out / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size() * sizeof(uint)));
        fail(bool(file), "could not write SP full-payload oracle file");
    };
    uint total_inactive = 0u, total_children = 0u;
    pipeline.set_output_denoise(false);
    fb.set_enable_accumulation(false);
    fb.auto_manage_accumulation_buffer(false);
    for (uint width : {64u, 128u, 63u, 65u}) {
        pipeline.stream() << synchronize() << commit();
        pipeline.change_resolution(make_uint2(width, 7u));
        fb.prepare_view_texture();
        pipeline.invalidate();
        draw(); draw(); // settle resize before uploading the complete canaries
        const uint pixels = pipeline.pixel_num();
        auto current = fb.cur_stable_planes_view(0u).view_as<uint>();
        fail(current.size() == size_t(pixels) * StablePlaneCount * words,
             "SP oracle must expose the full three-layer 256B current allocation");
        std::vector<uint> produced(current.size());
        pipeline.stream() << current.download(produced.data()) << synchronize() << commit();
        uint prototype = pixels;
        for (uint pixel = 0u; pixel < pixels; ++pixel) {
            const auto begin = size_t(pixel) * words;
            if (produced[begin + valid_word] != 0u && produced[begin] != InvalidUI32 &&
                produced[begin + 1u] != InvalidUI32 && produced[begin + 60u] != InvalidUI32) {
                prototype = pixel; break;
            }
        }
        fail(prototype < pixels, "SP oracle requires one real valid base record as its seed");
        for (uint word = 0u; word < words; ++word) {
            const bool floating = (word >= 2u && word <= 34u) ||
                (word >= 40u && word <= 49u) || (word >= 52u && word <= 56u);
            if (floating) fail((produced[size_t(prototype) * words + word] & 0x7f800000u) != 0x7f800000u,
                               "SP oracle seed must contain only finite FP32 fields");
        }
        std::vector<uint> seed(current.size()), actual(seed.size());
        for (uint prior_valid : {0u, 1u}) {
            for (uint record = 0u; record < pixels * StablePlaneCount; ++record) {
                for (uint word = 0u; word < words; ++word)
                    seed[size_t(record) * words + word] = produced[size_t(prototype) * words + word];
                // Real field values and IDs remain valid. Only padding gets
                // unique finite [0.5,1) bit canaries, avoiding arbitrary NaNs.
                seed[size_t(record) * words + 51u] = 0x3f000000u | ((record * 131u + 51u * 17u) & 0x007fffffu);
                seed[size_t(record) * words + 63u] = 0x3f000000u | ((record * 131u + 63u * 17u) & 0x007fffffu);
                seed[size_t(record) * words + valid_word] = record < pixels ? 0u : prior_valid;
            }
            pipeline.stream() << current.upload(seed.data()) << synchronize() << commit();
            draw(); // executes the real GBuffer, clear, SP build and consumers
            const uint frame = pipeline.frame_index() - 1u;
            std::vector<uint> ray_words(size_t(pixels) * sizeof(RayData) / sizeof(uint));
            std::vector<uint> hit_words(size_t(pixels) * sizeof(TriangleHit) / sizeof(uint));
            pipeline.stream() << current.download(actual.data())
                << fb.rays().view_as<uint>().download(ray_words.data())
                << fb.cur_visibility_buffer_view(frame).view_as<uint>().download(hit_words.data())
                << synchronize() << commit();
            uint inactive = 0u, children = 0u;
            for (uint record = 0u; record < pixels * StablePlaneCount; ++record) {
                const auto begin = size_t(record) * words;
                fail(actual[begin + 51u] == seed[begin + 51u] && actual[begin + 63u] == seed[begin + 63u],
                     "production SP stores/clears must preserve both padding canaries");
                if (record < pixels) continue;
                if (actual[begin + valid_word] != 0u) { ++children; continue; }
                ++inactive;
                for (uint word = 0u; word < words; ++word) {
                    if (word == 51u || word == 63u) continue;
                    const uint expected = prior_valid ? default_words[word] : seed[begin + word];
                    fail(actual[begin + word] == expected,
                         "inactive SP children must retain untouched payload or complete cleared defaults");
                }
            }
            total_inactive += inactive; total_children += children;
            // RayData contains eight padding bytes. Compare its forty defined
            // bytes per pixel separately so process allocation cannot fake a
            // changed camera input. TriangleHit has no padding.
            static_assert(sizeof(RayData) == 48u);
            std::vector<uint> ray_input(size_t(pixels) * 10u);
            for (uint pixel = 0u; pixel < pixels; ++pixel)
                for (uint word = 0u; word < 10u; ++word)
                    ray_input[size_t(pixel) * 10u + word] = ray_words[size_t(pixel) * 12u + word];
            const auto stem = std::to_string(width) + "-prior" + std::to_string(prior_valid);
            write_words(stem + "-planes.u32", actual);
            write_words(stem + "-rays.u32", ray_input);
            write_words(stem + "-hits.u32", hit_words);
            std::ofstream meta(out / (stem + ".txt"));
            meta << "width=" << width << " height=7 frame=" << frame << " inactive=" << inactive
                 << " children=" << children << " prior_valid=" << prior_valid << '\n';
            fail(bool(meta), "could not write SP oracle frame metadata");
        }
    }
    fail(total_inactive > 0u, "SP storage oracle must exercise inactive children");
    if (std::string(label) == "glass")
        fail(total_children > 0u, "glass SP storage oracle must exercise active child payloads");
    pipeline.change_resolution(original_resolution);
    fb.prepare_view_texture(); pipeline.invalidate();
    fb.set_enable_accumulation(original_accumulation);
    fb.auto_manage_accumulation_buffer(original_accumulation);
    pipeline.set_output_denoise(original_denoise);
    draw(); draw();
    std::cout << "PASS: production SP full-payload oracle " << label << " captures=" << out.string() << '\n';
}

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
    check_extension_history(pipeline, *pipeline.frame_buffer());
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

void check_stable_plane_history_lifecycle(vision::Pipeline& pipeline) {
    using namespace vision;
    auto fail = [](bool okay, const char* message) { if (!okay) throw std::runtime_error(message); };
    auto& fb = *pipeline.frame_buffer();
    const uint2 original_resolution = pipeline.resolution();
    const bool original_denoise = pipeline.output_desc().denoise;
    auto draw = [&] { pipeline.upload_data(); pipeline.display(1.0 / 60.0); };
    auto capacity = [&](bool retained) {
        fail(fb.cur_stable_planes_view(0u).size() == 3u * pipeline.pixel_num(),
             "raw stable planes must retain every complete current record");
        fail(fb.prev_stable_planes_view(0u).size() == (retained ? 3u * pipeline.pixel_num() : 3u),
             "stable-plane history capacity must follow the active consumer through toggles and resize");
    };
    auto same_bits = [](const auto& a, const auto& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; };
    for (auto resolution : {make_uint2(64u, 5u), make_uint2(128u, 5u),
                            make_uint2(63u, 5u), make_uint2(65u, 5u),
                            make_uint2(7u, 5u), make_uint2(1u), original_resolution}) {
        pipeline.set_output_denoise(false);
        draw();
        capacity(false);
        pipeline.change_resolution(resolution);
        fb.prepare_view_texture();
        pipeline.invalidate();
        capacity(false);
        draw(); draw();
        std::vector<StablePlaneData> previous_current(3u * pipeline.pixel_num());
        pipeline.stream() << fb.cur_stable_planes_view(0u).download(previous_current.data()) << synchronize() << commit();
        const uint frame_before_enable = pipeline.frame_index();
        pipeline.set_output_denoise(true);
        draw();
        capacity(true);
        fail(pipeline.frame_index() == frame_before_enable + 1u,
             "allocating denoiser history must preserve the previous raw frame for snapshotting");
        std::vector<StablePlaneHistory> previous(previous_current.size());
        pipeline.stream() << fb.prev_stable_planes_view(0u).download(previous.data()) << synchronize() << commit();
        uint valid = 0u;
        for (uint i = 0u; i < previous.size(); ++i) {
            const auto& a = previous[i]; const auto& b = previous_current[i];
            fail(a.valid == b.valid, "newly enabled SP history must preserve the previous raw record validity");
            if (b.valid == 0u) continue;
            ++valid;
            fail(same_bits(a.surface.hit, b.surface.hit) &&
                 same_bits(a.surface.virtual_position, b.surface.virtual_position) &&
                 same_bits(a.surface.virtual_geometric_normal, b.surface.virtual_geometric_normal) &&
                 a.surface.approximate == b.surface.approximate && a.surface.stable_branch == b.surface.stable_branch &&
                 same_bits(a.motion, b.motion) && same_bits(a.depth_position, b.depth_position) &&
                 a.branch_sequence == b.branch_sequence && a.instance_hash == b.instance_hash &&
                 a.depth == b.depth && a.material_id == b.material_id,
                 "enabling denoising must save every previous guide bit before current planes are overwritten");
        }
        fail(valid > 0u, "SP history lifecycle fixture must exercise valid previous records");
        pipeline.change_resolution(make_uint2(resolution.x + 2u, resolution.y + 1u));
        fb.prepare_view_texture();
        pipeline.invalidate();
        capacity(true);
        draw();
        pipeline.set_output_denoise(false);
        draw();
        capacity(false);
    }
    pipeline.change_resolution(original_resolution);
    fb.prepare_view_texture();
    pipeline.invalidate();
    capacity(false);
    // Existing callers retain history unless they explicitly opt out.
    fb.manage_stable_plane_buffers(true);
    capacity(true);
    fb.manage_stable_plane_buffers(true, false);
    capacity(false);
    pipeline.upload_bindless_array();
    pipeline.set_output_denoise(original_denoise);
    draw();
    capacity(original_denoise);
    std::cout << "PASS: SP history raw/on/raw allocation, exact prior guides, 1x1 and resize lifecycle\n";
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
    fail(std::abs(a.virtual_position[2] + 3.f) < 1e-4f && a.virtual_geometric_normal[2] > 0.999f,
         "mirror virtual position and normal must be reflected into camera space");
    if (tinted_mirror) fail(std::abs(a.denoiser_albedo[0] - 0.48f) < 0.01f && std::abs(a.denoiser_albedo[1] - 0.16f) < 0.01f,
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
        Float2 expected_prev = camera->prev_raster_coord(current.virtual_position.as_vec3()).xy();
        Float2 expected_cur = camera->raster_coord(current.virtual_position.as_vec3()).xy();
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
        pipeline.reset_status();
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
        if (!enabled) {
            fail(pipeline.frame_buffer()->stable_planes_size() <= StablePlaneCount,
                 "disabled stable planes must release full-resolution path storage");
            fail(denoiser->svgf_buffer_cur(0u, 1u).size() == 0u &&
                 denoiser->poisson()->guide_position().size() == 0u,
                 "disabled stable planes must release child history and spatial guides");
        } else {
            auto previous = pipeline.frame_buffer()->prev_stable_planes_view(0u);
            fail(previous.size() == pipeline.pixel_num() * StablePlaneCount &&
                 previous.size_in_byte() <= previous.size() * 128u,
                 "stable-plane history must retain only compact full-precision guides");
        }
        const auto frame_before_noop = pipeline.frame_index();
        integrator->set_stable_planes_enabled(enabled);
        fail(pipeline.frame_index() == frame_before_noop,
             "reapplying the same stable plane preference must preserve history");
        for (int i = 0; i < 3; ++i) { pipeline.upload_data(); pipeline.display(1.0 / 60.0); }
    }
    std::cout << "PASS: stable plane runtime on/off toggles and history reset\n";
    std::cout << "PASS: stable plane endpoint, branch rejection, virtual motion and SVGF history\n";
    check_stable_plane_history_lifecycle(pipeline);
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
        Float2 expected = camera->raster_coord(plane.surface.virtual_position.as_vec3()).xy() -
                          camera->prev_raster_coord(plane.surface.virtual_position.as_vec3()).xy();
        Bool current_ok = guide.valid && !guide.ambiguous && guide.replaced &&
            guide.hit.inst_id == plane.surface.hit.inst_id &&
            length(guide.position - plane.surface.virtual_position.as_vec3()) < 1e-5f &&
            length(guide.normal - plane.surface.virtual_geometric_normal.as_vec3()) < 1e-5f;
        Bool previous_ok = previous.valid && !previous.ambiguous && previous.replaced &&
            previous.hit.inst_id == old.surface.hit.inst_id &&
            length(previous.position - old.surface.virtual_position.as_vec3()) < 1e-5f &&
            length(previous.normal - old.surface.virtual_geometric_normal.as_vec3()) < 1e-5f;
        out.write(idx, make_float4(cast<float>(current_ok), cast<float>(previous_ok),
            length(motion_of(guide) - plane.motion), length(motion_of(guide) - expected)));
    };
    auto shader = pipeline.device().compile(kernel, "mirror_emitter_composed_guide");
    std::vector<float4> values(n);
    std::vector<StablePlaneData> current(n * StablePlaneCount);
    std::vector<StablePlaneHistory> previous(current.size());
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
            std::abs(p.surface.virtual_position[2] + 3.f) > 1e-4f || p.surface.hit.inst_id == InvalidUI32)
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
    auto old_planes = pipeline.device().create_buffer<StablePlaneHistory>(previous.size(), "emitter_coverage_old_planes");
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
        virtual_surfaces[i] = current[i].surface;
        auto &old = old_virtual_surfaces[i]; const auto &h = previous[i];
        old.hit = h.surface.hit; old.virtual_position = h.surface.virtual_position;
        old.virtual_geometric_normal = h.surface.virtual_geometric_normal;
        old.depth_position = h.depth_position; old.approximate = h.surface.approximate;
        old.stable_branch = h.surface.stable_branch; old.is_replaced = true;
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

// Isolate production gather/validation from the unchanged estimator combine.
namespace {
class DITemporalGatherOracle final : public vision::ReSTIRDI {
public:
    mutable vision::Uint* accepted{};
    mutable vision::Float3* received_view{};
    mutable vision::Float3* received_prev_view{};
    DITemporalGatherOracle() { temporal_.N = 9u; }
    [[nodiscard]] vision::DIReservoirVar combine_temporal(
        const vision::DIReservoirVar&, const vision::SurfaceDataVar&,
        vision::DIReservoirVar& donor, const vision::SurfaceDataVar&,
        vision::Float3 view, vision::Float3 previous_view) const noexcept override {
        *accepted = donor.sample.prim_id;
        *received_view = view; *received_prev_view = previous_view;
        donor.sample.age += 1u;
        // The real combine consumes exactly one next_1d after four targets.
        // Target evaluation does not consume random samples.
        (void)sampler()->next_1d();
        return donor;
    }
    [[nodiscard]] vision::DIReservoirVar reference(
        vision::DIReservoirVar rsv, const vision::SurfaceDataVar& cur_surf,
        const vision::Float2& motion_vec, const vision::SensorSample& ss,
        const vision::Var<vision::DIParam>& param) const noexcept;
};

vision::DIReservoirVar DITemporalGatherOracle::reference(
    vision::DIReservoirVar rsv, const vision::SurfaceDataVar& cur_surf,
    const vision::Float2& motion_vec, const vision::SensorSample& ss,
    const vision::Var<vision::DIParam>& param) const noexcept {
    using namespace vision;
    Float2 prev_p_film = previous_reservoir_coord(ss.p_film, motion_vec);
    Int2 prev_p = reservoir_pixel(prev_p_film);
    Float limit = rsv.C * param.history_limit;
    Int2 res = make_int2(dispatch_dim().xy());
    auto& camera = scene().sensor();
    Float3 view_pos = camera->device_position();
    Float3 prev_view_pos = camera->prev_device_position();
    auto get_prev_data = [this, &limit](const Uint2& pos, Float3& view_pos) {
        Uint index = dispatch_id(make_uint2(pos));
        DIReservoirVar prev_rsv = prev_reservoirs().read(index);
        prev_rsv->truncation(limit);
        SurfaceDataVar surf = prev_surfaces().read(index);
        view_pos = scene().sensor()->prev_device_position();
        $if(surf.is_replaced) { view_pos = prev_surface_extends().read(index).view_pos.as_vec3(); };
        return make_pair(surf, prev_rsv);
    };
    view_pos = cur_view_pos(cur_surf.is_replaced);
    $if(in_screen(prev_p, res) && param.temporal) {
        auto data = get_prev_data(make_uint2(prev_p), prev_view_pos);
        auto prev_surf = data.first;
        auto prev_rsv = data.second;
        $if(is_temporal_valid(cur_surf, prev_surf, param, addressof(prev_rsv.sample))) {
            rsv = combine_temporal(rsv, cur_surf, prev_rsv, prev_surf, view_pos, prev_view_pos);
        }
        $else {
            $for(i, temporal_.N) {
                Int2 p = reservoir_pixel(square_to_disk(sampler()->next_2d()) * param.t_radius + prev_p_film);
                $if(!in_screen(p, res)) { $continue; };
                auto data = get_prev_data(make_uint2(p), prev_view_pos);
                auto another_surf = data.first;
                auto another_rsv = data.second;
                $if(is_temporal_valid(cur_surf, another_surf, param, addressof(another_rsv.sample))) {
                    rsv = combine_temporal(rsv, cur_surf, another_rsv, another_surf, view_pos, prev_view_pos);
                    $break;
                };
            };
        };
    };
    return rsv;
}
} // namespace

void check_di_temporal_gather_equivalence(vision::Pipeline& pipeline) {
    using namespace vision;
    pipeline.activate_global_context();
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    auto& camera = pipeline.scene().sensor();
    auto& random = pipeline.renderer().sampler();
    const uint pixels = pipeline.pixel_num();
    const uint2 resolution = pipeline.resolution();
    if (resolution.x < 8u || resolution.y < 8u)
        throw std::runtime_error("DI temporal gather oracle needs at least an 8x8 fixture");
    DITemporalGatherOracle di;
    di.radiance() = make_shared<ScreenBuffer>("di_temporal_gather_oracle_radiance");
    const uint first_slot = pipeline.bindless_array().buffer_num();
    di.prepare();
    const uint last_slot = pipeline.bindless_array().buffer_num();
    pipeline.upload_bindless_array();
    constexpr uint words = 23u;
    const uint center = (resolution.y / 2u) * resolution.x + resolution.x / 2u;
    auto surface = [] {
        SurfaceDataVar result;
        result.hit.inst_id = 0u; result.hit.prim_id = 0u;
        result.hit.bary = make_float2(0.25f, 0.5f);
        result.normal_depth = make_float4(0.f, 0.f, 1.f, 2.f);
        result.pos_diff = make_float4(0.f, 0.f, 2.f, 1.f);
        result.diffuse_roughness = make_float4(0.5f, 0.25f, 0.125f, 0.4f);
        result.specular_roughness = make_float4(0.04f, 0.04f, 0.04f, 0.4f);
        result.flag = SurfaceData::Diffuse;
        return result;
    };
    Kernel initialize = [&](Uint scenario) {
        Env::instance().clear_global_vars();
        di.emplace_frame_index(Uint{1u});
        SurfaceDataVar previous = surface();
        DIReservoirVar reservoir;
        Uint index = dispatch_id();
        reservoir.weight_sum = cast<float>(index) + 0.125f;
        reservoir.C = 31.f; reservoir.W = cast<float>(index) + 0.75f;
        reservoir.sample.light_index = 7u; reservoir.sample.prim_id = index + 1u;
        reservoir.sample.bary = make_float2(0.125f, 0.75f);
        reservoir.sample.age = 2u; reservoir.sample.p_hat = cast<float>(index) + 0.5f;
        reservoir.sample->set_pos(make_float3(cast<float>(index), Float{0.375f}, Float{-1.5f}));
        // A valid neighborhood surrounds one invalid primary reprojection.
        $if((scenario == 1u || scenario == 2u || scenario == 3u) &&
            (scenario == 3u || index == center)) { reservoir.sample.age = 30u; };
        $if((scenario == 6u && index == center) || (scenario == 13u && index == 0u)) {
            previous.hit.inst_id = InvalidUI32; previous.hit.prim_id = InvalidUI32;
        };
        $if(scenario == 7u) { previous.diffuse_roughness.x = 1.f; };
        $if(scenario == 8u) { previous.pos_diff.z = 10.f; };
        $if(scenario == 9u) { previous.stable_branch = 2u; };
        $if(scenario == 10u || scenario == 11u) {
            previous.is_replaced = true;
            previous.stable_branch = ocarina::select(scenario == 10u, 1u, InvalidUI32);
            previous.virtual_position.set(Float3{make_float3(0.f, 0.f, 2.f)});
        };
        $if(scenario == 12u) { previous.normal_depth.z = -1.f; };
        di.prev_reservoirs().write(index, reservoir);
        di.prev_surfaces().write(index, previous);
        SurfaceExtendHistoryVar extension;
        extension.view_pos.set(make_float3(cast<float>(index) * 0.001f, Float{0.25f}, Float{-0.5f}));
        di.prev_surface_extends().write(index, extension);
        SurfaceExtendVar current_extension;
        current_extension.view_pos.set(Float3{make_float3(0.5f, -0.25f, 0.125f)});
        di.cur_surface_extends().write(index, current_extension);
    };
    auto initializer = pipeline.device().compile(initialize, "di_temporal_oracle_seed");
    auto compile = [&](bool reference) {
        Kernel kernel = [&](Uint scenario, Var<DIParam> param, BufferVar<uint> output) {
            Env::instance().clear_global_vars();
            di.emplace_frame_index(Uint{1u});
            camera->load_data(); random->load_data();
            camera->set_device_c2w(Float4x4{make_float4x4(1.f)});
            random->set_seed(dispatch_idx().xy(), Uint{31u}, Dimension::PathTracing);
            SurfaceDataVar current = surface();
            $if(scenario == 10u || scenario == 11u) {
                current.is_replaced = true; current.stable_branch = 1u;
                current.virtual_position.set(Float3{make_float3(0.f, 0.f, 2.f)});
            };
            DIReservoirVar reservoir;
            reservoir.weight_sum = 0.875f; reservoir.C = 2.f; reservoir.W = 0.625f;
            reservoir.sample.light_index = 3u; reservoir.sample.prim_id = 900u;
            reservoir.sample.bary = make_float2(0.25f, 0.625f);
            reservoir.sample.age = 4u; reservoir.sample.p_hat = 0.75f;
            reservoir.sample->set_pos(make_float3(0.125f, -0.5f, 0.25f));
            SensorSample ss;
            ss.p_film = make_float2(float(resolution.x / 2u) + 0.5f, float(resolution.y / 2u) + 0.5f);
            $if(scenario == 5u) { ss.p_film = make_float2(-0.5f, 0.5f); };
            $if(scenario == 13u) { ss.p_film = make_float2(0.5f); };
            Uint accepted = 0u;
            Float3 view = make_float3(0.f), previous_view = make_float3(0.f);
            di.accepted = &accepted; di.received_view = &view; di.received_prev_view = &previous_view;
            auto result = reference ? di.reference(reservoir, current, make_float2(0.f), ss, param)
                                    : di.temporal_reuse(reservoir, current, make_float2(0.f), ss, param);
            Float actual_next = random->next_1d();
            random->set_seed(dispatch_idx().xy(), Uint{31u}, Dimension::PathTracing);
            Bool known_eighteen = scenario == 1u || scenario == 3u ||
                                  (scenario >= 7u && scenario <= 9u) || scenario == 11u || scenario == 12u;
            $if(known_eighteen) { $for(i, 9u) { (void)random->next_2d(); }; };
            $if(scenario == 0u || scenario == 10u) { (void)random->next_1d(); };
            Float expected_next = random->next_1d();
            Uint start = dispatch_id() * words;
            auto put = [&](uint word, const Float& value) { output.write(start + word, as<uint>(value)); };
            output.write(start, accepted);
            put(1u, result.weight_sum); put(2u, result.C); put(3u, result.W);
            output.write(start + 4u, result.sample.light_index);
            output.write(start + 5u, result.sample.prim_id);
            put(6u, result.sample.bary.x); put(7u, result.sample.bary.y);
            output.write(start + 8u, result.sample.age); put(9u, result.sample.p_hat);
            for (uint component = 0u; component < 3u; ++component) {
                put(10u + component, result.sample->p_light()[component]);
                put(13u + component, view[component]); put(16u + component, previous_view[component]);
            }
            put(19u, actual_next); put(20u, expected_next);
            output.write(start + 21u, cast<uint>(known_eighteen || scenario == 0u || scenario == 4u ||
                                                scenario == 5u || scenario == 10u));
            output.write(start + 22u, cast<uint>(ReSTIRDI::is_temporal_valid(current, current, param, nullptr)));
        };
        return pipeline.device().compile(kernel, reference ? "di_temporal_gather_reference" : "di_temporal_gather_production");
    };
    auto reference_shader = compile(true);
    auto production_shader = compile(false);
    auto buffer = pipeline.device().create_buffer<uint>(pixels * words, "di_temporal_gather_oracle_results");
    std::vector<uint> expected(pixels * words), actual(expected.size());
    uint positive_fallbacks = 0u, rejected = 0u, replaced = 0u;
    for (uint scenario = 0u; scenario < 14u; ++scenario) {
        DIParam param{};
        param.temporal = scenario != 4u; param.max_age = 30u; param.history_limit = 5u;
        param.t_dot = 0.8f; param.t_depth = 0.3f; param.diff_factor = 0.3f;
        param.t_radius = scenario == 2u || scenario == 3u || scenario == 6u || scenario == 13u ? 4.f : 0.f;
        pipeline.stream() << initializer(scenario).dispatch(resolution)
                          << reference_shader(scenario, param, buffer).dispatch(resolution) << buffer.download(expected.data())
                          << production_shader(scenario, param, buffer).dispatch(resolution) << buffer.download(actual.data())
                          << synchronize() << commit();
        if (expected != actual) {
            uint word = 0u; while (expected[word] == actual[word]) ++word;
            throw std::runtime_error("DI temporal gather parity failed scenario=" + std::to_string(scenario) +
                                     " word=" + std::to_string(word));
        }
        uint accepted_count = 0u;
        for (uint pixel = 0u; pixel < pixels; ++pixel) {
            const uint start = pixel * words;
            if (actual[start + 22u] != 1u) throw std::runtime_error("DI temporal oracle invalid canonical fixture");
            if (actual[start + 21u] && actual[start + 19u] != actual[start + 20u])
                throw std::runtime_error("DI temporal oracle changed the independently replayed RNG consumption");
            if (actual[start]) {
                ++accepted_count;
                if (actual[start + 8u] != 3u) throw std::runtime_error("DI temporal gather age must increment once");
                if (actual[start + 2u] != 0x41200000u) throw std::runtime_error("DI temporal history C must truncate to 2*5=10");
                if (scenario == 10u &&
                    (actual[start + 13u] != 0x3f000000u || actual[start + 14u] != 0xbe800000u ||
                     actual[start + 15u] != 0x3e000000u || actual[start + 17u] != 0x3e800000u ||
                     actual[start + 18u] != 0xbf000000u))
                    throw std::runtime_error("DI temporal replaced receivers must load their distinct current/history view origins");
                if ((scenario == 2u || scenario == 6u) && actual[start] == center + 1u)
                    throw std::runtime_error("DI temporal fallback accepted its invalid initial pixel");
                if (scenario == 13u && actual[start] == 1u)
                    throw std::runtime_error("DI temporal corner fallback accepted its invalid initial pixel");
            }
        }
        const bool first_valid = scenario == 0u || scenario == 10u;
        const bool fallback = scenario == 2u || scenario == 6u || scenario == 13u;
        if (first_valid && accepted_count != pixels) throw std::runtime_error("DI temporal oracle did not accept all valid initial pixels");
        if (fallback && accepted_count == 0u) throw std::runtime_error("DI temporal oracle did not exercise positive-radius fallback");
        if (!first_valid && !fallback && accepted_count != 0u) throw std::runtime_error("DI temporal oracle accepted invalid history");
        positive_fallbacks += fallback ? accepted_count : 0u;
        replaced += scenario == 10u ? accepted_count : 0u;
        rejected += !first_valid && !fallback ? pixels : 0u;
    }
    pipeline.frame_buffer()->unregister(di.radiance());
    ByteBufferView empty{handle_ty{0u}, 0u};
    for (uint slot = first_slot; slot < last_slot; ++slot) pipeline.bindless_array().set_buffer(slot, empty);
    pipeline.invalidate();
    std::cout << "PASS: DI temporal real gather parity, 14 scenarios; fallback=" << positive_fallbacks
              << " replaced=" << replaced << " rejected=" << rejected << "; zero-radius consumes 18 RNG scalars\n";
}

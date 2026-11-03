#pragma once

// Actual scene/ray regression for the guide-to-ReBLUR conversion dataflow. All
// variants use the same default wavelengths, material graphs and sampler seeds.
inline void check_reblur_guide_fusion() {
    using namespace vision;
    namespace fs = std::filesystem;
    {
        reblur::Filter cache;
        const auto size = make_uint2(13u, 9u);
        const size_t pixels = size_t(size.x) * size.y;
        cache.prepare_native(Global::instance().device(), size, true);
        if (cache.guides().size() != pixels * 32u)
            throw std::runtime_error("fused remodulation cache must allocate exactly 32 bytes per pixel");
        cache.prepare_native(Global::instance().device(), size);
        if (cache.guides().size() != pixels * 80u)
            throw std::runtime_error("native and replay guide cache must retain its original 80-byte layout");
        cache.prepare_native(Global::instance().device(), size, true);
        if (cache.guides().size() != pixels * 32u)
            throw std::runtime_error("switching back to compact guide storage must release the unused guide planes");
    }
    struct Environment {
        std::string name, old;
        bool present;
        static void set(const char *key, const char *value) {
#ifdef _WIN32
            _putenv_s(key, value ? value : "");
#else
            if (value) setenv(key, value, 1); else unsetenv(key);
#endif
        }
        Environment(const char *key, const char *value) : name(key),
            old(std::getenv(key) ? std::getenv(key) : ""), present(std::getenv(key) != nullptr) { set(key, value); }
        ~Environment() { set(name.c_str(), present ? old.c_str() : nullptr); }
    } profiling{"REBLUR_GPU_PROFILE", nullptr},
      snapshot_frame{"REBLUR_SNAPSHOT_FRAME", "2"};
    const auto directory = fs::temp_directory_path() /
        ("corona_guide_fusion_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    std::cerr << "guide_oracle evidence=" << directory.string() << std::endl;
    std::ofstream(directory / "receiver.obj") << "v -4 -4 -1\nv 4 -4 -1\nv 0 4 -1\nvn 0 0 1\nf 1//1 2//1 3//1\n";
    std::ofstream(directory / "back.obj") << "v -4 -4 -3\nv 4 -4 -3\nv 0 4 -3\nvn 0 0 1\nf 1//1 2//1 3//1\n";
    std::ofstream(directory / "front.obj") << "v -4 -4 1\nv 0 4 1\nv 4 -4 1\nvn 0 0 -1\nf 1//1 2//1 3//1\n";
    std::ofstream(directory / "textured-receiver.obj") <<
        "v -4 -4 -1\nv 4 -4 -1\nv 4 4 -1\nv -4 4 -1\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\n"
        "f 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n";
    std::ofstream(directory / "bounce.obj") <<
        "v -4 -4 1\nv -4 4 1\nv 4 4 1\nv 4 -4 1\nvn 0 0 -1\n"
        "f 1//1 2//1 3//1\nf 1//1 3//1 4//1\n";
    std::ofstream(directory / "ceiling-light.obj") <<
        "v -3 2 -0.5\nv 3 2 -0.5\nv 3 2 0.8\nv -3 2 0.8\nvn 0 -1 0\n"
        "f 1//1 2//1 3//1\nf 1//1 3//1 4//1\n";
    {
        const unsigned char colors[]{210,55,25, 35,180,80, 75,40,190, 180,155,45};
        std::ofstream image(directory / "guide-albedo.ppm", std::ios::binary);
        image << "P6\n2 2\n255\n";
        image.write(reinterpret_cast<const char *>(colors), sizeof(colors));
    }
    auto read_bytes = [](const fs::path &path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("missing guide fusion evidence: " + path.string());
        const auto size = input.tellg(); std::vector<char> bytes(static_cast<size_t>(size));
        input.seekg(0); input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return bytes;
    };
    uint selected_scenario = InvalidUI32;
    if (const char *selected = std::getenv("REBLUR_GUIDE_ORACLE_SCENARIO")) {
        if (selected[0] < '0' || selected[0] > '4' || selected[1] != '\0')
            throw std::runtime_error("REBLUR_GUIDE_ORACLE_SCENARIO must be an integer in [0,4]");
        selected_scenario = uint(selected[0] - '0');
    }
    for (uint scenario = 0u; scenario < 5u; ++scenario) {
        if (selected_scenario != InvalidUI32 && selected_scenario != scenario) continue;
        const auto stage = [scenario](const char *name, uint variant = InvalidUI32) {
            std::cerr << "guide_oracle scenario=" << scenario;
            if (variant != InvalidUI32) std::cerr << " variant=" << variant;
            std::cerr << " stage=" << name << std::endl;
        };
        stage("scene_begin");
        auto scene = DataWrap::parse(R"({
          "scene": {
            "camera":{"type":"thin_lens","param":{"fov_y":45,"lens_radius":0,"focal_distance":3,"filter":{"type":"box"},"transform":{"type":"look_at","param":{"position":[0,0,0],"target_pos":[0,0,-1],"up":[0,1,0]}}}},
            "materials":[{"type":"principled_bsdf","name":"receiver","param":{"color":[0.7,0.2,0.1],"roughness":0.3,"subsurface_weight":0,"spec_tint":[0.1,0.4,0.8]}}],
            "shapes":[{"type":"model","param":{"fn":"receiver.obj","material":"receiver","normalize_to_unit_bounds":false}}],
            "lights":[{"type":"spherical","param":{"color":{"channels":"xyz","node":{"type":"number","param":{"value":[1,1,1]}}},"scale":1}}]
          },
          "render":{"sampler":{"type":"independent","param":{"spp":1}},"light_sampler":{"type":"uniform"},
            "integrator":{"type":"rt","param":{"max_depth":4,"min_depth":4,"denoiser":{"type":"ReBLUR"},
              "direct":{"stable_planes":true,"M_light":8,"M_bsdf":1,"temporal":{"open":false},"spatial":{"open":false}},
              "indirect":{"temporal":{"open":false},"spatial":{"open":false}}}}},
          "pipeline":{"type":"fixed","param":{"frame_buffer":{"type":"normal","param":{"resolution":[13,9]}}}},
          "output":{"spp":0,"save_exit":false,"denoise":true,"fn":"unused.exr"}
        })");
        if (scenario == 0u) scene["scene"]["lights"][0]["param"]["scale"] = 0.f;
        if (scenario == 1u) {
            scene["scene"]["camera"]["param"]["lens_radius"] = .02f;
            scene["scene"]["materials"] = DataWrap::parse(R"([
              {"name":"glass","type":"principled_bsdf","param":{"roughness":0.03162277660168379,"subsurface_weight":0,"transmission_weight":1,"ior":1.5,"color":[1,1,1]}},
              {"name":"red","type":"principled_bsdf","param":{"color":[0.8,0.1,0.1],"subsurface_weight":0,"roughness":1,"spec_tint":[0,0,0]}},
              {"name":"blue","type":"principled_bsdf","param":{"color":[0.1,0.1,0.8],"subsurface_weight":0,"roughness":1,"spec_tint":[0,0,0]}}])");
            scene["scene"]["shapes"] = DataWrap::parse(R"([
              {"type":"model","param":{"fn":"receiver.obj","material":"glass","normalize_to_unit_bounds":false}},
              {"type":"model","param":{"fn":"back.obj","material":"red","normalize_to_unit_bounds":false}},
              {"type":"model","param":{"fn":"front.obj","material":"blue","normalize_to_unit_bounds":false}}])");
        }
        if (scenario == 2u) {
            scene["scene"]["materials"][0] = DataWrap::parse(R"({"type":"principled_bsdf","name":"receiver","param":{"color":[0.8,0.2,0.1],"coat_weight":1,"coat_ior":2.5,"roughness":0.4,"subsurface_weight":0,"normal":{"node":"tilted","channels":"xyz"}},"node_tab":{"tilted":{"type":"number","param":{"value":[0.6,0,0.8]}}}})");
            scene["render"]["integrator"]["param"]["direct"]["stable_planes"] = false;
        }
        if (scenario >= 3u) {
            stage("textured_materials_begin");
            // A directly lit textured receiver and a separately lit bounce
            // surface give the real GI final shader positive receiver samples.
            scene["scene"]["materials"] = DataWrap::parse(R"([
              {"name":"textured","type":"principled_bsdf","param":{
                "color":{"channels":"xyz","node":{"type":"image","param":{"fn":"guide-albedo.ppm","color_space":"linear","filter":"nearest"}}},
                "roughness":0.55,"coat_weight":0.3,"subsurface_weight":0,"spec_tint":[0.2,0.4,0.8],
                "normal":{"channels":"xyz","node":"mapped"}},
                "node_tab":{"mapped":{"type":"converter","construct_name":"normal_map","param":{"color":[0.65,0.5,0.95],"strength":0.3}}}},
              {"name":"bounce","type":"principled_bsdf","param":{"color":[0.3,0.7,0.5],"subsurface_weight":0,"roughness":0.65}}
            ])");
            stage("textured_materials_done");
            stage("textured_shapes_begin");
            scene["scene"]["shapes"] = DataWrap::parse(R"([
              {"type":"model","param":{"fn":"textured-receiver.obj","material":"textured","normalize_to_unit_bounds":false}},
              {"type":"model","param":{"fn":"bounce.obj","material":"bounce","normalize_to_unit_bounds":false}},
              {"type":"model","param":{"fn":"ceiling-light.obj","material":"bounce","normalize_to_unit_bounds":false,
                "emission":{"type":"area","param":{"color":[1,0.8,0.6],"scale":8}}}}
            ])");
            stage("textured_shapes_done");
            if (scenario == 4u) {
                // Zero tint/no coat still has grazing Fresnel reflection; a
                // sampled endpoint also remains valid at zero signal energy.
                scene["scene"]["materials"][0]["param"]["coat_weight"] = 0.f;
                scene["scene"]["materials"][0]["param"]["spec_tint"] = DataWrap::parse("[0,0,0]");
            }
        }
        std::ofstream(directory / ("scene-" + std::to_string(scenario) + ".json")) << scene.dump(2);
        stage("scene_done");
        std::vector<float4> reference_rgb, reference_diffuse, reference_specular;
        std::vector<float4> edsl_rgb;
        for (uint variant = 0u; variant < 5u; ++variant) {
            const bool fused = variant >= 2u;
            const bool final_output_only = variant == 4u;
            const char *variant_name = variant == 4u ? "-edsl-core-batch-final-only" :
                (variant == 0u ? "-legacy-staged" : (variant == 1u ? "-precise-staged" :
                (variant == 2u ? "-precise-fused80" : "-precise-fused32")));
            const auto capture = directory / (std::to_string(scenario) + variant_name);
            Environment producer{"VISION_RESTIR_PRECISE_LOBES", variant == 0u ? "0" : "1"};
            Environment mode{"REBLUR_FUSE_GUIDES", fused ? "1" : "0"};
            Environment compact{"REBLUR_COMPACT_GUIDE_CACHE", variant >= 3u ? "1" : "0"};
            // The first four paths retain filtered D/S. Leave the batch selector
            // unset in the final path to exercise the production default.
            Environment batching{"REBLUR_BATCH_COMPOSE", final_output_only ? nullptr : "0"};
            Environment composition{"REBLUR_COMPOSE_IN_UNPACK", nullptr};
            Environment snapshot{"REBLUR_EDSL_SNAPSHOT_DIR", capture.string().c_str()};
            Global::instance().set_scene_path(directory);
            stage("make_pipeline_begin", variant);
            auto pipeline = Corona::Tests::make_vision_test_pipeline(scene, directory);
            stage("make_pipeline_done", variant);
            auto *integrator = dynamic_cast<IlluminationIntegrator *>(pipeline->renderer().integrator().get());
            if (!integrator) throw std::runtime_error("guide fusion fixture requires the actual illumination integrator");
            integrator->set_denoise_enabled(true); pipeline->frame_buffer()->set_enable_accumulation(false);
            stage("prepare_begin", variant);
            pipeline->prepare();
            stage("prepare_done", variant);
            pipeline->frame_buffer()->prepare_view_texture(); pipeline->upload_data();
            stage("upload_done", variant);
            auto &fb = *pipeline->frame_buffer();
            const uint initial_raw_count = scenario == 2u || variant != 0u ? StablePlaneCount : pipeline->pixel_num() * StablePlaneCount;
            if (fb.stable_direct().size() != initial_raw_count || fb.stable_indirect().size() != initial_raw_count)
                throw std::runtime_error("initial raw stable allocation must follow the prepared producer policy");
            for (uint frame = 0u; frame < 3u; ++frame) {
                stage("render_begin", variant);
                if (frame == 2u) {
                    pipeline->scene().sensor()->set_yaw(.2f);
                    pipeline->scene().sensor()->update_device_data();
                }
                pipeline->upload_data(); pipeline->display(1.0 / 60.0);
                stage("render_done", variant);
            }
            const uint pixels = pipeline->pixel_num();
            std::vector<float4> rgb(pixels), diffuse(pixels * StablePlaneCount), specular(diffuse.size());
            pipeline->stream() << fb.rt_buffer().device_buffer().download(rgb.data());
            // Batch Pack retires raw D/S into guide/raw fallback caches. Their
            // filtered-output comparison belongs only to retained paths.
            if (!final_output_only)
                pipeline->stream() << fb.denoiser_diffuse().view().download(diffuse.data())
                    << fb.denoiser_specular().view().download(specular.data());
            pipeline->stream() << synchronize() << commit();
            if (variant == 0u) { reference_rgb = rgb; reference_diffuse = diffuse; reference_specular = specular; }
            else {
                const auto exact = [](const auto &a, const auto &b) {
                    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0;
                };
                if (final_output_only) {
                    if (!exact(rgb, edsl_rgb))
                        throw std::runtime_error("scene " + std::to_string(scenario) + variant_name +
                            " batched final RT must match retained EDSL composition byte-for-byte");
                } else if (!exact(rgb, reference_rgb) || !exact(diffuse, reference_diffuse) || !exact(specular, reference_specular)) {
                    throw std::runtime_error("scene " + std::to_string(scenario) + variant_name +
                        " changed final FP32 radiance or world-space lobe hit distances beyond the reference contract");
                }
                const auto staged = directory / (std::to_string(scenario) + "-legacy-staged");
                const uint layers = scenario == 2u ? 1u : StablePlaneCount;
                for (uint layer = 0u; layer < layers; ++layer) for (uint slot = 0u; slot < 6u; ++slot) {
                    const auto name = "f2_l" + std::to_string(layer) + "_p0_u" + std::to_string(slot) + ".bin";
                    const auto packed = read_bytes(capture / name);
                    if (packed != read_bytes(staged / name))
                        throw std::runtime_error("scene " + std::to_string(scenario) + variant_name +
                            " changed a packed ReBLUR input: " + name);
                    if (scenario == 0u && layer == 0u && slot < 2u) {
                        bool sampled_black_hit = false;
                        for (size_t offset = 6u; offset + 1u < packed.size(); offset += 8u) {
                            const auto bits = static_cast<unsigned char>(packed[offset]) |
                                (uint(static_cast<unsigned char>(packed[offset + 1u])) << 8u);
                            sampled_black_hit |= (bits & 0x7fffu) > 0u && (bits & 0x7fffu) < 0x7c00u;
                        }
                        if (!sampled_black_hit)
                            throw std::runtime_error("black diffuse and specular samples must retain positive finite packed hit distances");
                    }
                }
                if (fused) {
                    const auto metadata = read_bytes(capture / "metadata.jsonl");
                    if (std::string(metadata.begin(), metadata.end()).find("Pack-FusedGuides") == std::string::npos)
                        throw std::runtime_error("guide fusion regression must execute the fused pack command");
                    const bool batched = std::string(metadata.begin(), metadata.end()).find("Pack-FusedGuides-BatchCompose") != std::string::npos;
                    if (batched != final_output_only)
                        throw std::runtime_error("batch fixture must select only the production-default final-output-only path");
                    std::array<bool, StablePlaneCount> captured_pack{};
                    std::istringstream metadata_lines(std::string(metadata.begin(), metadata.end()));
                    std::string metadata_line;
                    while (std::getline(metadata_lines, metadata_line)) {
                        if (metadata_line.empty()) continue;
                        const auto record = DataWrap::parse(metadata_line);
                        if (record.at("pass").get<uint>() != 0u) continue;
                        const uint layer = record.at("layer").get<uint>();
                        if (layer >= layers) throw std::runtime_error("Pack metadata layer exceeds the scene layer count");
                        const auto shader = record.at("shader").get<std::string>();
                        const bool marked_batched = shader.find("Pack-FusedGuides-BatchCompose") != std::string::npos;
                        if (marked_batched != final_output_only)
                            throw std::runtime_error("every Pack must match the selected retained or batch path");
                        const bool marked_precleared = shader.find("Pack-FusedGuides-BatchCompose-Precleared") != std::string::npos;
                        if (marked_precleared)
                            throw std::runtime_error("EDSL Pack must not use an external-core preclear path");
                        captured_pack[layer] = true;
                    }
                    for (uint layer = 0u; layer < layers; ++layer)
                        if (!captured_pack[layer]) throw std::runtime_error("guide oracle requires Pack metadata for every layer");
                }
            }
            if (variant == 3u) edsl_rgb = rgb;
            if (scenario == 1u && (variant == 3u || variant == 4u)) {
                const uint direct_base = fb.stable_direct_base(), indirect_base = fb.stable_indirect_base();
                uint lifecycle_step = 0u;
                auto check_raw = [&](bool retained) {
                    const uint step = lifecycle_step++;
                    const uint expected = retained && integrator->stable_planes_enabled()
                        ? pipeline->pixel_num() * StablePlaneCount : StablePlaneCount;
                    if (fb.stable_raw_outputs_retained() != retained || fb.stable_direct().size() != expected ||
                        fb.stable_indirect().size() != expected || fb.stable_direct_base() != direct_base ||
                        fb.stable_indirect_base() != indirect_base)
                        throw std::runtime_error("actual ReSTIR raw allocation lifecycle step=" + std::to_string(step) +
                            " expected retained/count=" + std::to_string(retained) + "/" + std::to_string(expected) +
                            " actual=" + std::to_string(fb.stable_raw_outputs_retained()) + "/" +
                            std::to_string(fb.stable_direct().size()) + "/" + std::to_string(fb.stable_indirect().size()) +
                            " expected slots=" + std::to_string(direct_base) + "/" + std::to_string(indirect_base) +
                            " actual slots=" + std::to_string(fb.stable_direct_base()) + "/" + std::to_string(fb.stable_indirect_base()));
                };
                auto render = [&] {
                    pipeline->upload_data(); pipeline->display(1.0 / 60.0);
                    std::vector<float4> output(pipeline->pixel_num());
                    pipeline->stream() << fb.rt_buffer().view().download(output.data()) << synchronize() << commit();
                    for (const auto &value : output) for (uint c = 0u; c < 4u; ++c)
                        if (!std::isfinite(value[c])) throw std::runtime_error("raw allocation lifecycle produced a non-finite frame");
                };
                check_raw(false);
                pipeline->set_output_denoise(false); render(); check_raw(true);
                pipeline->set_output_denoise(true); render(); check_raw(false);
                {
                    const auto debug_directory = directory / "raw-lifecycle-debug";
                    const auto debug_frame = std::to_string(integrator->frame_index());
                    Environment debug_root{"VISION_EVAL_DEBUG_DIR", debug_directory.string().c_str()};
                    Environment debug_index{"VISION_EVAL_DEBUG_FRAME", debug_frame.c_str()};
                    render(); check_raw(true);
                    for (uint layer = 0u; layer < StablePlaneCount; ++layer) {
                        std::vector<float4> raw_direct(pipeline->pixel_num()), raw_indirect(raw_direct.size());
                        pipeline->stream() << fb.stable_direct_view(layer).download(raw_direct.data())
                            << fb.stable_indirect_view(layer).download(raw_indirect.data()) << synchronize() << commit();
                        const auto prefix = "raw_layer_" + std::to_string(layer);
                        const auto direct_file = read_bytes(debug_directory / (prefix + "_direct.f32"));
                        const auto indirect_file = read_bytes(debug_directory / (prefix + "_indirect.f32"));
                        const size_t bytes = raw_direct.size() * sizeof(float4);
                        if (direct_file.size() != bytes || indirect_file.size() != bytes ||
                            std::memcmp(direct_file.data(), raw_direct.data(), bytes) != 0 ||
                            std::memcmp(indirect_file.data(), raw_indirect.data(), bytes) != 0)
                            throw std::runtime_error("requested raw layer diagnostics must retain exact complete GPU channels");
                    }
                }
                render(); check_raw(false);
                pipeline->change_resolution(make_uint2(17u, 9u)); render(); check_raw(false);
                pipeline->set_output_denoise(false); render(); check_raw(true);
                pipeline->change_resolution(make_uint2(19u, 11u)); render(); check_raw(true);
                pipeline->set_output_denoise(true); render(); check_raw(false);
                integrator->set_stable_planes_enabled(false); render(); check_raw(false);
                integrator->set_stable_planes_enabled(true); render(); check_raw(false);
                std::cout << "PASS: actual ReSTIR raw allocation ON/OFF/debug/resize/stable-plane lifecycle, variant="
                          << variant << (final_output_only ? " batch\n" : " retained\n");
            }
        }
    }
    std::cout << "PASS: available data paths across " << (selected_scenario == InvalidUI32 ? 5u : 1u)
              << " scenes preserve exact ReBLUR inputs; batched final RT matches retained core byte-for-byte; evidence="
              << directory.string() << '\n';
}

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
      gi_backend{"VISION_RESTIR_GI_BACKEND", nullptr},
      precise_lobes{"VISION_RESTIR_PRECISE_LOBES", "1"},
      single_debug_frame{"VISION_EVAL_DEBUG_FRAMES", nullptr},
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
              "direct":{"open":false,"max_recursion":1,"M_bsdf":1,"temporal":{"open":false},"spatial":{"open":false}},
              "indirect":{"open":true,"backend":"rtxpt","temporal":{"open":false},"spatial":{"open":false}}}}},
          "pipeline":{"type":"fixed","param":{"frame_buffer":{"type":"normal","param":{"resolution":[13,9]}}}},
          "output":{"spp":0,"save_exit":false,"denoise":true,"fn":"unused.exr"}
        })");
        if (scenario == 0u || scenario == 2u) {
            scene["scene"]["shapes"].push_back(DataWrap::parse(
                R"({"type":"model","param":{"fn":"bounce.obj","material":"receiver","normalize_to_unit_bounds":false}})"));
        }
        if (scenario == 0u) {
            // Let GI select real illuminated endpoints, then scale the lobe
            // radiance below FP16 precision at Pack. Distance stays geometric.
            scene["render"]["integrator"]["param"]["indirect"]["factor"] = 1e-15f;
        }
        if (scenario == 1u) {
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
        const bool combined = scenario == 1u || scenario >= 3u;
        scene["render"]["integrator"]["param"]["direct"]["open"] = combined;
        std::ofstream(directory / ("scene-" + std::to_string(scenario) + ".json")) << scene.dump(2);
        std::vector<float4> reference_rgb, reference_lobes;
        const auto baseline = directory / (std::to_string(scenario) + "-staged");
        for (uint variant = 0; variant < (combined ? 3u : 4u); ++variant) {
            const bool fused = variant != 0u, compact = variant >= 2u, batch = variant == 3u;
            const auto capture = directory / (std::to_string(scenario) +
                (batch ? "-batch" : (compact ? "-compact" : (fused ? "-fused" : "-staged"))));
            Environment mode{"REBLUR_FUSE_GUIDES", fused ? "1" : "0"};
            Environment guide_cache{"REBLUR_COMPACT_GUIDE_CACHE", compact ? "1" : "0"};
            Environment batching{"REBLUR_BATCH_COMPOSE", batch ? "1" : "0"};
            Environment composition{"REBLUR_COMPOSE_IN_UNPACK", nullptr};
            Environment snapshot{"REBLUR_EDSL_SNAPSHOT_DIR", capture.string().c_str()};
            Environment debug_root{"VISION_EVAL_DEBUG_DIR", combined ? capture.string().c_str() : nullptr};
            Environment debug_frame{"VISION_EVAL_DEBUG_FRAME", combined ? "2" : nullptr};
            Environment full_diagnostics{"VISION_GI_GUIDE_REUSE_FULL_READBACK", combined ? "1" : nullptr};
            Environment physical_snapshot{"VISION_GI_NRD_SNAPSHOT_COPY", combined ? "1" : nullptr};
            Global::instance().set_scene_path(directory);
            auto pipeline = Corona::Tests::make_vision_test_pipeline(scene, directory);
            auto *integrator = dynamic_cast<IlluminationIntegrator *>(pipeline->renderer().integrator().get());
            if (!integrator) throw std::runtime_error("guide fusion fixture needs the illumination integrator");
            integrator->set_denoise_enabled(true);
            pipeline->frame_buffer()->set_enable_accumulation(false);
            pipeline->prepare();
            pipeline->frame_buffer()->prepare_view_texture();
            if (const char *reason = integrator->render_incompatibility()) throw std::runtime_error(reason);
            for (uint frame = 0; frame < 3u; ++frame) {
                if (frame == 2u) {
                    pipeline->scene().sensor()->set_yaw(.2f);
                    pipeline->scene().sensor()->update_device_data();
                }
                pipeline->upload_data();
                pipeline->display(1.0 / 60.0);
            }
            const uint count = pipeline->pixel_num();
            std::vector<float4> rgb(count);
            pipeline->stream() << pipeline->frame_buffer()->rt_buffer().view().download(rgb.data()) << synchronize() << commit();
            for (const auto &v : rgb) for (uint c = 0; c < 3u; ++c)
                if (!std::isfinite(v[c])) throw std::runtime_error("guide fusion produced non-finite radiance");
            if (!variant) reference_rgb = rgb;
            else if (std::memcmp(reference_rgb.data(), rgb.data(), count * sizeof(float4)) != 0)
                throw std::runtime_error("the guide pack variant changed final FP32 radiance");
            if (!combined && !batch) {
                auto &fb = *pipeline->frame_buffer();
                std::vector<float4> lobes(2u * count);
                pipeline->stream() << fb.denoiser_layer_view(fb.denoiser_diffuse(), 0u).download(lobes.data())
                    << fb.denoiser_layer_view(fb.denoiser_specular(), 0u).download(lobes.data() + count)
                    << synchronize() << commit();
                if (!variant) reference_lobes = lobes;
                else if (std::memcmp(lobes.data(), reference_lobes.data(), lobes.size() * sizeof(float4)))
                    throw std::runtime_error("the guide pack variant changed retained lobe radiance or hit distances");
            }
            auto compare = [&](const fs::path &relative) {
                auto packed = read_bytes(capture / relative);
                if (packed.empty() || (variant && packed != read_bytes(baseline / relative)))
                    throw std::runtime_error("the guide pack variant changed " + relative.string());
                return packed;
            };
            const uint layers = combined ? 3u : 1u;
            for (uint layer = 0; layer < layers; ++layer) for (uint slot = 0; slot < 6u; ++slot) {
                const auto name = "f2_l" + std::to_string(layer) + "_p0_u" + std::to_string(slot) + ".bin";
                const auto packed = compare(name);
                if (combined && layer == 0u) compare(fs::path{"physical-di"} / name);
                if (scenario == 0u && slot < 2u) {
                    const auto magnitude = [&](size_t offset) {
                        const uint lo = static_cast<unsigned char>(packed[offset]);
                        const uint hi = static_cast<unsigned char>(packed[offset + 1u]);
                        return ((hi << 8u) | lo) & 0x7fffu;
                    };
                    bool sampled_black_hit = false;
                    for (size_t offset = 0; offset + 7u < packed.size(); offset += 8u) {
                        const auto distance = magnitude(offset + 6u);
                        sampled_black_hit |= magnitude(offset) == 0u && magnitude(offset + 2u) == 0u &&
                            magnitude(offset + 4u) == 0u && distance > 0u && distance < 0x7c00u;
                    }
                    if (!sampled_black_hit)
                        throw std::runtime_error("packed black diffuse and specular samples must retain geometric distances");
                }
            }
            const auto metadata = read_bytes(capture / "metadata.jsonl");
            std::istringstream lines{std::string(metadata.begin(), metadata.end())};
            std::array<uint, 3> pack_count{};
            for (std::string line; std::getline(lines, line);) {
                const auto record = DataWrap::parse(line);
                if (record.at("frame") != 2u || record.at("pass") != 0u || record.at("slot") != 0u) continue;
                const uint layer = record.at("layer").get<uint>();
                if (layer >= layers) throw std::runtime_error("invalid ReBLUR layer in pack metadata");
                const auto shader = record.at("shader").get<std::string>();
                if (fused != shader.starts_with("Pack-FusedGuides") ||
                    batch != (shader.find("BatchCompose") != std::string::npos))
                    throw std::runtime_error("the requested ReBLUR pack kernel did not execute");
                ++pack_count[layer];
            }
            for (uint layer = 0; layer < layers; ++layer)
                if (pack_count[layer] != ((combined && layer == 0u) ? 2u : 1u))
                    throw std::runtime_error("unexpected physical DI / private GI pack dispatch count");
            if (combined) {
                for (const auto *stage_name : {"producer", "filtered"}) {
                    const auto prefix = std::string{"native_reblur_samples_"} + stage_name;
                    const auto signals = compare(prefix + ".signals.f32");
                    const auto guides = compare(prefix + ".guides.bin");
                    if (signals.size() != 10u * count * sizeof(float4) || guides.size() != 3u * count * sizeof(reblur::Guide))
                        throw std::runtime_error("full physical DI and three private GI signals or guides are incomplete");
                    if (std::string_view(stage_name) != "filtered") continue;
                    std::vector<float4> values(10u * count);
                    std::memcpy(values.data(), signals.data(), signals.size());
                    bool lit_gi = false;
                    for (uint i = 0; i < count; ++i) for (uint c = 0; c < 3u; ++c) {
                        double sum = 0;
                        for (uint channel = 0; channel < 10u; ++channel) {
                            const auto value = values[channel * count + i][c];
                            sum += value;
                            if (channel >= 3u && channel <= 8u && value > 0.f) lit_gi = true;
                        }
                        if (!std::isfinite(sum) || std::abs(sum - rgb[i][c]) > 1e-5 * std::max(1., std::abs(sum)))
                            throw std::runtime_error("the filtered physical DI and private GI layers must sum to the final pixel");
                    }
                    if (scenario >= 3u && !lit_gi)
                        throw std::runtime_error("the lit second surface must produce measurable GI in the textured fixture");
                }
            }
        }
    }
    std::cout << "PASS: ReBLUR pack and filtered output across selected RTXPT GI scenarios; evidence=" << directory.string() << '\n';
}

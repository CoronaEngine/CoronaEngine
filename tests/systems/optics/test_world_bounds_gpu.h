#pragma once

// Exercise production geometry/light refresh with a deterministic CUDA consumer.
// The probe replaces only the image integrator's compilation, not the scene,
// light sampler, encoded buffers, bindless tables, or geometry update paths.
class WorldBoundsProbePipeline final : public vision::Pipeline {
    std::unordered_map<const vision::Renderer*, vision::Shader<void(vision::Buffer<vision::float4>)>> probes_;
public:
    explicit WorldBoundsProbePipeline(const vision::PipelineDesc& desc) : Pipeline(desc) {}
    vision::string_view impl_type() const noexcept override { return "WorldBoundsProbePipeline"; }
    vision::string_view category() const noexcept override { return "pipeline"; }
    void init_postprocessor(const vision::DenoiserDesc&) override {}
    void render(double) noexcept override {}
    unsigned compilations{};
    void compile() noexcept override {
        using namespace vision;
        activate_global_context();
        Global::SceneGpuContextScope scope{scene().geometry().bindless_array(), device()};
        Kernel kernel = [&](BufferVar<float4> output) {
            auto& sampler = renderer().light_sampler();
            LightSampleContext ref{make_float3(0.f), make_float3(0, 1, 0), make_float3(0, 1, 0)};
            sampler->for_each([&](TLight light, uint index) {
                if (light->match(LightType::Area)) {
                    auto [type_id, inst_id] = sampler->extract_light_id(Uint{index});
                    sampler->dispatch_light(type_id, inst_id, [&](const Light* decoded) {
                        output.write(1u, make_float4(decoded->PMF(0u), decoded->PMF(1u), 0.f, 0.f));
                    });
                }
                if (!light->match(LightType::DeltaDirection)) return;
                auto [type_id, inst_id] = sampler->extract_light_id(Uint{index});
                sampler->dispatch_light(type_id, inst_id, [&](const Light* decoded) {
                    auto ctx = decoded->compute_light_eval_context(ref, LightSurfacePoint{});
                    output.write(0u, make_float4(ctx.pos, sampler->PMF(ref, Uint{index})));
                });
            });
        };
        probes_[&renderer()] = device().compile(kernel, "world_bounds_light_probe");
        ++compilations;
    }
    void add_probe_view(uint64_t id) {
        using namespace vision;
        auto context = make_unique<ViewContext>();
        context->renderer.set_frame_buffer(Node::create_shared<FrameBuffer>(frame_buffer_desc_));
        view_contexts_.emplace(id, std::move(context));
        activate_view_context(id);
        renderer().pre_init(renderer_desc_);
        renderer().init(renderer_desc_, scene());
        frame_buffer()->set_enable_accumulation(false);
        activate_view_context(0u);
    }
    vision::float4 read_probe(uint64_t view = 0, unsigned element = 0) {
        using namespace vision;
        activate_view_context(view);
        auto result = device().create_buffer<float4>(2, "world_bounds_result");
        float4 host[2]{};
        stream() << probes_.at(&renderer())(result).dispatch(1u) << result.download(host)
                 << synchronize() << commit();
        return host[element];
    }
};

inline vision::Device& world_bounds_test_device() {
    using namespace vision;
    // Compiled Vision kernels use process-wide printer/debugger resources.
    // Keep their owning CUDA device alive across both probe fixtures.
    static auto device = [] {
        RHIContext::instance().init(std::filesystem::current_path());
        auto device = RHIContext::instance().create_device("cuda");
        device.init_rtx();
        return device;
    }();
    Global::instance().set_device(&device);
    return device;
}

inline void world_bounds_gpu_regressions() {
    using namespace vision;
    world_bounds_test_device();
    auto desc = make_empty_project_desc();
    desc.renderer_desc.light_sampler_desc.sub_type = "power";
    desc.renderer_desc.render_setting.min_world_radius = 1.f;
    for (const auto* type : {"directional", "point"}) {
        LightDesc light;
        light.init(ParameterSet{DataWrap::parse(
            std::string{"{\"type\":\""} + type + "\",\"param\":{\"direction\":[0,-1,0],\"position\":[0,0,0]}}")});
        desc.scene_desc.light_descs.push_back(light);
    }
    auto pipeline = std::make_shared<WorldBoundsProbePipeline>(desc.pipeline_desc);
    pipeline->initialize_(desc.pipeline_desc);
    pipeline->init_project(desc);
    pipeline->frame_buffer()->set_enable_accumulation(false);
    auto mesh = std::make_shared<Mesh>(
        vector<Vertex>{Vertex{make_float3(-1, 0, 0), make_float3(0, 0, 1), make_float2(0)},
                       Vertex{make_float3(1, 0, 0), make_float3(0, 0, 1), make_float2(0)},
                       Vertex{make_float3(0, 2, 0), make_float3(0, 0, 1), make_float2(0)}},
        vector<Triangle>{Triangle{0, 1, 2}});
    ShapeInstance instance{mesh};
    instance.set_o2w(make_float4x4(1.f));
    auto group = std::make_shared<ShapeGroup>(std::move(instance));
    pipeline->scene().add_shape(group);
    pipeline->scene().register_instance_meshes();
    pipeline->scene().tidy_up();
    pipeline->scene().fill_instances();
    pipeline->prepare_geometry();
    pipeline->renderer().prepare_lights(pipeline->scene());
    pipeline->upload_scene_bindless_array();
    pipeline->upload_bindless_array();
    pipeline->compile();
    auto verify = [&](float radius, float3 center) {
        auto actual = pipeline->read_probe();
        // Equal colors: directional power = pi*r^2; point power = 4*pi.
        float pmf = radius * radius / (radius * radius + 4.f);
        std::cout << "WorldBounds probe expected_radius=" << radius << " actual=("
                  << actual.x << ',' << actual.y << ',' << actual.z << ") pmf="
                  << actual.w << " expected_pmf=" << pmf << std::endl;
        expect(std::abs(actual.y - radius) < 1e-4f && actual.x == 0.f && actual.z == 0.f,
               "CUDA directional-light sample must use current world radius");
        expect(std::abs(actual.w - pmf) < 1e-4f,
               "CUDA power sampler PMF must use current world radius");
        expect(all(pipeline->scene().world_center() == center), "CPU world center must match transformed triangle");
        for (auto light : pipeline->scene().light_manager().lights()) {
            if (!light->match(LightType::DeltaDirection)) continue;
            auto& encoded = pipeline->scene().light_manager().lights().get_datas(light.get());
            vector<buffer_ty> uploaded(encoded.host_buffer().size());
            pipeline->stream() << encoded.device_buffer().download(uploaded.data()) << synchronize() << commit();
            // DirectionalLight's encoded tail is radius followed by center xyz.
            const auto tail = uploaded.size() - 4;
            expect(std::abs(ocarina::bit_cast<float>(uploaded[tail]) - radius) < 1e-4f,
                   "uploaded directional radius must match CPU scene");
            for (uint axis = 0; axis < 3; ++axis) {
                expect(ocarina::bit_cast<float>(encoded.host_buffer()[tail + 1 + axis]) == center[axis] &&
                       ocarina::bit_cast<float>(uploaded[tail + 1 + axis]) == center[axis],
                       "host encoding and CUDA directional center must match current bounds");
            }
        }
    };
    verify(std::sqrt(2.f), make_float3(0, 1, 0));
    auto initial_slots = pipeline->bindless_array().buffer_num();
    group->instance(0).set_o2w(scale<H>(make_float3(20.f)));
    group->instance(0).init_aabb();
    group->aabb = group->instance(0).aabb;
    pipeline->update_geometry();
    verify(std::sqrt(800.f), make_float3(0, 20, 0));
    group->instance(0).set_o2w(make_float4x4(1.f));
    group->instance(0).init_aabb();
    group->aabb = group->instance(0).aabb;
    pipeline->update_geometry();
    verify(std::sqrt(2.f), make_float3(0, 1, 0));
    expect(pipeline->bindless_array().buffer_num() == initial_slots,
           "bounds refresh must reuse light-sampling bindless slots");
    auto compiles = pipeline->compilations;
    pipeline->update_geometry();
    expect(pipeline->compilations == compiles, "unchanged bounds must not recompile lights");
    group->instance(0).set_o2w(scale<H>(make_float3(0.f)));
    group->instance(0).init_aabb();
    group->aabb = group->instance(0).aabb;
    pipeline->update_geometry();
    verify(1.f, make_float3(0));
    pipeline->scene().remove_shape(0);
    pipeline->rebuild_geometry_gpu();
    verify(1.f, make_float3(0));
    group->instance(0).set_o2w(make_float4x4(1.f));
    group->instance(0).init_aabb();
    group->aabb = group->instance(0).aabb;
    pipeline->scene().add_shape(group);
    pipeline->rebuild_geometry_gpu();
    verify(std::sqrt(2.f), make_float3(0, 1, 0));
    pipeline->stream() << synchronize() << commit();
    std::cout << "WorldBounds CUDA radius and power-PMF regressions passed\n";
}

inline void world_bounds_multiview_area_regression() {
    using namespace vision;
    world_bounds_test_device();
    auto desc = make_empty_project_desc();
    desc.renderer_desc.light_sampler_desc.sub_type = "power";
    desc.renderer_desc.render_setting.polymorphic_mode = EInstance;
    desc.renderer_desc.render_setting.min_world_radius = 1.f;
    for (const auto* type : {"directional", "area", "spherical"}) {
        LightDesc light;
        light.init(ParameterSet{DataWrap::parse(
            std::string{"{\"type\":\""} + type + "\",\"param\":{\"direction\":[0,-1,0]}}")});
        desc.scene_desc.light_descs.push_back(light);
    }
    auto pipeline = std::make_shared<WorldBoundsProbePipeline>(desc.pipeline_desc);
    pipeline->initialize_(desc.pipeline_desc);
    pipeline->init_project(desc);
    pipeline->frame_buffer()->set_enable_accumulation(false);
    pipeline->add_probe_view(7);
    auto group = std::make_shared<ShapeGroup>();
    group->aabb = Box3f(make_float3(-2.f), make_float3(2.f));
    pipeline->scene().add_shape(group);
    pipeline->scene().register_instance_meshes();
    pipeline->scene().tidy_up();
    pipeline->scene().fill_instances();
    pipeline->prepare_geometry();
    auto initial_slots = pipeline->bindless_array().buffer_num();
    for (unsigned step = 0; step < 8; ++step) {
        const float extent = step % 2 ? 2.f : 4.f;
        group->aabb = Box3f(make_float3(-extent), make_float3(extent));
        pipeline->update_geometry();
        for (auto view : {0u, 7u}) {
            auto direction = pipeline->read_probe(view);
            expect(std::abs(direction.y - std::sqrt(3.f) * extent) < 1e-4f,
                   "every view must consume changed EInstance directional constants");
            auto area = pipeline->read_probe(view, 1);
            expect(std::abs(area.x - .5f) < 1e-6f && std::abs(area.y - .5f) < 1e-6f,
                   "every view must consume live area-light sampling buffers");
        }
        expect(pipeline->bindless_array().buffer_num() == initial_slots,
               "repeated multiview refresh must not leak area/environment/power slots");
    }
    std::cout << "WorldBounds two-view EInstance area/environment lifecycle regressions passed\n";
}

//
// Created by GitHub Copilot on 2026/4/4.
//

#include "base/scattering/material.h"
#include "base/scattering/interaction.h"
#include "base/import/node_desc.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "base/color/spectrum.h"
#include "base/sampler.h"
#include "math/util.h"
#include <iostream>

using namespace vision;
using namespace ocarina;

namespace {

constexpr float kLooseEps = 8e-2f;
constexpr uint kSampleCount = 8192u;
constexpr uint kMaterialCount = 8u;
constexpr uint DiffuseIndex = 0u;
constexpr uint MirrorIndex = 1u;
constexpr uint GlassIndex = 2u;
constexpr uint MetalIndex = 3u;
constexpr uint MetallicIndex = 4u;
constexpr uint PlasticIndex = 5u;
constexpr uint SubstrateIndex = 6u;
constexpr uint PrincipledIndex = 7u;

int g_pass = 0;
int g_fail = 0;

void expect(bool cond, const char *message) {
    if (!cond) {
        std::cerr << "[test-material-energy] FAIL: " << message << std::endl;
        ++g_fail;
    } else {
        ++g_pass;
    }
}

SampledWavelengths make_test_swl() {
    SampledWavelengths swl(3u, 1u);
    swl.set_lambda(0u, 602.785f);
    swl.set_lambda(1u, 539.285f);
    swl.set_lambda(2u, 445.772f);
    swl.set_pdf(0u, 1.f / 3.f);
    swl.set_pdf(1u, 1.f / 3.f);
    swl.set_pdf(2u, 1.f / 3.f);
    return swl;
}

PartialDerivative<Float3> make_identity_frame() {
    PartialDerivative<Float3> frame;
    frame.x = make_float3(1.f, 0.f, 0.f);
    frame.y = make_float3(0.f, 1.f, 0.f);
    frame.z = make_float3(0.f, 0.f, 1.f);
    return frame;
}

class TestPipeline final : public Pipeline {
public:
    explicit TestPipeline(const PipelineDesc &desc)
        : Pipeline(desc) {}

    [[nodiscard]] string_view impl_type() const noexcept override { return "test_pipeline"; }
    [[nodiscard]] string_view category() const noexcept override { return "pipeline"; }

    void init_postprocessor(const DenoiserDesc &desc) override {}
    void render(double dt) noexcept override {}
};

class TestSampler final : public Sampler {
private:
    optional<Uint> state_{};

public:
    using Sampler::Sampler;

    [[nodiscard]] string_view impl_type() const noexcept override { return "test_sampler"; }
    [[nodiscard]] string_view category() const noexcept override { return "sampler"; }

    void load_data() noexcept override {
        state_.emplace(Uint{0u});
    }

    void set_seed(const Uint2 &pixel, const Uint &sample_index, const Uint &dim) noexcept override {
        Uint state = tea<D>(tea<D>(pixel.x, pixel.y), tea<D>(sample_index, dim));
        try_load_data();
        state_ = state;
    }

    void temporary(const ocarina::function<void(Sampler *)> &func) noexcept override {
        try_load_data();
        Uint temp_state = *state_;
        func(this);
        *state_ = temp_state;
    }

    [[nodiscard]] bool is_valid() const noexcept override {
        return state_ && state_->is_valid();
    }

    [[nodiscard]] Float next_1d() noexcept override {
        return lcg<D>(*state_);
    }
};

SP<Material> create_material(const char *json) {
    MaterialDesc desc;
    desc.init(ParameterSet(DataWrap::parse(json)));
    auto material = Material::create_root(desc);
    material->prepare();
    return material;
}

void install_srgb_spectrum(Pipeline &pipeline) {
    SpectrumDesc desc("Spectrum");
    desc.sub_type = "srgb";
    auto spectrum = Node::create_shared<Spectrum>(desc);
    pipeline.renderer().set_spectrum(spectrum);
    pipeline.renderer().spectrum()->prepare();
}

void test_material_energy(Device &device,
                          const SP<Material> &diffuse,
                          const SP<Material> &mirror,
                          const SP<Material> &glass,
                          const SP<Material> &metal,
                          const SP<Material> &metallic,
                          const SP<Material> &plastic,
                          const SP<Material> &substrate,
                          const SP<Material> &principled) {
    std::cout << "[test-material-energy] Running test_material_energy ..." << std::endl;
    Stream stream = device.create_stream();
    Buffer<float3> results = device.create_buffer<float3>(kMaterialCount, "material_energy");

    Kernel kernel = [&](BufferVar<float3> out) {
        SampledWavelengths swl = make_test_swl();
        auto frame = make_identity_frame();
        Float3 world_wo = normalize(make_float3(0.6f, 0.f, 0.8f));

        auto make_interaction = [&] {
            Interaction it(false);
            it.pos = make_float3(0.f);
            it.wo = world_wo;
            it.ng = frame.normal();
            it.ng_local = frame.normal();
            it.shading = frame;
            return it;
        };

        auto build_evaluator = [&](const SP<Material> &material) {
            Interaction it = make_interaction();
            return material->create_evaluator(it, swl);
        };

        auto integrate_material = [&](const SP<Material> &material, Uint seed) {
            TSampler sampler{make_shared<TestSampler>()};
            sampler->load_data();
            sampler->set_seed(make_uint2(seed, 0u), 0u, 0u);
            MaterialEvaluator evaluator = build_evaluator(material);
            SampledSpectrum ret = SampledSpectrum::zero(swl);
            $for(i, kSampleCount) {
                BSDFSample bs = evaluator.sample(world_wo, sampler);
                ScatterEval se = bs.eval;
                $if(se.pdf() > 0.f) {
                    ret += se.safe_throughput();
                };
            };
            return ret / kSampleCount;
        };

        auto material_albedo = [&](const SP<Material> &material) {
            MaterialEvaluator evaluator = build_evaluator(material);
            return evaluator.albedo(world_wo);
        };

        out.write(DiffuseIndex, integrate_material(diffuse, DiffuseIndex).vec3());
        out.write(MirrorIndex, material_albedo(mirror).vec3());
        out.write(GlassIndex, material_albedo(glass).vec3());
        out.write(MetalIndex, material_albedo(metal).vec3());
        out.write(MetallicIndex, material_albedo(metallic).vec3());
        out.write(PlasticIndex, integrate_material(plastic, PlasticIndex).vec3());
        out.write(SubstrateIndex, integrate_material(substrate, SubstrateIndex).vec3());
        out.write(PrincipledIndex, material_albedo(principled).vec3());
    };

    auto shader = device.compile(kernel, "test_material_energy");
    vector<float3> host(kMaterialCount, make_float3(0.f));
    stream << shader(results).dispatch(1u);
    stream << results.download(host.data());
    stream << synchronize() << commit();

    const char *names[kMaterialCount] = {
        "Diffuse", "Mirror", "Glass", "Metal",
        "Metallic", "Plastic", "Substrate", "PrincipledBSDF"
    };

    for (uint i = 0u; i < kMaterialCount; ++i) {
        std::cout << "[test-material-energy] Testing " << names[i]
                  << " with energy [" << host[i].x << ", " << host[i].y << ", " << host[i].z << "]"
                  << std::endl;
        int fail_before = g_fail;
        expect(host[i].x >= 0.f && host[i].y >= 0.f && host[i].z >= 0.f,
               (std::string(names[i]) + " integral >= 0").c_str());
        expect(host[i].x <= 1.f + kLooseEps,
               (std::string(names[i]) + " integral R <= 1").c_str());
        expect(host[i].y <= 1.f + kLooseEps,
               (std::string(names[i]) + " integral G <= 1").c_str());
        expect(host[i].z <= 1.f + kLooseEps,
               (std::string(names[i]) + " integral B <= 1").c_str());
        std::cout << "[test-material-energy] Result " << names[i] << ": "
                  << (g_fail == fail_before ? "PASS" : "FAIL") << std::endl;
    }

    expect(host[GlassIndex].x > 0.f, "Glass total energy should be positive");
    expect(host[MetalIndex].x > 0.f, "Metal total energy should be positive");
    expect(host[PlasticIndex].x > 0.f, "Plastic total energy should be positive");
    expect(host[PrincipledIndex].x > 0.f, "Principled total energy should be positive");
}

}// namespace

int main(int argc, char *argv[]) {
    auto device = RHIContext::instance().create_device();
    Global::instance().set_device(&device);

    PipelineDesc desc;
    auto pipeline = make_shared<TestPipeline>(desc);
    Global::instance().set_pipeline(pipeline);
    install_srgb_spectrum(*pipeline);

    auto diffuse = create_material(R"json(
{
    "type": "diffuse",
    "name": "MatDiffuse",
    "param": {
        "color": {"channels": "xyz", "node": {"type": "number", "param": {"value": [1.0, 1.0, 1.0]}}},
        "sigma": {"channels": "x", "node": {"type": "number", "param": {"value": 0.0}}}
    },
    "node_tab": {}
}
)json");

    auto mirror = create_material(R"json(
{
    "type": "mirror",
    "name": "MatMirror",
    "param": {
        "color": {"channels": "xyz", "node": {"type": "number", "param": {"value": [1.0, 1.0, 1.0]}}},
        "roughness": {"channels": "x", "node": {"type": "number", "param": {"value": 0.14}}},
        "anisotropic": {"channels": "x", "node": {"type": "number", "param": {"value": 0.0}}},
        "remapping_roughness": false
    },
    "node_tab": {}
}
)json");

    auto glass = create_material(R"json(
{
    "type": "glass",
    "name": "MatGlass",
    "param": {
        "color": {"channels": "xyz", "node": {"type": "number", "param": {"value": [1.0, 1.0, 1.0]}}},
        "ior": {"channels": "x", "node": {"type": "number", "param": {"value": 1.5}}},
        "roughness": {"channels": "x", "node": {"type": "number", "param": {"value": 0.14}}},
        "anisotropic": {"channels": "x", "node": {"type": "number", "param": {"value": 0.0}}},
        "remapping_roughness": false
    },
    "node_tab": {}
}
)json");

    auto metal = create_material(R"json(
{
    "type": "metal",
    "name": "MatMetal",
    "param": {
        "material_name": "Cu",
        "roughness": {"channels": "x", "node": {"type": "number", "param": {"value": 0.14}}},
        "anisotropic": {"channels": "x", "node": {"type": "number", "param": {"value": 0.0}}},
        "remapping_roughness": false
    },
    "node_tab": {}
}
)json");

    auto metallic = create_material(R"json(
{
    "type": "metallic",
    "name": "MatMetallic",
    "param": {
        "color": {"channels": "xyz", "node": {"type": "number", "param": {"value": [1.0, 1.0, 1.0]}}},
        "edge_tint": {"channels": "xyz", "node": {"type": "number", "param": {"value": [0.9, 0.85, 0.8]}}},
        "roughness": {"channels": "x", "node": {"type": "number", "param": {"value": 0.14}}},
        "anisotropic": {"channels": "x", "node": {"type": "number", "param": {"value": 0.0}}},
        "remapping_roughness": false
    },
    "node_tab": {}
}
)json");

    auto plastic = create_material(R"json(
{
    "type": "plastic",
    "name": "MatPlastic",
    "param": {
        "color": {"channels": "xyz", "node": {"type": "number", "param": {"value": [1.0, 1.0, 1.0]}}},
        "ior": {"channels": "x", "node": {"type": "number", "param": {"value": 1.5}}},
        "roughness": {"channels": "x", "node": {"type": "number", "param": {"value": 0.12}}},
        "anisotropic": {"channels": "x", "node": {"type": "number", "param": {"value": 0.0}}},
        "sigma_a": {"channels": "xyz", "node": {"type": "number", "param": {"value": [0.0, 0.0, 0.0]}}},
        "thickness": {"channels": "x", "node": {"type": "number", "param": {"value": 1.0}}},
        "remapping_roughness": true
    },
    "node_tab": {}
}
)json");

    auto substrate = create_material(R"json(
{
    "type": "substrate",
    "name": "MatSubstrate",
    "param": {
        "color": {"channels": "xyz", "node": {"type": "number", "param": {"value": [1.0, 1.0, 1.0]}}},
        "spec": {"channels": "xyz", "node": {"type": "number", "param": {"value": [0.05, 0.05, 0.05]}}},
        "roughness": {"channels": "x", "node": {"type": "number", "param": {"value": 0.5}}},
        "anisotropic": {"channels": "x", "node": {"type": "number", "param": {"value": 0.0}}},
        "remapping_roughness": false
    },
    "node_tab": {}
}
)json");

    auto principled = create_material(R"json(
{
    "type": "principled_bsdf",
    "name": "MatPrincipledBSDF",
    "param": {
        "color": {"channels": "xyz", "node": {"type": "number", "param": {"value": [1.0, 1.0, 1.0]}}},
        "metallic": {"channels": "x", "node": {"type": "number", "param": {"value": 0.0}}},
        "ior": {"channels": "x", "node": {"type": "number", "param": {"value": 1.5}}},
        "roughness": {"channels": "x", "node": {"type": "number", "param": {"value": 0.3}}},
        "spec_tint": {"channels": "xyz", "node": {"type": "number", "param": {"value": [1.0, 1.0, 1.0]}}},
        "anisotropic": {"channels": "x", "node": {"type": "number", "param": {"value": 0.0}}},
        "opacity": {"channels": "x", "node": {"type": "number", "param": {"value": 1.0}}},
        "sheen_weight": {"channels": "x", "node": {"type": "number", "param": {"value": 0.0}}},
        "sheen_roughness": {"channels": "x", "node": {"type": "number", "param": {"value": 0.5}}},
        "sheen_tint": {"channels": "xyz", "node": {"type": "number", "param": {"value": [1.0, 1.0, 1.0]}}},
        "coat_weight": {"channels": "x", "node": {"type": "number", "param": {"value": 0.0}}},
        "coat_roughness": {"channels": "x", "node": {"type": "number", "param": {"value": 0.2}}},
        "coat_ior": {"channels": "x", "node": {"type": "number", "param": {"value": 1.5}}},
        "coat_tint": {"channels": "xyz", "node": {"type": "number", "param": {"value": [1.0, 1.0, 1.0]}}},
        "subsurface_weight": {"channels": "x", "node": {"type": "number", "param": {"value": 0.0}}},
        "subsurface_radius": {"channels": "xyz", "node": {"type": "number", "param": {"value": [1.0, 1.0, 1.0]}}},
        "subsurface_scale": {"channels": "x", "node": {"type": "number", "param": {"value": 0.2}}},
        "transmission_weight": {"channels": "x", "node": {"type": "number", "param": {"value": 0.0}}}
    },
    "node_tab": {}
}
)json");

    test_material_energy(device, diffuse, mirror, glass, metal, metallic, plastic, substrate, principled);

    std::cout << "\n[test-material-energy] " << g_pass << " passed, "
              << g_fail << " failed." << std::endl;
    return g_fail > 0 ? 1 : 0;
}
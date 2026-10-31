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
#include <array>
#include <cmath>
#include <iostream>

using namespace vision;
using namespace ocarina;

namespace {

constexpr float kLooseEps = 8e-2f;
constexpr uint kSampleCount = 8192u;
constexpr uint kMaterialCount = 8u;

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

void test_material_energy(Device &device, const std::array<SP<Material>, kMaterialCount> &materials) {
    Stream stream = device.create_stream();
    Buffer<float3> results = device.create_buffer<float3>(kMaterialCount, "material_energy");
    Kernel kernel = [&](BufferVar<float3> out) {
        SampledWavelengths swl = make_test_swl();
        auto frame = make_identity_frame();
        Float3 wo = normalize(make_float3(0.6f, 0.f, 0.8f));
        for (uint i = 0; i < kMaterialCount; ++i) {
            Interaction it(false);
            it.pos = make_float3(0.f); it.wo = wo;
            it.ng = frame.normal(); it.ng_local = frame.normal(); it.shading = frame;
            auto evaluator = materials[i]->create_evaluator(it, swl);
            TSampler sampler{make_shared<TestSampler>()};
            sampler->load_data();
            sampler->set_seed(make_uint2(i, 0u), 0u, 0u);
            SampledSpectrum energy = SampledSpectrum::zero(swl);
            $for(sample_index, kSampleCount) {
                auto sample = evaluator.sample(wo, sampler);
                $if(sample.eval.pdf() > 0.f) { energy += sample.eval.safe_throughput(); };
            };
            out.write(i, (energy / kSampleCount).vec3());
        }
    };
    auto shader = device.compile(kernel, "test_material_energy");
    std::array<float3, kMaterialCount> host{};
    stream << shader(results).dispatch(1u) << results.download(host.data()) << synchronize() << commit();
    const char *names[]{"BlackBody", "Opaque", "SmoothMetal", "RoughMetal",
                        "Transmission", "Coat", "Sheen", "MixedMetal"};
    for (uint i = 0; i < kMaterialCount; ++i) {
        std::cout << names[i] << " sampled energy=" << host[i].x << ',' << host[i].y << ',' << host[i].z << '\n';
        for (uint channel = 0; channel < 3; ++channel) {
            expect(std::isfinite(host[i][channel]), "sampled material energy must be finite");
            expect(host[i][channel] >= 0.f, "sampled material energy must be non-negative");
            expect(host[i][channel] <= 1.f + kLooseEps, "sampled material energy must not exceed one");
        }
        if (i == 0u) {
            expect(host[i].x == 0.f && host[i].y == 0.f && host[i].z == 0.f,
                   "black_body must absorb all incident energy");
        } else {
            expect(host[i].x > 0.f, "each Principled configuration must scatter energy");
        }
    }
}

}// namespace

int main() {
    auto device = RHIContext::instance().create_device();
    Global::instance().set_device(&device);
    PipelineDesc desc;
    auto pipeline = make_shared<TestPipeline>(desc);
    Global::instance().set_pipeline(pipeline);
    install_srgb_spectrum(*pipeline);

    const char *fixtures[]{
        R"({"type":"black_body","param":{}})",
        R"({"type":"principled_bsdf","param":{"color":[0.7,0.6,0.5],"roughness":0.5}})",
        R"({"type":"principled_bsdf","param":{"color":[0.8,0.7,0.6],"metallic":1,"roughness":0.001}})",
        R"({"type":"principled_bsdf","param":{"color":[0.8,0.7,0.6],"metallic":1,"roughness":0.5}})",
        R"({"type":"principled_bsdf","param":{"color":[1,1,1],"transmission_weight":1,"ior":1.5,"roughness":0.4}})",
        R"({"type":"principled_bsdf","param":{"color":[0.7,0.6,0.5],"coat_weight":1,"roughness":0.4}})",
        R"({"type":"principled_bsdf","param":{"color":[0.7,0.6,0.5],"sheen_weight":0.5,"roughness":0.4}})",
        R"({"type":"principled_bsdf","param":{"color":[0.7,0.6,0.5],"metallic":0.5,"roughness":0.4}})",
    };
    static_assert(std::size(fixtures) == kMaterialCount);
    std::array<SP<Material>, kMaterialCount> materials;
    for (uint i = 0; i < kMaterialCount; ++i) materials[i] = create_material(fixtures[i]);
    test_material_energy(device, materials);
    std::cout << "[test-material-energy] " << g_pass << " passed, " << g_fail << " failed.\n";
    return g_fail == 0 ? 0 : 1;
}

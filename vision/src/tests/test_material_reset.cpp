#include "base/color/spectrum.h"
#include "base/import/node_desc.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "base/scattering/interaction.h"
#include "base/scattering/material.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace vision;
using namespace ocarina;

namespace {

int g_pass = 0;
int g_fail = 0;

class TestPipeline final : public Pipeline {
public:
    explicit TestPipeline(const PipelineDesc &desc)
        : Pipeline(desc) {}

    [[nodiscard]] string_view impl_type() const noexcept override { return "test_pipeline"; }
    [[nodiscard]] string_view category() const noexcept override { return "pipeline"; }
    void init_postprocessor(const DenoiserDesc &) override {}
    void render(double) noexcept override {}
};

void expect(bool cond, const char *message) {
    if (!cond) {
        std::cerr << "[test-material-reset] FAIL: " << message << std::endl;
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

void install_srgb_spectrum(Pipeline &pipeline) {
    SpectrumDesc desc("Spectrum");
    desc.sub_type = "srgb";
    auto spectrum = Node::create_shared<Spectrum>(desc);
    pipeline.renderer().set_spectrum(spectrum);
    pipeline.renderer().spectrum()->prepare();
}

SP<Material> create_material(const char *json) {
    MaterialDesc desc;
    desc.init(ParameterSet(DataWrap::parse(json)));
    auto material = Material::create_root(desc);
    material->prepare();
    return material;
}

constexpr const char *kDiffuseJson = R"json(
{
    "type": "diffuse",
    "name": "MatDiffuse",
    "param": {
        "color": {"channels": "xyz", "node": {"type": "number", "param": {"value": [1.0, 1.0, 1.0]}}},
        "sigma": {"channels": "x", "node": {"type": "number", "param": {"value": 0.0}}}
    },
    "node_tab": {}
}
)json";

constexpr const char *kPrincipledJson = R"json(
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
)json";

float3 evaluate_material(Device &device, PolymorphicGUI<SP<Material>> &material_set, uint material_id) {
    Stream stream = device.create_stream();
    Buffer<float3> buffer = device.create_buffer<float3>(1u, "test_material_reset_result");

    Kernel kernel = [&](BufferVar<float3> out, Uint encoded_id) {
        SampledWavelengths swl = make_test_swl();
        auto frame = make_identity_frame();
        Float3 wo = normalize(make_float3(0.3f, 0.2f, 0.93f));
        Float3 wi = normalize(make_float3(0.15f, 0.1f, 0.98f));

        Interaction it(false);
        it.pos = make_float3(0.f);
        it.wo = wo;
        it.ng = frame.normal();
        it.ng_local = frame.normal();
        it.shading = frame;

        material_set.dispatch(encoded_id, [&](const Material *material) {
            MaterialEvaluator evaluator = material->create_evaluator(it, swl);
            ScatterEval eval = evaluator.evaluate(wo, wi, MaterialEvalMode::F, BxDFFlag::All, TransportMode::Radiance);
            out.write(0u, eval.f.vec3());
        });
    };

    auto shader = device.compile(kernel, "test_material_reset_eval");
    float3 host = make_float3(0.f);
    stream << shader(buffer, material_id).dispatch(1u);
    stream << buffer.download(&host);
    stream << synchronize() << commit();
    return host;
}

bool is_finite_vec(float3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

}// namespace

int main() {
    auto device = RHIContext::instance().create_device();
    Global::instance().set_device(&device);

    PipelineDesc desc;
    auto pipeline = make_shared<TestPipeline>(desc);
    Global::instance().set_pipeline(pipeline);
    install_srgb_spectrum(*pipeline);

    auto &material_set = pipeline->scene().materials();
    material_set.clear();
    material_set.set_mode(PolymorphicMode::ETopology);

    auto diffuse = create_material(kDiffuseJson);
    auto principled = create_material(kPrincipledJson);
    pipeline->scene().add_material(diffuse);
    pipeline->scene().add_material(principled);
    pipeline->scene().prepare_materials();
    pipeline->upload_scene_bindless_array();
    pipeline->upload_bindless_array();

    uint before_id = material_set.encode_id(0u, principled.get());
    float3 before = evaluate_material(device, material_set, before_id);
    expect(is_finite_vec(before), "before replace evaluation must be finite");

    auto replacement = create_material(kPrincipledJson);
    replacement->set_index(1u);
    bool replaced = material_set.replace(1, replacement);
    expect(replaced, "replace must succeed");
    pipeline->scene().prepare_materials();
    pipeline->upload_scene_bindless_array();
    pipeline->upload_bindless_array();

    uint after_id = material_set.encode_id(0u, replacement.get());
    float3 after = evaluate_material(device, material_set, after_id);
    expect(is_finite_vec(after), "after replace evaluation must be finite");
    expect(after.x >= 0.f && after.y >= 0.f && after.z >= 0.f,
           "after replace evaluation must stay non-negative");

    std::cout << "[test-material-reset] before = ["
              << before.x << ", " << before.y << ", " << before.z << "]\n";
    std::cout << "[test-material-reset] after  = ["
              << after.x << ", " << after.y << ", " << after.z << "]\n";
    std::cout << "[test-material-reset] pass=" << g_pass << " fail=" << g_fail << std::endl;
    return g_fail == 0 ? 0 : 1;
}

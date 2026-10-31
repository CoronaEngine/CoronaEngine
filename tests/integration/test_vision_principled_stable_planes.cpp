#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "base/sampler.h"
#include "base/scattering/material.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

// Exercise the real material graph: zero-weight sheen, coat, metal, specular
// and diffuse children must not hide a pure Principled transmission lobe.
void check_principled_stable_planes(vision::Pipeline &pipeline) {
    using namespace vision;
    pipeline.activate_global_context();
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    auto &sampler = pipeline.renderer().sampler();
    const auto original_normal_mode = MaterialRegistry::instance().individual_ns();
    struct RestoreNormalMode {
        bool value;
        ~RestoreNormalMode() { MaterialRegistry::instance().individual_ns() = value; }
    } restore{original_normal_mode};

    enum class Kind { Glass, Metal, Fallback };
    enum class Tint { White, Colored, Black };
    struct Fixture {
        const char *name;
        Kind kind;
        float metallic, transmission, roughness, sheen, coat;
        bool mapped{false};
        Tint tint{Tint::White};
    };
    const Fixture fixtures[] = {
        {"pure-glass", Kind::Glass, 0.f, 1.f, 0.f, 0.f, 0.f},
        {"pure-metal", Kind::Metal, 1.f, 0.f, 0.f, 0.f, 0.f},
        {"rough-glass", Kind::Fallback, 0.f, 1.f, .3f, 0.f, 0.f},
        {"rough-metal", Kind::Fallback, 1.f, 0.f, .3f, 0.f, 0.f},
        {"diffuse-mixture", Kind::Fallback, 0.f, 0.f, 0.f, 0.f, 0.f},
        {"near-pure-glass-mixture", Kind::Fallback, 0.f, .999f, 0.f, 0.f, 0.f},
        {"near-pure-metal-mixture", Kind::Fallback, .999f, 0.f, 0.f, 0.f, 0.f},
        {"sheen-mixture", Kind::Fallback, 0.f, 1.f, 0.f, .2f, 0.f},
        {"coat-mixture", Kind::Fallback, 0.f, 1.f, 0.f, 0.f, .2f},
        {"mapped-glass", Kind::Glass, 0.f, 1.f, 0.f, 0.f, 0.f, true},
        {"tinted-glass", Kind::Glass, 0.f, 1.f, 0.f, 0.f, 0.f, false, Tint::Colored},
        {"black-transmission-glass", Kind::Glass, 0.f, 1.f, 0.f, 0.f, 0.f, false, Tint::Black},
    };

    for (const auto &fixture : fixtures) {
        auto description = DataWrap::parse(R"({"type":"principled_bsdf","param":{
            "color":[1,1,1],"ior":1.5,"spec_tint":[1,1,1],
            "metallic":0,"transmission_weight":1,"roughness":0,
            "sheen_weight":0,"coat_weight":0,"coat_roughness":0}})");
        auto &params = description["param"];
        params["metallic"] = fixture.metallic;
        params["transmission_weight"] = fixture.transmission;
        params["roughness"] = fixture.roughness;
        params["sheen_weight"] = fixture.sheen;
        params["coat_weight"] = fixture.coat;
        const auto tint = fixture.tint == Tint::Colored ? make_float3(.25f, 0.f, .8f) :
                          make_float3(fixture.tint == Tint::Black ? 0.f : 1.f);
        params["color"] = DataWrap::array({tint.x, tint.y, tint.z});
        if (fixture.mapped) {
            params["normal"] = DataWrap::parse(R"({"node":"test_normal","channels":"xyz"})");
            description["node_tab"] = DataWrap::parse(R"({
                "test_normal":{"type":"number","param":{"value":[0.6,0,0.8]}}})");
        }
        MaterialDesc desc;
        desc.init(ParameterSet{description});
        auto material = Material::create_root(desc);
        material->prepare();
        pipeline.geometry().upload_bindless_array(pipeline.stream());
        pipeline.upload_data();

        // Both normal conventions must use the same existing delta frame.
        for (bool individual_normals : {false, true}) {
            MaterialRegistry::instance().individual_ns() = individual_normals;
            constexpr uint samples = 4096u;
            Kernel kernel = [&](BufferVar<float4> output) {
                Env::instance().clear_global_vars();
                sampler->load_data();
                Uint id = dispatch_id();
                Uint scenario = id % 5u;
                SampledWavelengths swl{3u};
                Interaction it{false};
                it.shading.x = make_float3(1.f, 0.f, 0.f);
                it.shading.y = make_float3(0.f, 1.f, 0.f);
                it.shading.z = make_float3(0.f, 0.f, 1.f);
                Float x = select(scenario == 0u || scenario == 4u, 0.f, .5f);
                x = select(scenario == 3u, .9f, x);
                Float z = sqrt(1.f - x * x) * select(scenario == 2u || scenario == 3u, -1.f, 1.f);
                Float3 local_wo = make_float3(x, Float{0.f}, z);
                $if(scenario == 4u) {
                    it.shading.x = make_float3(0.f, 1.f, 0.f);
                    it.shading.y = make_float3(0.f, 0.f, 1.f);
                    it.shading.z = make_float3(1.f, 0.f, 0.f);
                };
                if (fixture.kind != Kind::Glass || fixture.mapped) {
                    // Opaque Principled is one-sided; inside/TIR cases belong
                    // to the transmissive fixture, not the metal fallback.
                    local_wo = make_float3(0.f, 0.f, 1.f);
                }
                auto initial_frame = it.shading;
                it.ng = it.shading.z;
                it.wo = initial_frame.to_world(local_wo);
                // Keep the input frame before normal mapping mutates it.
                auto reference_it = it;
                auto evaluator = material->create_evaluator(it, swl);
                sampler->set_seed(dispatch_idx().xy(), Uint{791u}, Dimension::PathTracing);
                Float expected_rng = sampler->next_1d();
                sampler->set_seed(dispatch_idx().xy(), Uint{791u}, Dimension::PathTracing);
                auto stable = evaluator.eval_stable_lobes(it.wo);
                auto repeated = evaluator.eval_stable_lobes(it.wo);
                Float actual_rng = sampler->next_1d();
                Bool ok = actual_rng == expected_rng;
                Float stage = select(ok, 0.f, 1.f);
                auto record_stage = [&](float value) { stage = select(stage == 0.f && !ok, value, stage); };
                for (uint branch = 0; branch < 2u; ++branch) {
                    auto &b = stable.lobes[branch];
                    auto &r = repeated.lobes[branch];
                    ok &= b.valid == r.valid && b.probability == r.probability &&
                          all(b.wi == r.wi) && all(b.weight.vec3() == r.weight.vec3());
                }
                record_stage(2.f);
                Uint selected = InvalidUI32;
                if (fixture.kind == Kind::Fallback) {
                    ok &= stable.has_stochastic_remainder && !stable.lobes[0].valid && !stable.lobes[1].valid;
                    ok &= evaluator.flag() != SurfaceData::NearSpec;
                    record_stage(3.f);
                } else {
                    ok &= !stable.has_stochastic_remainder && evaluator.flag() == SurfaceData::NearSpec;
                    ok &= stable.lobes[1].valid && !stable.lobes[1].transmission;
                    record_stage(4.f);
                    auto bs = evaluator.sample_delta(it.wo, sampler);
                    selected = bs.stable_lobe_index;
                    ok &= bs.valid() && selected < 2u;
                    for (uint branch = 0; branch < 2u; ++branch) {
                        auto &b = stable.lobes[branch];
                        $if(selected == branch) {
                            Float3 expected_weight = bs.eval.throughput().vec3() * b.probability;
                            ok &= b.valid && length(b.wi - bs.wi) < 1e-5f &&
                                  all(abs(b.weight.vec3() - expected_weight) < 3e-5f * max(abs(expected_weight), make_float3(1.f)));
                        };
                    }
                    record_stage(5.f);
                    if (fixture.mapped) {
                        // n=(.6,0,.8), wo=(0,0,1): independently computed
                        // reflection and Snell refraction for eta=1.5.
                        ok &= stable.lobes[0].valid && stable.lobes[0].transmission;
                        ok &= length(stable.lobes[1].wi - initial_frame.to_world(Float3{make_float3(.96f, 0.f, .28f)})) < 1e-5f;
                        ok &= length(stable.lobes[0].wi - initial_frame.to_world(Float3{make_float3(-.22990908f, 0.f, -.97321212f)})) < 1e-5f;
                    } else {
                        ok &= length(stable.lobes[1].wi - initial_frame.to_world(make_float3(-local_wo.xy(), local_wo.z))) < 1e-5f;
                        if (fixture.kind == Kind::Metal) {
                            ok &= !stable.lobes[0].valid && stable.lobes[1].probability == 1.f && selected == 1u;
                            ok &= all(abs(stable.lobes[1].weight.vec3() - make_float3(1.f)) < .003f);
                        } else {
                            ok &= abs(stable.lobes[0].probability + stable.lobes[1].probability - 1.f) < 1e-6f;
                            if (fixture.tint == Tint::Black) {
                                ok &= !stable.lobes[0].valid && stable.lobes[0].probability == 0.f &&
                                      all(stable.lobes[0].weight.vec3() == make_float3(0.f)) && selected == 1u;
                            }
                            $if(scenario == 0u || scenario == 4u) {
                                // F0=((1.5-1)/(1.5+1))^2=.04. Radiance
                                // transmission includes eta^-2: .96/2.25.
                                const float tint_average = (tint.x + tint.y + tint.z) / 3.f;
                                const float reflected_probability = .04f / (.04f + .96f * tint_average);
                                ok &= stable.lobes[0].valid == (fixture.tint != Tint::Black) && stable.lobes[0].transmission;
                                ok &= abs(stable.lobes[1].probability - reflected_probability) < 1e-5f;
                                ok &= all(abs(stable.lobes[1].weight.vec3() - make_float3(.04f)) < .003f);
                                ok &= all(abs(stable.lobes[0].weight.vec3() - Float3{tint * .42666667f}) < .003f);
                                if (fixture.tint != Tint::Black)
                                    ok &= length(stable.lobes[0].wi - initial_frame.to_world(Float3{make_float3(0.f, 0.f, -1.f)})) < 1e-5f;
                            };
                            if (fixture.tint != Tint::Black) {
                                $if(scenario == 1u) {
                                    ok &= stable.lobes[0].valid && length(stable.lobes[0].wi - make_float3(-1.f / 3.f, 0.f, -.94280904f)) < 1e-5f;
                                };
                                $if(scenario == 2u) {
                                    ok &= stable.lobes[0].valid && length(stable.lobes[0].wi - make_float3(-.75f, 0.f, .66143783f)) < 1e-5f;
                                };
                            }
                            $if(scenario == 3u) {
                                ok &= !stable.lobes[0].valid && stable.lobes[0].probability == 0.f &&
                                      all(stable.lobes[0].weight.vec3() == make_float3(0.f)) &&
                                      stable.lobes[1].probability == 1.f && selected == 1u;
                            };
                        }
                    }
                    record_stage(6.f);
                }
                // Rebuild the actual Principled children independently, then
                // strip only the stable adapter. Parent links now belong to
                // this plain reference, never the evaluator under test.
                $if(scenario == 0u || scenario == 1u || scenario == 4u) {
                    auto reference_root = material->create_lobe_set(reference_it, swl);
                    LobeSet::Lobes children;
                    static_cast<const LobeSet *>(reference_root.get())->for_each([&](const WeightedLobe &child) {
                        children.push_back(child);
                    });
                    auto plain_lobes = make_unique<LobeSet>(std::move(children));
                    MaterialEvaluator plain{reference_it, swl};
                    plain.link(std::move(plain_lobes));
                    auto same_eval = [&](const ScatterEval &a, const ScatterEval &b) {
                        return a.flags == b.flags &&
                               abs(a.pdf() - b.pdf()) <= 2e-5f * max(abs(b.pdf()), 1.f) &&
                               all(abs(a.f.vec3() - b.f.vec3()) <= 2e-5f * max(abs(b.f.vec3()), make_float3(1.f)));
                    };
                    for (float side : {-1.f, 1.f}) {
                        Float3 wi = initial_frame.to_world(normalize(Float3{make_float3(.2f, .1f, side)}));
                        auto ordinary = evaluator.evaluate(it.wo, wi);
                        auto reference = plain.evaluate(it.wo, wi);
                        ok &= same_eval(ordinary, reference);
                    }
                    record_stage(7.f);
                    sampler->set_seed(dispatch_idx().xy(), Uint{379u}, Dimension::PathTracing);
                    auto ordinary_sample = evaluator.sample(it.wo, sampler);
                    Float ordinary_next = sampler->next_1d();
                    sampler->set_seed(dispatch_idx().xy(), Uint{379u}, Dimension::PathTracing);
                    auto reference_sample = plain.sample(it.wo, sampler);
                    Float reference_next = sampler->next_1d();
                    ok &= ordinary_next == reference_next && ordinary_sample.valid() == reference_sample.valid() &&
                          ordinary_sample.stable_lobe_index == InvalidUI32 && reference_sample.stable_lobe_index == InvalidUI32;
                    $if(ordinary_sample.valid() && reference_sample.valid()) {
                        ok &= length(ordinary_sample.wi - reference_sample.wi) < 1e-5f &&
                              abs(ordinary_sample.eta - reference_sample.eta) < 1e-6f &&
                              same_eval(ordinary_sample.eval, reference_sample.eval);
                    };
                    record_stage(8.f);
                };
                output.write(id, make_float4(select(ok, 1.f, -stage), cast<float>(selected),
                                             stable.lobes[0].weight[0], stable.lobes[1].weight[0]));
            };
            auto shader = pipeline.device().compile(kernel, "principled_stable_plane_contract");
            auto buffer = pipeline.device().create_buffer<float4>(samples, "principled_stable_plane_results");
            std::vector<float4> values(samples);
            pipeline.stream() << shader(buffer).dispatch(samples) << buffer.download(values.data()) << synchronize() << commit();
            uint reflected = 0u, transmitted = 0u;
            for (uint i = 0; i < samples; ++i) {
                const auto &v = values[i];
                if (v.x != 1.f || !std::isfinite(v.z) || !std::isfinite(v.w)) {
                    std::cout << "Principled fixture=" << fixture.name << " individual_normals=" << individual_normals
                              << " sample=" << i << " stage=" << v.x << " branch=" << v.y
                              << " weights=" << v.z << ',' << v.w << '\n';
                    throw std::runtime_error("Principled stable planes must preserve classification, transport, branch identity and delta sampling");
                }
                if (i % 5u == 0u) { reflected += v.y == 1.f; transmitted += v.y == 0.f; }
            }
            if (fixture.kind == Kind::Glass && !fixture.mapped && fixture.tint != Tint::Black && (reflected < 8u || transmitted < 500u))
                throw std::runtime_error("pure Principled glass fixture must sample reflection and transmission");
        }
        std::cout << "PASS: Principled stable-plane material " << fixture.name << '\n';
    }
}

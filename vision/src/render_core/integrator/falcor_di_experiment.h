#pragma once

#include "ReSTIR/direct.h"
#include "base/color/spectrum.h"
#include "falcor_di/common.h"
#include "falcor_di/candidates.h"
#include "falcor_di/reuse.h"
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

namespace vision {

// Static RGB, pinhole, area-light ReSTIR DI experiment.
class FalcorDIExperiment final : private Toolkit {
    static constexpr uint has_history_bit = 1u;
    static constexpr uint same_geometry_bit = 2u;
    uint2 extent_{};
    uint light_count_{};
    uint previous_frame_{InvalidUI32};
    WP<const GeometryGpuResource> previous_geometry_;
    uint64_t previous_geometry_revision_{};
    std::string mode_;
    bool publish_gi_{};
    Buffer<uint4> surfaces0_;
    Buffer<uint2> surface_normals_;
    Buffer<uint2> ris_;
    Buffer<float2> neighbors_;
    Buffer<falcor_di::Reservoir> reservoirs_;
    Buffer<uint4> light_primitives_;
    Buffer<falcor_di::LightTriangleGeometry> light_geometry_;
    Buffer<float> probabilities_;
    Buffer<uint> light_bases_;
    Shader<void()> prepare_light_geometry_;
    Shader<void(uint)> prepare_surface_, presample_;
    Shader<void(uint, uint)> candidates_, shade_;
    Shader<void(uint, uint3, uint)> reuse_;

    template<typename T>
    [[nodiscard]] static Buffer<T> uploaded(const vector<T> &values, const char *name) {
        auto result = device().create_buffer<T>(values.size(), name);
        result.upload_immediately(values.data());
        return result;
    }

    void prepare_lights() {
        const auto &instances = scene().instances();
        vector<uint> bases(instances.size(), InvalidUI32);
        vector<uint4> primitives;
        vector<float> probabilities;
        const auto *environment = scene().light_manager().env_light();
        if (environment && (environment->impl_type() != "spherical" || any(environment->average() != make_float3(0.f))))
            throw std::runtime_error("Falcor DI experiment requires an unlit environment");
        for (uint i = 0; i < instances.size(); ++i) {
            const auto &shape = instances[i];
            if (!shape->has_emission()) continue;
            const auto *light = shape->emission().get();
            if (light->impl_type() != "area" || light->source_desc()["two_sided"].as_bool(false) || !light->color_ ||
                (light->color_->impl_type() != "number" && light->color_->impl_type() != "constant"))
                throw std::runtime_error("Falcor DI experiment requires constant one-sided area lights");
            const auto areas = shape->surface_areas();
            const auto emission_luminance = luminance(light->average());
            bases[i] = static_cast<uint>(probabilities.size());
            for (uint prim_id = 0; prim_id < areas.size(); ++prim_id) {
                primitives.push_back(make_uint4(i, prim_id, shape->handle().light_id, 0u));
                probabilities.push_back(areas[prim_id] * emission_luminance);
            }
        }
        float power = 0.f;
        for (const auto value : probabilities) power += value;
        if (!(power > 0.f) || !std::isfinite(power))
            throw std::runtime_error("Falcor DI experiment found no finite light power");
        for (auto &value : probabilities) value /= power;
        light_count_ = static_cast<uint>(probabilities.size());
        if (light_count_ != 8u) throw std::runtime_error("Falcor DI experiment uses the eight-triangle Kitchen light PDF");
        light_primitives_ = uploaded(primitives, "Falcor DI emitter primitive IDs");
        light_geometry_ = device().create_buffer<falcor_di::LightTriangleGeometry>(
            light_count_, "Falcor DI emitter geometry cache");
        probabilities_ = uploaded(probabilities, "Falcor DI light probabilities");
        light_bases_ = uploaded(bases, "Falcor DI emitter offsets");
    }

    void prepare_buffers() {
        extent_ = pipeline()->resolution();
        const auto pixels = extent_.x * extent_.y;
        const auto pitch = ((extent_.x + 15u) / 16u) * ((extent_.y + 15u) / 16u) * 256u;
        surfaces0_ = device().create_buffer<uint4>(2u * pixels, "Falcor DI surface position and depth");
        surface_normals_ = device().create_buffer<uint2>(2u * pixels, "Falcor DI surface normals");
        reservoirs_ = device().create_buffer<falcor_di::Reservoir>(3u * pitch, "Falcor DI packed reservoir");
        ris_ = device().create_buffer<uint2>(falcor_di::tile_count * falcor_di::tile_size, "Falcor DI RIS tiles");
        vector<float2> offsets;
        offsets.reserve(falcor_di::neighbor_count);
        constexpr float step = 1.f / 1.3247179572447f;
        float u = 0.5f, v = 0.5f;
        while (offsets.size() < falcor_di::neighbor_count) {
            u += step;
            v += step * step;
            if (u >= 1.f) u -= 1.f;
            if (v >= 1.f) v -= 1.f;
            const float x = u - 0.5f, y = v - 0.5f;
            if (x * x + y * y > 0.25f) continue;
            offsets.push_back(make_float2(float(int(x * 250.f)), float(int(y * 250.f))) / 127.f);
        }
        neighbors_ = uploaded(offsets, "Falcor DI neighbor offsets");
    }

    static Uint encode_normal(const Float3 &normal) {
        const Float3 n = normal / max(abs(normal.x) + abs(normal.y) + abs(normal.z), 1e-20f);
        const Float2 sign = make_float2(select(n.x < 0.f, -1.f, 1.f), select(n.y < 0.f, -1.f, 1.f));
        const Float2 oct = select(n.z < 0.f, (1.f - abs(n.yx())) * sign, n.xy());
        const Int2 snorm = cast<int2>(round(clamp(oct, -1.f, 1.f) * 32767.f));
        const Uint2 bits = as<uint2>(snorm);
        return (bits.x & 65535u) | (bits.y << 16u);
    }

    void compile(ReSTIRDI &direct, uint bias_mode) {
        const uint pixels = extent_.x * extent_.y;
        auto &camera = scene().sensor();
        auto &geo = geometry();
        auto &sampler = renderer().sampler();
        auto &spec = spectrum();
        auto &fb = frame_buffer();
        auto &materials = scene().materials();
        auto &lights = renderer().light_sampler();
        Kernel prepare_light_geometry = [&] {
            const Uint index = dispatch_id();
            const Uint4 primitive = light_primitives_.read(index, false);
            const auto p0 = geo.compute_light_eval_context(primitive.x, primitive.y, make_float2(1.f, 0.f));
            const auto p1 = geo.compute_light_eval_context(primitive.x, primitive.y, make_float2(0.f, 1.f));
            const auto p2 = geo.compute_light_eval_context(primitive.x, primitive.y, make_float2(0.f));
            falcor_di::LightTriangleGeometryVar triangle;
            triangle.p0.set(p0.pos);
            triangle.p1.set(p1.pos);
            triangle.p2.set(p2.pos);
            triangle.ng.set(p0.ng);
            // The authored normal is interpolated by the framework. When the
            // corner hemispheres disagree, let it resolve the sampled point.
            const Bool cacheable = p0.PDF_pos > 0.f && falcor_di::finite(p0.PDF_pos) &&
                                   all(p0.ng == p1.ng) && all(p0.ng == p2.ng);
            triangle.pdf_pos = select(cacheable, p0.PDF_pos, 0.f);
            // This route accepts only constant one-sided emitters in RGB.
            const auto swl = spec->sample_wavelength(sampler);
            Float3 emission = make_float3(0.f);
            lights->dispatch_light(primitive.z, [&](const Light *light) {
                if (!light->match(LightType::Area)) return;
                emission = light->color_.eval_illumination_spectrum(p0.uv, swl).sample.vec3() * light->scale();
            });
            triangle.emission.set(emission);
            light_geometry_.write(index, triangle, false);
        };
        prepare_light_geometry.function()->set_block_dim(32u);
        prepare_light_geometry_ = device().compile(prepare_light_geometry, "Falcor DI light geometry preparation");

        Kernel prepare = [&](Uint frame) {
            const Uint index = dispatch_id();
            const Uint offset = (frame & 1u) * pixels + index;
            auto hit = fb.cur_visibility_buffer_var(frame).read(index);
            Float4 position_depth = make_float4(0.f, 0.f, 0.f, -1.f);
            Uint2 normals = make_uint2(0u);
            camera->load_data();
            const auto swl = spec->sample_wavelength(sampler);
            SurfaceDataVar gi_surface;
            if (publish_gi_) gi_surface.hit = hit;
            $if(hit->is_hit()) {
                auto ray = fb.rays().read(index)->to_ray_state().ray;
                auto it = geo.compute_surface_interaction(hit, ray);
                materials.dispatch(it.material_id(), [&](const Material *material) {
                    if (publish_gi_) {
                        Interaction guide_it = it;
                        guide_it.wo = guide_it.ng;
                        auto guide = material->create_evaluator(guide_it, swl);
                        set_reuse_material_guides(gi_surface, guide, *spec, swl);
                    }
                    auto bsdf = material->create_evaluator(it, swl);
                    position_depth = make_float4(it.pos, length(it.pos - camera->device_position()));
                    normals = make_uint2(encode_normal(bsdf.shading_frame().normal()), encode_normal(it.ng));
                    if (publish_gi_) {
                        gi_surface.flag = bsdf.flag();
                        gi_surface->set_diffuse_factor(bsdf.diffuse_factor());
                        gi_surface->set_position(it.pos);
                        gi_surface->set_normal(it.shading.normal());
                        gi_surface->set_depth(camera->linear_depth(ray->at(ray->t_max())));
                    }
                });
            };
            if (publish_gi_) fb.cur_surfaces_var(frame).write(index, gi_surface);
            surfaces0_.write(offset, as<uint4>(position_depth));
            surface_normals_.write(offset, normals);
        };
        prepare.function()->set_block_dim(16u, 16u);
        prepare_surface_ = device().compile(prepare, "Falcor DI surface preparation");

        const auto context = [&](const Uint &frame, const Bool &has_history) {
            return falcor_di::Context{extent_, frame, has_history, light_count_, surfaces0_, surface_normals_,
                                     reservoirs_, neighbors_, light_primitives_, light_geometry_,
                                     probabilities_, light_bases_, ris_};
        };
        Kernel presample = [&](Uint frame) {
            sampler->load_data();
            falcor_di::presample_light(context(frame, Bool{false}), dispatch_idx().xy(), sampler);
        };
        presample.function()->set_block_dim(256u, 1u);
        presample_ = device().compile(presample, "Falcor DI light presampling");

        Kernel candidates = [&](Uint frame, Uint slice) {
            Function::current()->require_ray_query_compute_workgroups();
            const auto ctx = context(frame, Bool{false});
            const Uint2 pixel = dispatch_idx().xy();
            const auto surface = falcor_di::load_surface(ctx, make_int2(pixel), false);
            sampler->load_data();
            sampler->set_seed(pixel, frame, Dimension::ReSTIR_RIS);
            const auto swl = spec->sample_wavelength(sampler);
            falcor_di::ReservoirVar reservoir;
            HitBSDFVar gi_proposal;
            $if(falcor_di::valid_surface(surface)) {
                const Uint index = dispatch_id();
                const auto hit = fb.cur_visibility_buffer_var(frame).read(index);
                auto ray = fb.rays().read(index)->to_ray_state().ray;
                auto it = geo.compute_surface_interaction(hit, ray);
                materials.dispatch(it.material_id(), [&](const Material *material) {
                    const auto bsdf = material->create_evaluator(it, swl);
                    reservoir = falcor_di::sample_initial(ctx, pixel, surface, bsdf, it.wo, sampler,
                                                          publish_gi_ ? addressof(gi_proposal) : nullptr);
                });
            };
            falcor_di::store_reservoir(ctx, pixel, slice, reservoir);
            if (publish_gi_) fb.hit_bsdfs().write(dispatch_id(), gi_proposal);
        };
        candidates.function()->set_block_dim(16u, 16u);
        candidates_ = device().compile(candidates, "Falcor DI initial candidates");

        const auto reuse_settings = direct.settings();
        const bool use_temporal = reuse_settings["temporal"]["open"].get<bool>();
        const bool use_spatial = reuse_settings["spatial"]["open"].get<bool>();
        OC_INFO_FORMAT("Falcor DI reuse settings: temporal={}, spatial={}", use_temporal, use_spatial);
        Kernel reuse = [&](Uint frame, Uint3 slices, Uint history) {
            Function::current()->require_ray_query_compute_workgroups();
            sampler->load_data();
            const auto ctx = context(frame, (history & has_history_bit) != 0u);
            const Uint2 pixel = dispatch_idx().xy();
            const auto surface = falcor_di::load_surface(ctx, make_int2(pixel), false);
            falcor_di::ReservoirVar reservoir;
            $if(falcor_di::valid_surface(surface)) {
                auto candidate = falcor_di::load_reservoir(ctx, pixel, slices.x);
                $if(falcor_di::valid_reservoir(candidate)) {
                    const auto sample = falcor_di::sample_light(ctx, candidate.light_index, surface, candidate->uv());
                    $if(!falcor_di::visible(surface, sample)) {
                        // Keep the candidate count even when the selected light is blocked.
                        falcor_di::ReservoirVar rejected;
                        rejected.count = candidate.count;
                        candidate = rejected;
                    };
                };
                const auto swl = spec->sample_wavelength(sampler);
                reservoir = falcor_di::reuse_direct(ctx, pixel, surface, candidate, slices.y, swl, bias_mode, sampler,
                                                     use_temporal, use_spatial, (history & same_geometry_bit) != 0u);
            };
            falcor_di::store_reservoir(ctx, pixel, slices.z, reservoir);
        };
        reuse.function()->set_block_dim(16u, 16u);
        reuse_ = device().compile(reuse, "Falcor DI visibility and spatiotemporal reuse");

        Kernel shade = [&](Uint frame, Uint slice) {
            const auto ctx = context(frame, Bool{false});
            const Uint i = dispatch_id();
            const Uint2 pixel = dispatch_idx().xy();
            auto hit = fb.cur_visibility_buffer_var(frame).read(i);
            Float3 value = make_float3(0.f);
            const auto swl = spec->sample_wavelength(sampler);
            $if(hit->is_hit()) {
                auto primary = fb.rays().read(i)->to_ray_state().ray;
                auto it = geo.compute_surface_interaction(hit, primary);
                $if(it.has_emission()) {
                    lights->dispatch_light(it.light_id(), [&](const Light *light) {
                        if (!light->match(LightType::Area)) return;
                        LightSampleContext ref;
                        ref.pos = primary.org_min.xyz();
                        value = light->evaluate_wi(ref, it, swl, LightEvalMode::L).L.vec3();
                    });
                } $else {
                    const auto surface = falcor_di::load_surface(ctx, make_int2(pixel), false);
                    const auto reservoir = falcor_di::load_reservoir(ctx, pixel, slice);
                    $if(falcor_di::valid_surface(surface) && falcor_di::valid_reservoir(reservoir)) {
                        const auto sample = falcor_di::sample_light(ctx, reservoir.light_index, surface, reservoir->uv());
                        const Float3 weighted_radiance = sample.radiance * reservoir.weight;
                        $if(sample.distance > 0.f && any(weighted_radiance > make_float3(0.f))) {
                            auto shadow = it.spawn_ray(sample.direction, max(0.f, sample.distance - 1e-4f));
                            $if(!geo.trace_occlusion(shadow)) {
                                materials.dispatch(it.material_id(), [&](const Material *material) {
                                    auto bsdf = material->create_evaluator(it, swl);
                                    value = bsdf.evaluate(it.wo, sample.direction).f.vec3() * weighted_radiance;
                                });
                            };
                        };
                    };
                };
            };
            direct.write_radiance(i, make_float4(value, 1.f));
        };
        shade.function()->set_block_dim(16u, 16u);
        shade_ = device().compile(shade, "Falcor DI final shading");
    }

public:
    FalcorDIExperiment(ReSTIRDI &direct, std::string_view mode, bool publish_gi)
        : mode_{mode}, publish_gi_{publish_gi} {
        uint bias_mode = 0u;
        if (mode == "basic") bias_mode = 1u;
        else if (mode == "pairwise") bias_mode = 2u;
        else if (mode == "raytraced") bias_mode = 3u;
        else throw std::runtime_error("VISION_FALCOR_DI_MODE must be basic, pairwise, or raytraced");
        prepare_lights();
        prepare_buffers();
        compile(direct, bias_mode);
        OC_INFO_FORMAT("Falcor DI native route active: mode={}, {} triangles, {}x{}", mode,
                       light_count_, extent_.x, extent_.y);
    }

    [[nodiscard]] bool matches(uint2 extent, std::string_view mode, bool publish_gi) const noexcept {
        return extent.x == extent_.x && extent.y == extent_.y && mode == mode_ && publish_gi == publish_gi_;
    }

    [[nodiscard]] CommandBatch dispatch(uint frame) {
        const uint current = 1u + (frame & 1u);
        const uint3 slices = make_uint3(0u, 3u - current, current);
        const bool has_history = previous_frame_ != InvalidUI32 && frame > 0u && previous_frame_ + 1u == frame;
        uint history = has_history ? has_history_bit : 0u;
        if (mode_ == "pairwise") {
            const auto resource = geometry().gpu_resource();
            const uint64_t revision = resource->visibility_revision();
            if (has_history && previous_geometry_.lock() == resource && previous_geometry_revision_ == revision) {
                history |= same_geometry_bit;
            }
            previous_geometry_ = resource;
            previous_geometry_revision_ = revision;
        }
        previous_frame_ = frame;
        CommandBatch batch;
        batch << prepare_light_geometry_().dispatch(light_count_);
        batch << prepare_surface_(frame).dispatch(extent_);
        batch << presample_(frame).dispatch(make_uint2(falcor_di::tile_size, falcor_di::tile_count));
        batch << candidates_(frame, slices.x).dispatch(extent_);
        batch << reuse_(frame, slices, history).dispatch(extent_);
        batch << shade_(frame, slices.z).dispatch(extent_);
        return batch;
    }
};

} // namespace vision

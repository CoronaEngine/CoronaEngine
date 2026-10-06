//
// Created by Zero on 2023/10/29.
//

#include "indirect.h"
#include "base/integral/integrator.h"

namespace vision {
using namespace ocarina;
ReSTIRGI::ReSTIRGI(IntegratorPtr integrator,
                   const vision::ParameterSet &desc)
    : ReSTIR(integrator, desc),
    sample_num_(desc["sample_num"].as_uint(1u)),
    ratio_(desc["ratio"].as_uint(2u)),
    debias_(desc["debias"].as_bool(true)) {
    // Spend the extra reuse work on GI; explicit scene settings take priority.
    spatial_.sample_num = spatial_.open ? desc["spatial"]["sample_num"].as_uint(4u) : 1u;
    spatial_.sampling_radius = desc["spatial"]["radius"].as_float(12.f);
}

Float ReSTIRGI::Jacobian_det(Float3 cur_pos, Float3 neighbor_pos,
                             Var<SurfacePoint> sample_point) const noexcept {
    Float ret = 0.f;
    Float3 cur_vec = cur_pos - sample_point->position();
    Float3 neighbor_vec = neighbor_pos - sample_point->position();
    Float cur_dist2 = length_squared(cur_vec);
    Float neighbor_dist2 = length_squared(neighbor_vec);
    $if(sample_point->valid() && cur_dist2 > 0.f && neighbor_dist2 > 0.f) {
        Float cos_phi_c = abs_dot(normalize(cur_vec), sample_point->normal());
        Float cos_phi_n = abs_dot(normalize(neighbor_vec), sample_point->normal());
        Float denominator = cos_phi_n * cur_dist2;
        $if(denominator > 0.f) {
            ret = (cos_phi_c * neighbor_dist2) / denominator;
            ret = ocarina::max(ocarina::zero_if_nan_inf(ret), 0.f);
        };
    };
    return ret;
}

bool ReSTIRGI::render_UI(Widgets *widgets) noexcept {
    return widgets->use_tree("ReSTIR GI", [&] {
        changed_ |= widgets->check_box("switch", &open_);
        if (open_) {
            render_sub_UI(widgets);
        }
    });
}

void ReSTIRGI::render_sub_UI(Widgets *widgets) noexcept {
    changed_ |= widgets->check_box("temporal", &temporal_.open);
    changed_ |= widgets->drag_uint("max age", &max_age_, 1, 0, 100);
    changed_ |= widgets->drag_float("diffuse factor threshold", &diff_factor_, 0, 1);
    widgets->slider_uint("resolution ratio", &ratio_, 1, 4);
    widgets->button_click("resize", [&] {

    });
    if (temporal_.open) {
        changed_ |= widgets->input_uint_limit("history", &temporal_.limit, 0, 50, 1, 3);
        changed_ |= widgets->input_float_limit("temporal theta",
                                               &temporal_.theta, 0, 90, 1, 1);
        changed_ |= widgets->input_float_limit("temporal depth", &temporal_.depth_threshold,
                                               0, 1, 0.02, 0.1);
        changed_ |= widgets->input_float_limit("temporal radius", &temporal_.sampling_radius,
                                               0, 50, 1, 5);
    }
    changed_ |= widgets->check_box("spatial", &spatial_.open);
    if (spatial_.open) {
        changed_ |= widgets->input_float_limit("spatial theta",
                                               &spatial_.theta, 0, 90, 1, 1);
        changed_ |= widgets->input_float_limit("spatial depth", &spatial_.depth_threshold,
                                               0, 1, 0.02, 0.1);
        changed_ |= widgets->input_float_limit("spatial radius", &spatial_.sampling_radius,
                                               0, 50, 1, 5);
    }
}

GISampleVar ReSTIRGI::init_sample(const Interaction &it, const SensorSample &ss,
                                  HitBSDFVar &hit_bsdf) noexcept {
    Uint2 pixel = dispatch_idx().xy();
    sampler()->set_seed(pixel, frame_index(), 3);
    GISampleVar sample;
    $if(hit_bsdf->valid() && !ocarina::isinf(hit_bsdf.pdf)) {
        Interaction sp_it{false};
        sp_it.pos = make_float3(0.f);
        sp_it.ng = make_float3(0.f);
        HitContext hit_context{sp_it};
        hit_context.suppress_initial_emission = true;
        RayVar ray = it.spawn_ray(hit_bsdf.wi.as_vec3());
        RayState ray_state = RayState::create(ray);
        // Store radiance independent of the source receiver's BSDF/prefix.
        // Dividing a weighted result cannot recover zero throughput channels.
        Float3 L = integrator()->Li(ray_state, hit_bsdf.pdf, spectrum()->one(), hit_context, *this);
        sample.sp->set(sp_it);
        sample.Lo.set(ocarina::zero_if_nan_inf(L));
    };
    return sample;
}

void ReSTIRGI::compile_initial_samples() noexcept {
    switch_profile::Scope profile{"GI.initial.compile", "compile"};
    TSpectrum &spectrum = pipeline()->spectrum();
    TSensor &camera = scene().sensor();
    Kernel kernel = [&](Uint frame_index) {
        initial(sampler(), frame_index, spectrum);
        SurfaceDataVar surf = cur_surfaces().read(dispatch_id());
        $if(surf.hit->is_miss()) {
            radiance_->write(dispatch_id(), make_float4(0.f));
            $return();
        };
        camera->load_data();
        sampler()->load_data();
        integrator()->load_data();
        Uint2 pixel = dispatch_idx().xy();
        sampler()->set_seed(pixel, frame_index, 0);
        SensorSample ss = sampler()->sensor_sample(pixel, camera->filter());
        Float3 view_pos = cur_view_pos(surf.is_replaced);
        Interaction it = pipeline()->geometry().compute_surface_interaction(surf.hit, view_pos);
        HitBSDFVar hit_bsdf = frame_buffer().hit_bsdfs().read(dispatch_id());
        GISampleVar sample = init_sample(it, ss, hit_bsdf);
        samples_.write(dispatch_id(), sample);
    };
    initial_samples_ = device().compile(kernel, "ReSTIR indirect initial samples");
}

ScatterEval ReSTIRGI::eval_bsdf(const Interaction &it, const GISampleVar &sample,
                                MaterialEvalMode mode) const noexcept {
    return outline("ReSTIRGI::eval_bsdf", [&] {
        ScatterEval ret{spectrum()->dimension(), 1};
        scene().materials().dispatch(it.material_id(), [&](const Material *material) {
            MaterialEvaluator bsdf = material->create_evaluator(it, sampled_wavelengths());
            Float3 wi = normalize(sample.sp->position() - it.pos);
            ret = bsdf.evaluate(it.wo, wi, mode);
        });
        return ret;
    });
}

Float ReSTIRGI::compute_p_hat(const vision::Interaction &it,
                              const vision::GISampleVar &sample) const noexcept {
    Float ret = 0.f;
    $if(sample.sp->valid() && luminance(sample.Lo.as_vec3()) > 0.f &&
        length_squared(sample.sp->position() - it.pos) > 0.f) {
        ScatterEval eval = eval_bsdf(it, sample, MaterialEvalMode::All);
        // Shading normals can leave F positive when the geometric hemisphere
        // rejects the PDF. Match PT's receiver support before reusing a sample.
        $if(eval.valid() && !ocarina::isinf(eval.pdf())) {
            ret = ocarina::max(ocarina::zero_if_nan_inf(sample->p_hat(eval.f.vec3())), 0.f);
        };
    };
    return ret;
}

Float ReSTIRGI::selected_sample_support(const Interaction &target_it,
                                         const Interaction &source_it,
                                         const GISampleVar &sample) const noexcept {
    // Receiver support under the cached-Lo approximation; this does not
    // re-evaluate the secondary vertex's view-dependent outgoing radiance.
    Float support = 0.f;
    Float jacobian = Jacobian_det(target_it.pos, source_it.pos, sample.sp);
    $if(jacobian > 0.f) {
        ScatterEval eval = eval_bsdf(source_it, sample, MaterialEvalMode::All);
        Float p_hat = ocarina::zero_if_nan_inf(sample->p_hat(eval.f.vec3()));
        Float pdf = eval.pdf();
        $if(p_hat > 0.f && eval.valid() && !ocarina::isinf(pdf)) {
            support = cast<float>(pipeline()->geometry().visibility(source_it, sample.sp->position()));
        };
    };
    return support;
}

GIReservoirVar ReSTIRGI::combine_temporal(const GIReservoirVar &cur_rsv, SurfaceDataVar cur_surf,
                                          GIReservoirVar &other_rsv, SurfaceDataVar *neighbor_surf,
                                          Float3 view_pos, Float3 prev_view_pos) const noexcept {
    other_rsv.sample.age += 1;
    TSensor &camera = scene().sensor();
    Interaction it = pipeline()->geometry().compute_surface_interaction(cur_surf.hit, view_pos);
    GIReservoirVar ret;
    Float cur_p_hat = compute_p_hat(it, cur_rsv.sample);
    ret->update(sampler()->next_1d(), cur_rsv.sample,
                GIReservoir::safe_weight(cur_rsv.C, cur_p_hat, cur_rsv.W), cur_rsv.C);
    Float other_weight = 0.f;
    Interaction neighbor_it{false};
    if (neighbor_surf) {
        neighbor_it = pipeline()->geometry().compute_surface_interaction(neighbor_surf->hit, prev_view_pos);
        $if(other_rsv.W > 0.f && other_rsv.sample.sp->valid()) {
            Float other_p_hat = compute_p_hat(it, other_rsv.sample);
            Float jacobian = Jacobian_det(it.pos, neighbor_it.pos, other_rsv.sample.sp);
            $if(other_p_hat > 0.f && jacobian > 0.f) {
                Float visibility = pipeline()->geometry().visibility(it, other_rsv.sample.sp->position());
                other_weight = GIReservoir::safe_weight(other_rsv.C, other_p_hat * jacobian, other_rsv.W) * visibility;
            };
        };
    }
    // A null or occluded realization still represents its original stream.
    ret->update(sampler()->next_1d(), other_rsv.sample, other_weight, other_rsv.C);
    Float p_hat = compute_p_hat(it, ret.sample);
    if (debias_) {
        Float normalization = 0.f;
        $if(ret.weight_sum > 0.f && p_hat > 0.f && ret.sample.sp->valid()) {
            // Every positive-weight winner is supported at the current receiver.
            normalization = ocarina::max(ocarina::zero_if_nan_inf(cur_rsv.C), 0.f);
            if (neighbor_surf) {
                Float count = ocarina::max(ocarina::zero_if_nan_inf(other_rsv.C), 0.f);
                $if(count > 0.f) {
                    // Test the final winner, even if the history's own sample is null.
                    normalization += count * selected_sample_support(it, neighbor_it, ret.sample);
                };
            }
        };
        ret->update_W_with_support(p_hat, normalization);
    } else {
        ret->update_W(p_hat);
    }
    return ret;
}

GIReservoirVar ReSTIRGI::temporal_reuse(GIReservoirVar rsv, const SurfaceDataVar &cur_surf,
                                        const Float2 &motion_vec, const SensorSample &ss,
                                        const Var<GIParam> &param) const noexcept {
    Float2 prev_p_film = previous_reservoir_coord(ss.p_film, motion_vec, previous_film_offset(param.camera_jitter));
    Int2 prev_p = reservoir_pixel(prev_p_film);
    Float limit = rsv.C * param.history_limit;
    Int2 res = make_int2(dispatch_dim().xy());
    TSensor &camera = scene().sensor();

    Float3 view_pos = cur_view_pos(cur_surf.is_replaced);
    Float3 prev_view_pos = camera->prev_device_position();

    auto get_prev_data = [this, &limit](const Uint2 &pos, Float3 &view_pos) {
        Uint index = dispatch_id(make_uint2(pos));
        GIReservoirVar prev_rsv = prev_reservoirs().read(index);
        prev_rsv->truncation(limit);
        SurfaceDataVar surf = prev_surfaces().read(index);
        view_pos = scene().sensor()->prev_device_position();
        $if(surf.is_replaced) {
            view_pos = prev_surface_extends().read(index).view_pos;
        };
        return make_pair(surf, prev_rsv);
    };

    $if(in_screen(prev_p, res) && param.temporal) {
        auto data = get_prev_data(make_uint2(prev_p), prev_view_pos);
        auto prev_surf = data.first;
        auto prev_rsv = data.second;

        $if(is_temporal_valid(cur_surf, prev_surf,
                              param, addressof(prev_rsv.sample))) {
            rsv = combine_temporal(rsv, cur_surf, prev_rsv, addressof(prev_surf),
                                   view_pos, prev_view_pos);
        }
        $else {
            $for(i, temporal_.N) {
                Int2 p = reservoir_pixel(square_to_disk(sampler()->next_2d()) * param.t_radius + prev_p_film);
                $if(!in_screen(p, res)) { $continue; };
                auto data = get_prev_data(make_uint2(p), prev_view_pos);
                auto another_surf = data.first;
                auto another_rsv = data.second;
                $if(is_temporal_valid(cur_surf, another_surf,
                                      param, addressof(another_rsv.sample))) {
                    rsv = combine_temporal(rsv, cur_surf, another_rsv, addressof(another_surf),
                                           view_pos, prev_view_pos);
                    $break;
                };
            };
        };
    };
    return rsv;
}

void ReSTIRGI::compile_temporal_reuse() noexcept {
    switch_profile::Scope profile{"GI.temporal.compile", "compile"};
    TSpectrum &spectrum = pipeline()->spectrum();
    TSensor &camera = scene().sensor();
    //todo remedy init samples and reservoir
    Kernel kernel = [&](Var<GIParam> param, Uint frame_index) {
        initial(sampler(), frame_index, spectrum);
        SurfaceDataVar surf = cur_surfaces().read(dispatch_id());
        $if(surf.hit->is_miss()) {
            $return();
        };
        sampler()->load_data();
        camera->load_data();
        Uint2 pixel = dispatch_idx().xy();
        SensorSample ss;
        sampler()->temporary([&](Sampler *sampler) {
            // Film coordinates must agree with the GBuffer motion vector used
            // below; the GI reservoir RNG is separately seeded afterwards.
            sampler->set_seed(make_uint2(0u), frame_index, 0);
            ss = sampler->sensor_sample(pixel, camera->filter(), param.camera_jitter != 0u);
        });
        sampler()->set_seed(pixel, frame_index, 4);
        GISampleVar sample = samples_.read(dispatch_id());
        HitBSDFVar hit_bsdf = frame_buffer().hit_bsdfs().read(dispatch_id());
        GIReservoirVar rsv;
        Float p_hat = sample->p_hat(hit_bsdf.bsdf.as_vec3());
        Float weight = GIReservoir::safe_weight(1, p_hat, 1.f / hit_bsdf.pdf);
        rsv->update(0.5f, sample, weight);
        rsv->update_W(p_hat);
        Float2 motion_vec = frame_buffer().motion_vectors().read(dispatch_id());
        rsv = temporal_reuse(rsv, surf, motion_vec, ss, param);
        passthrough_reservoirs().write(dispatch_id(), rsv);
    };
    temporal_pass_ = device().compile(kernel, "ReSTIR indirect temporal reuse");
}

GIReservoirVar ReSTIRGI::constant_combine(const GIReservoirVar &canonical_rsv,
                                          const Container<uint> &rsv_idx) const noexcept {
    TSensor &camera = scene().sensor();
    SurfaceDataVar cur_surf = cur_surfaces().read(dispatch_id());
    Float3 view_pos = cur_view_pos(cur_surf.is_replaced);
    Interaction canonical_it = pipeline()->geometry().compute_surface_interaction(cur_surf.hit, view_pos);

    GIReservoirVar ret = canonical_rsv;

    rsv_idx.for_each([&](const Uint &idx) {
        GIReservoirVar rsv = passthrough_reservoirs().read(idx);
        Float weight = 0.f;
        $if(rsv.W > 0.f && rsv.sample.sp->valid()) {
            SurfaceDataVar neighbor_surf = cur_surfaces().read(idx);
            Interaction neighbor_it = pipeline()->geometry().compute_surface_interaction(neighbor_surf.hit, view_pos);
            Float p_hat = compute_p_hat(canonical_it, rsv.sample);
            p_hat = p_hat * Jacobian_det(canonical_it.pos, neighbor_it.pos, rsv.sample.sp);
            $if(p_hat > 0.f) {
                Float v = pipeline()->geometry().visibility(canonical_it, rsv.sample.sp->position());
                weight = GIReservoir::safe_weight(rsv.C, p_hat, rsv.W) * v;
            };
        };
        // Do not condition the denominator on this donor's sampled radiance
        // or visibility. Cross-domain support correction is a separate step.
        ret->update(sampler()->next_1d(), rsv.sample, weight, rsv.C);
    });
    Float p_hat = compute_p_hat(canonical_it, ret.sample);
    if (debias_) {
        Float normalization = 0.f;
        $if(ret.weight_sum > 0.f && p_hat > 0.f && ret.sample.sp->valid()) {
            normalization = ocarina::max(ocarina::zero_if_nan_inf(canonical_rsv.C), 0.f);
            rsv_idx.for_each([&](const Uint &idx) {
                GIReservoirVar source_rsv = passthrough_reservoirs().read(idx);
                Float count = ocarina::max(ocarina::zero_if_nan_inf(source_rsv.C), 0.f);
                $if(count > 0.f) {
                    SurfaceDataVar source_surf = cur_surfaces().read(idx);
                    Interaction source_it = pipeline()->geometry().compute_surface_interaction(source_surf.hit, view_pos);
                    // Zero Lo/W in this source's realization does not remove its support.
                    normalization += count * selected_sample_support(canonical_it, source_it, ret.sample);
                };
            });
        };
        ret->update_W_with_support(p_hat, normalization);
    } else {
        ret->update_W(p_hat);
    }
    return ret;
}

GIReservoirVar ReSTIRGI::combine_spatial(GIReservoirVar cur_rsv,
                                         const Container<uint> &rsv_idx) const noexcept {

    cur_rsv = constant_combine(cur_rsv, rsv_idx);

    return cur_rsv;
}

GIReservoirVar ReSTIRGI::spatial_reuse(GIReservoirVar rsv, const SurfaceDataVar &cur_surf,
                                       const Int2 &pixel, const Var<GIParam> &param) const noexcept {
    $if(param.spatial) {
        Int2 res = make_int2(dispatch_dim().xy());
        Container<uint> rsv_idx{spatial_.sample_num};
        $for(i, spatial_.sample_num) {
            Float2 offset = square_to_disk(sampler()->next_2d()) * param.s_radius;
            Int2 offset_i = make_int2(ocarina::round(offset));
            Int2 another_pixel = pixel + offset_i;
            another_pixel = ocarina::clamp(another_pixel, make_int2(0u), res - 1);
            Uint index = dispatch_id(another_pixel);
            SurfaceDataVar other_surf = cur_surfaces().read(index);
            $if(is_valid_neighbor(cur_surf, other_surf, param)) {
                rsv_idx.push_back(index);
            };
        };
        $if(cur_surf.hit->is_hit()) {
            rsv = combine_spatial(rsv, rsv_idx);
        };
    };
    return rsv;
}

Float3 ReSTIRGI::shading(GIReservoirVar rsv,
                         const SurfaceDataVar &cur_surf) const noexcept {
    TSensor &camera = scene().sensor();
    Float3 view_pos = cur_view_pos(cur_surf.is_replaced);
    Float3 throughput = make_float3(1.f);
    $if(cur_surf.is_replaced) {
        SurfaceExtendVar surf_ext = cur_surface_extends().read(dispatch_id());
        throughput = surf_ext.throughput;
        view_pos = surf_ext.view_pos;
    };
    Interaction it = pipeline()->geometry().compute_surface_interaction(cur_surf.hit, view_pos);
    Float3 ret = make_float3(0.f);
    $if(rsv.W > 0.f && rsv.sample.sp->valid() &&
        length_squared(rsv.sample.sp->position() - it.pos) > 0.f) {
        ScatterEval scatter_eval = eval_bsdf(it, rsv.sample, MaterialEvalMode::All);
        $if(scatter_eval.valid() && !ocarina::isinf(scatter_eval.pdf())) {
            ret = ocarina::zero_if_nan_inf(throughput * rsv.sample.Lo.as_vec3() * scatter_eval.f.vec3() * rsv.W);
        };
    };
    return ret;
}

void ReSTIRGI::compile_spatial_shading() noexcept {
    switch_profile::Scope profile{"GI.spatial_shading.compile", "compile"};
    TSensor &camera = scene().sensor();
    TLightSampler &light_sampler = renderer().light_sampler();
    TSpectrum &spectrum = pipeline()->spectrum();

    Kernel kernel = [&](Var<GIParam> param, Uint frame_index) {
        initial(sampler(), frame_index, spectrum);
        SurfaceDataVar surf = cur_surfaces().read(dispatch_id());
        $if(surf.hit->is_miss()) {
            $return();
        };
        sampler()->load_data();
        sampler()->set_seed(dispatch_idx().xy(), frame_index, 5);
        camera->load_data();
        GIReservoirVar rsv = passthrough_reservoirs().read(dispatch_id());
        rsv = spatial_reuse(rsv, surf, make_int2(dispatch_idx().xy()), param);
        Float3 L = shading(rsv, surf);
        radiance_->write(dispatch_id(), make_float4(L, 1.f));
        cur_reservoirs().write(dispatch_id(), rsv);
    };
    spatial_shading_ = device().compile(kernel, "ReSTIR indirect spatial reuse and shading");
}

GIParam ReSTIRGI::construct_param() const noexcept {
    GIParam param;
    param.camera_jitter = integrator()->jitter_primary_samples();
    param.max_age = max_age_;
    param.diff_factor = diff_factor_;

    param.spatial = static_cast<uint>(spatial_.open);
    param.N = spatial_.sample_num;
    param.s_dot = spatial_.dot_threshold();
    param.s_depth = spatial_.depth_threshold;
    param.s_radius = spatial_.sampling_radius;

    param.temporal = static_cast<uint>(temporal_.open);
    param.history_limit = temporal_.limit;
    param.t_dot = temporal_.dot_threshold();
    param.t_depth = temporal_.depth_threshold;
    param.t_radius = temporal_.sampling_radius;
    return param;
}

CommandBatch ReSTIRGI::dispatch(uint frame_index) const noexcept {
    CommandBatch ret;
    const Pipeline *rp = pipeline();
    GIParam param = construct_param();
    // The first frame after a mode/scene reset must not reuse old reservoirs.
    bool history_valid = history_.begin_frame(frame_index, param.camera_jitter);
    param.temporal = param.temporal && history_valid;
    ret << initial_samples_(frame_index).dispatch(rp->resolution());
    ret << temporal_pass_(param, frame_index).dispatch(rp->resolution());
    ret << spatial_shading_(param, frame_index).dispatch(rp->resolution());
    return ret;
}

void ReSTIRGI::update_resolution(ocarina::uint2 res) noexcept {
    Pipeline *rp = pipeline();
    reservoirs_.super() = device().create_buffer<GIReservoir>(rp->pixel_num() * 3,
                                                              "ReSTIRGI::reservoirs_ x 3");
    reservoirs_.register_self(0, rp->pixel_num());
    reservoirs_.register_view_index(1, rp->pixel_num(), rp->pixel_num());
    reservoirs_.register_view_index(2, rp->pixel_num() * 2, rp->pixel_num());
    vector<GIReservoir> host{rp->pixel_num() * 3, GIReservoir{}};
    reservoirs_.upload_immediately(host.data());

    samples_.super() = device().create_buffer<GISample>(rp->pixel_num(),
                                                        "ReSTIRGI::samples_");
    samples_.register_self();
    vector<GISample> vec{rp->pixel_num(), GISample{}};
    samples_.upload_immediately(vec.data());
}

void ReSTIRGI::prepare() noexcept {
    switch_profile::Scope profile{"ReSTIRGI::prepare", "buffers"};
    Pipeline *rp = pipeline();

    frame_buffer().prepare_screen_buffer(radiance_);

    reservoirs_.super() = device().create_buffer<GIReservoir>(rp->pixel_num() * 3,
                                                              "ReSTIRGI::reservoirs_ x 3");
    reservoirs_.register_self(0, rp->pixel_num());
    reservoirs_.register_view(rp->pixel_num(), rp->pixel_num());
    reservoirs_.register_view(rp->pixel_num() * 2, rp->pixel_num());
    vector<GIReservoir> host{rp->pixel_num() * 3, GIReservoir{}};
    reservoirs_.upload_immediately(host.data());

    samples_.super() = device().create_buffer<GISample>(rp->pixel_num(),
                                                        "ReSTIRGI::samples_");
    samples_.register_self();
    vector<GISample> vec{rp->pixel_num(), GISample{}};
    samples_.upload_immediately(vec.data());
}
}// namespace vision

VS_REGISTER_HOTFIX(vision, ReSTIRGI)

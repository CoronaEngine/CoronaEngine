#include "reblur.h"
#include "base/mgr/pipeline.h"
#include "base/integral/integrator.h"
#include "utils.h"

namespace vision::reblur {
namespace {
Settings read_settings(const DataWrap &data, Settings s) {
    auto read = [&](const char *key, float current, float low, float high) {
        const auto it = data.find(key);
        if (it == data.end() || !it->is_number()) return current;
        const auto value = it->get<float>();
        return std::isfinite(value) ? std::clamp(value, low, high) : current;
    };
    s.max_history = read("max_history", s.max_history, 1.f, 63.f);
    s.fast_history = std::min(read("fast_history", s.fast_history, 0.f, 63.f), s.max_history);
    s.history_fix_frames = read("history_fix_frames", s.history_fix_frames, 0.f, 63.f);
    s.history_fix_stride = read("history_fix_stride", s.history_fix_stride, 0.f, 100.f);
    s.diffuse_prepass = read("diffuse_prepass", s.diffuse_prepass, 0.f, 100.f);
    s.specular_prepass = read("specular_prepass", s.specular_prepass, 0.f, 100.f);
    s.max_radius = read("max_radius", s.max_radius, 0.f, 100.f);
    s.min_radius = std::min(read("min_radius", s.min_radius, 0.f, 100.f), s.max_radius);
    s.lobe_fraction = read("lobe_fraction", s.lobe_fraction, 0.f, 1.f);
    s.roughness_fraction = read("roughness_fraction", s.roughness_fraction, 0.f, 1.f);
    s.plane_sensitivity = read("plane_sensitivity", s.plane_sensitivity, 0.f, 1.f);
    s.min_hit_weight = read("min_hit_weight", s.min_hit_weight, 0.f, 1.f);
    s.stabilization = read("stabilization", s.stabilization, 0.f, 1.f);
    s.antilag_sigma = read("antilag_sigma", s.antilag_sigma, 0.f, 10.f);
    s.antilag_sensitivity = read("antilag_sensitivity", s.antilag_sensitivity, 0.f, 10.f);
    if (data.contains("anti_firefly") && data["anti_firefly"].is_boolean())
        s.anti_firefly = data["anti_firefly"].get<bool>();
    s.reconstruction = static_cast<HitDistanceReconstructionMode>(
        static_cast<uint>(read("reconstruction", static_cast<float>(s.reconstruction), 0.f, 2.f)));
    return s;
}
}
ReBLUR::ReBLUR(const DenoiserDesc &desc) : Denoiser(desc) {
    scene_settings_ = read_settings(desc.parameter_set().data(), Settings{});
    filter_->settings = scene_settings_;
}
bool ReBLUR::set_reblur_settings(string_view json) noexcept {
    if (settings_override_ == json) return false;
    const auto data = DataWrap::parse(json.begin(), json.end(), nullptr, false);
    if (!data.is_object()) return false;
    filter_->settings = read_settings(data, scene_settings_);
    settings_override_ = json;
    filter_->reset();
    frame_ = InvalidUI32;
    return true;
}
string ReBLUR::reblur_settings_json() const {
    const auto &s = filter_->settings;
    return DataWrap{{"max_history", s.max_history}, {"fast_history", s.fast_history},
        {"history_fix_frames", s.history_fix_frames}, {"history_fix_stride", s.history_fix_stride},
        {"diffuse_prepass", s.diffuse_prepass}, {"specular_prepass", s.specular_prepass},
        {"min_radius", s.min_radius}, {"max_radius", s.max_radius},
        {"lobe_fraction", s.lobe_fraction}, {"roughness_fraction", s.roughness_fraction},
        {"plane_sensitivity", s.plane_sensitivity}, {"min_hit_weight", s.min_hit_weight},
        {"stabilization", s.stabilization}, {"antilag_sigma", s.antilag_sigma},
        {"antilag_sensitivity", s.antilag_sensitivity}, {"anti_firefly", bool(s.anti_firefly)},
        {"reconstruction", static_cast<uint>(s.reconstruction)}}.dump();
}
void ReBLUR::initialize_(const NodeDesc &) noexcept {
    DenoiserDesc desc{"svgf"}; desc.set_value("coverage_only", true);
    coverage_ = Node::create_shared<Denoiser>(desc);
}
void ReBLUR::prepare() noexcept {
    update_resolution(frame_buffer().raytracing_resolution());
    coverage_->prepare();
}
void ReBLUR::update_resolution(uint2 resolution) noexcept {
    filter_->prepare(device(), resolution, pipeline()->renderer().integrator()->stable_planes_enabled() ? 3u : 1u);
    filter_->reset(); frame_ = InvalidUI32;
    if (coverage_ && coverage_->has_prepared_resources()) coverage_->update_resolution(resolution);
}
void ReBLUR::set_enabled(bool enabled) noexcept {
    if (enabled_ != enabled) { filter_->reset(); frame_ = InvalidUI32; }
    enabled_ = enabled;
    if (coverage_) coverage_->set_enabled(enabled);
}
void ReBLUR::set_stable_planes_enabled(bool enabled) noexcept {
    if (has_prepared_resources()) filter_->prepare(device(), frame_buffer().raytracing_resolution(), enabled ? 3u : 1u);
    filter_->reset(); frame_ = InvalidUI32;
    if (coverage_) coverage_->set_stable_planes_enabled(enabled);
}
void ReBLUR::render_sub_UI(Widgets *widgets) noexcept {
    bool on = enabled_;
    if (widgets->check_box("turn on", &on)) { set_enabled(on); changed_ = true; }
}
void ReBLUR::compile() noexcept {
    auto &camera = scene().sensor(); auto &sampler = renderer().sampler();
    Kernel guides = [&](Var<GuideParam> p) {
        auto output = guide_view(p.output);
        Uint i = dispatch_id();
        $if(p.layered != 0u) {
            // Test the small validity field before loading the surface, camera
            // and sampler state for mostly empty secondary stable planes.
            $if(p.planes.read(i).valid == 0u) {
                Var<Guide> empty;
                empty.position = make_float4(0.f); empty.normal_roughness = make_float4(0.f, 0.f, 1.f, 1.f);
                empty.diffuse_factor = make_float4(1.f, 1.f, 1.f, 0.f); empty.specular_factor = empty.diffuse_factor;
                empty.identity = make_uint4(0u);
                output.write(i, empty);
                $return();
            };
        };
        camera->load_data(); sampler->load_data();
        sampler->set_seed(dispatch_idx().xy(), p.frame, 1024u);
        Var<Guide> g;
        g.position = make_float4(0.f); g.normal_roughness = make_float4(0.f, 0.f, 1.f, 1.f);
        g.diffuse_factor = make_float4(1.f, 1.f, 1.f, 0.f); g.specular_factor = g.diffuse_factor;
        g.identity = make_uint4(0u); Float2 motion = p.motion.read(i);
        TriangleHitVar hit = p.visibility.read(i);
        Float3 eye = camera->device_position(), prefix = make_float3(1.f);
        Bool valid = hit->is_hit();
        SurfaceDataVar surface;
        Uint branch = 1u, instance_hash = hit.inst_id;
        $if(p.layered != 0u) {
            auto plane = p.planes.read(i);
            surface = plane.surface; hit = surface.hit;
            eye = plane.extension.view_pos.as_vec3(); prefix = plane.extension.throughput.as_vec3();
            valid = plane.valid != 0u && hit->is_hit();
            branch = plane.branch_sequence; instance_hash = guide_instance(hit.inst_id, plane.instance_hash);
            motion = plane.motion;
        } $else {
            $if(p.stable != 0u) {
                surface = p.surfaces.read(i); hit = surface.hit;
                valid = hit->is_hit(); branch = surface.stable_branch;
                $if(surface.is_replaced != 0u) {
                    auto extension = p.extensions.read(i);
                    eye = extension.view_pos.as_vec3(); prefix = extension.throughput.as_vec3();
                };
            };
        };
        $if(valid) {
            auto it = pipeline()->geometry().compute_surface_interaction(hit, eye);
            g.position = make_float4(it.pos, camera->linear_depth(it.pos));
            g.identity = make_uint4(instance_hash, it.material_id(), branch, 1u);
            $if(it.has_material()) {
                SampledWavelengths swl{spectrum()->dimension()};
                scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                    auto bsdf = material->create_evaluator(it, swl);
                    SampledSpectrum d{swl.dimension()}, s{swl.dimension()}, unused_d{swl.dimension()}, unused_s{swl.dimension()};
                    Float3 wo = select(dot(bsdf.shading_frame().normal(), it.wo) < 0.f, -it.wo, it.wo);
                    bsdf.denoiser_factors(wo, d, s);
                    Float2 roughness = make_float2(1.f);
                    bsdf.reuse_material(unused_d, unused_s, roughness);
                    Float r = sqrt(saturate(roughness.x * roughness.y));
                    g.normal_roughness = make_float4(bsdf.shading_frame().normal(), r);
                    Float3 diffuse_factor = safe_rgb(spectrum()->linear_srgb(d, swl) * prefix);
                    Float3 specular_factor = safe_rgb(spectrum()->linear_srgb(s, swl) * prefix);
                    for (uint c = 0; c < 3u; ++c) { g.diffuse_factor[c] = diffuse_factor[c]; g.specular_factor[c] = specular_factor[c]; }
                    // Independent lobe guide rays preserve valid black samples without changing estimator RNG.
                    for (bool spec : {false, true}) {
                        Float4 signal = spec ? p.specular.read(i) : p.diffuse.read(i);
                        Float3 factor = spec ? g.specular_factor.xyz() : g.diffuse_factor.xyz();
                        $if(signal.w == 0.f && any(factor > 0.f)) {
                            auto sample = bsdf.sample_lobe(it.wo, sampler, spec);
                            $if(sample.valid()) {
                                auto ray = it.spawn_ray(sample.wi);
                                auto h = pipeline()->geometry().trace_closest(ray);
                                Float distance = scene().world_diameter();
                                $if(h->is_hit()) {
                                    auto next = pipeline()->geometry().compute_surface_interaction(h, ray);
                                    distance = length(next.pos - it.pos);
                                };
                                signal.w = max(distance, 1e-5f);
                            };
                            if (spec) p.specular.write(i, signal); else p.diffuse.write(i, signal);
                        };
                    }
                });
            };
            $if(p.stable != 0u) {
                g.position = make_float4(surface.virtual_position.as_vec3(), camera->linear_depth(surface.virtual_position.as_vec3()));
                g.normal_roughness = make_float4(normalize(surface.virtual_normal.as_vec3()), g.normal_roughness.w);
            };
            // Producer motion is a displacement between projections of the
            // same film sample. Keep it unchanged: a static camera has zero
            // motion even when the subpixel sample moves within the pixel.
        };
        g.diffuse_factor.w = motion.x; g.specular_factor.w = motion.y;
        output.write(i, g);
    };
    prepare_guides_ = device().compile(guides, "REBLUR-PrepareGuides");
    Kernel publish = [](Var<PublishParam> p) {
        Uint i = dispatch_id();
        p.direct.write(i, make_RadType4(p.diffuse.read(i).xyz() + p.bypass.read(i).xyz(), 1.f));
        p.indirect.write(i, make_RadType4(p.specular.read(i).xyz(), 1.f));
    };
    publish_ = device().compile(publish, "REBLUR-PublishPT");
    filter_->compile(device());
    coverage_->compile();
    frame_ = InvalidUI32;
}

CommandBatch ReBLUR::dispatch(RealTimeDenoiseInput &input) noexcept {
    CommandBatch ret;
    if (!enabled_) return ret;
    if (input.composed_coverage) {
        auto coverage_input = input;
        coverage_input.composed_coverage = input.coverage_planes.size() != 0u;
        return coverage_->dispatch(coverage_input);
    }
    if (!input.has_lobe_signals) {
        OC_ERROR("REBLUR requires explicit diffuse/specular radiance and world-space hit distances");
        return ret;
    }
    filter_->prepare(device(), input.resolution, input.layer_count);
    const auto &camera = scene().sensor();
    if (frame_ != input.frame_index) {
        if (frame_ == InvalidUI32 || frame_ + 1u != input.frame_index || fov_ != camera->fov_y()) filter_->reset();
        previous_raster_ = current_raster_; current_raster_ = camera->host_world_to_raster();
        frame_ = input.frame_index; fov_ = camera->fov_y();
    }
    GuideParam gp;
    gp.visibility = input.visibility.descriptor(); gp.motion = input.motion_vec.descriptor();
    gp.planes = input.stable_planes.descriptor(); gp.surfaces = input.stable_surfaces.descriptor();
    gp.extensions = frame_buffer().cur_surface_exts_view(input.frame_index).descriptor();
    gp.layered = input.layer_count > 1u; gp.stable = input.use_stable_planes; gp.frame = input.frame_index;
    gp.output = filter_->guides().descriptor();
    gp.diffuse = input.lobe_signals.diffuse_radiance_hitdist.descriptor();
    gp.specular = input.lobe_signals.specular_radiance_hitdist.descriptor();
    ret << prepare_guides_(gp).dispatch(input.resolution);
    FilterParam p;
    p.frame = input.frame_index; p.history_valid = input.frame_index != 0u;
    p.raw_diffuse = gp.diffuse; p.raw_specular = gp.specular;
    p.output_diffuse = input.lobe_signals.output_diffuse.descriptor();
    p.output_specular = input.lobe_signals.output_specular.descriptor();
    p.camera = input.camera_pos; p.prev_camera = input.prev_camera_pos;
    p.world_to_raster = current_raster_; p.prev_world_to_raster = previous_raster_;
    p.unproject = 2.f * std::tan(radians(camera->fov_y()) * .5f) / static_cast<float>(input.resolution.y);
    const auto frame_delta_ms = pipeline()->frame_delta_ms();
    p.frame_rate_scale = frame_delta_ms > 0.0 ? std::max(33.333f / static_cast<float>(frame_delta_ms), 1.f) : 1.f;
    ret << filter_->dispatch(p, input.layer_index);
    if (input.channel_kind == RealTimeDenoiseInput::ChannelKind::DiffuseSpecular) {
        PublishParam pp;
        pp.diffuse = p.output_diffuse; pp.specular = p.output_specular;
        pp.bypass = frame_buffer().denoiser_layer_view(frame_buffer().denoiser_bypass(), input.layer_index).descriptor();
        pp.direct = input.direct.descriptor(); pp.indirect = input.indirect.descriptor();
        ret << publish_(pp).dispatch(input.resolution);
        if (input.layer_count == 1u) ret << coverage_->dispatch(input);
    }
    return ret;
}
}

VS_MAKE_CLASS_CREATOR_HOTFIX_DIRECTORY(vision::reblur, ReBLUR, 1)

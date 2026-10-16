#include "relax.h"
#include "base/mgr/pipeline.h"
#include "base/integral/integrator.h"
#include "utils.h"
#include <cstdlib>
#include <fstream>

namespace vision::relax {
namespace {
Uint guide_instance(Uint receiver, Uint prefix) {
    return (prefix * 1664525u) ^ (receiver + 1013904223u);
}
Float3 safe_rgb(Float3 rgb) {
    return select(all(rgb == rgb) && all(abs(rgb) < 3.402823466e38f),
                  clamp(rgb, make_float3(0.f), make_float3(65504.f)), make_float3(0.f));
}
const char *profile_file() { return std::getenv("VISION_RELAX_PROFILE"); }
void record_time(string_view stage, uint frame, uint layer, double ms) {
    if (const auto *path = profile_file()) {
        std::ofstream out(path, std::ios::app);
        out << "{\"stage\":\"" << stage << "\",\"frame\":" << frame << ",\"layer\":" << layer << ",\"ms\":" << ms << "}\n";
    }
}
}
ReLAX::ReLAX(const DenoiserDesc &desc) : Denoiser(desc) {
    auto &s = filter_->settings;
    s.diffuse_history = std::clamp(desc["diffuse_history"].as_float(30.f), 1.f, 255.f);
    s.specular_history = std::clamp(desc["specular_history"].as_float(30.f), 1.f, 255.f);
    s.diffuse_fast_history = std::clamp(desc["diffuse_fast_history"].as_float(6.f), 0.f, s.diffuse_history);
    s.specular_fast_history = std::clamp(desc["specular_fast_history"].as_float(6.f), 0.f, s.specular_history);
    s.diffuse_prepass = std::max(desc["diffuse_prepass"].as_float(30.f), 0.f);
    s.specular_prepass = std::max(desc["specular_prepass"].as_float(50.f), 0.f);
    s.atrous_iterations = std::clamp(desc["atrous_iterations"].as_uint(5u), 2u, 8u);
    s.anti_firefly = desc["anti_firefly"].as_bool(false);
    s.reconstruction = std::min(desc["reconstruction"].as_uint(0u), 2u);
}
void ReLAX::initialize_(const NodeDesc &) noexcept {
    DenoiserDesc desc{"svgf"}; desc.set_value("coverage_only", true);
    coverage_ = Node::create_shared<Denoiser>(desc);
}
void ReLAX::prepare() noexcept {
    update_resolution(frame_buffer().raytracing_resolution());
    coverage_->prepare();
}
void ReLAX::update_resolution(uint2 resolution) noexcept {
    filter_->prepare(device(), resolution, pipeline()->renderer().integrator()->stable_planes_enabled() ? 3u : 1u);
    filter_->reset(); frame_ = InvalidUI32;
    if (coverage_ && coverage_->has_prepared_resources()) coverage_->update_resolution(resolution);
}
void ReLAX::set_enabled(bool enabled) noexcept {
    if (enabled_ != enabled) { filter_->reset(); frame_ = InvalidUI32; }
    enabled_ = enabled;
    if (coverage_) coverage_->set_enabled(enabled);
}
void ReLAX::set_stable_planes_enabled(bool enabled) noexcept {
    if (has_prepared_resources()) filter_->prepare(device(), frame_buffer().raytracing_resolution(), enabled ? 3u : 1u);
    filter_->reset(); frame_ = InvalidUI32;
    if (coverage_) coverage_->set_stable_planes_enabled(enabled);
}
void ReLAX::render_sub_UI(Widgets *widgets) noexcept {
    bool on = enabled_;
    if (widgets->check_box("turn on", &on)) { set_enabled(on); changed_ = true; }
}
void ReLAX::compile() noexcept {
    Clock compile_clock;
    auto &camera = scene().sensor(); auto &sampler = renderer().sampler();
    Kernel guides = [&](Var<GuideParam> p) {
        camera->load_data(); sampler->load_data();
        sampler->set_seed(dispatch_idx().xy(), p.frame, 1024u);
        Uint i = dispatch_id();
        Var<Guide> g;
        g.position = make_float4(0.f); g.normal_roughness = make_float4(0.f, 0.f, 1.f, 1.f);
        g.diffuse_factor = make_float4(1.f); g.specular_factor = make_float4(1.f);
        g.identity = make_uint4(0u); g.motion = p.motion.read(i);
        TriangleHitVar hit = p.visibility.read(i);
        Float3 eye = camera->device_position(), prefix = make_float3(1.f);
        Bool valid = hit->is_hit();
        SurfaceDataVar surface;
        Uint branch = 1u, instance_hash = hit.inst_id;
        $if(p.layered != 0u) {
            auto plane = p.planes.read(i);
            surface = plane.surface; hit = surface.hit;
            eye = plane.extension.view_pos; prefix = plane.extension.throughput;
            valid = plane.valid != 0u && hit->is_hit();
            branch = plane.branch_sequence; instance_hash = guide_instance(hit.inst_id, plane.instance_hash);
            g.motion = plane.motion;
        } $else {
            $if(p.stable != 0u) {
                surface = p.surfaces.read(i); hit = surface.hit;
                valid = hit->is_hit(); branch = surface.stable_branch;
                $if(surface.is_replaced != 0u) {
                    auto extension = p.extensions.read(i);
                    eye = extension.view_pos; prefix = extension.throughput;
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
                    g.diffuse_factor = make_float4(safe_rgb(spectrum()->linear_srgb(d, swl) * prefix), 0.f);
                    g.specular_factor = make_float4(safe_rgb(spectrum()->linear_srgb(s, swl) * prefix), 0.f);
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
                g.position = make_float4(surface.virtual_position, camera->linear_depth(surface.depth_position));
                g.normal_roughness = make_float4(normalize(surface.virtual_normal), g.normal_roughness.w);
            };
            // Producer motion is a displacement between projections of the
            // same film sample. Keep it unchanged: a static camera has zero
            // motion even when the subpixel sample moves within the pixel.
        };
        p.output.write(i, g);
    };
    prepare_guides_ = device().compile(guides, "RELAX-PrepareGuides");
    Kernel publish = [](Var<PublishParam> p) {
        Uint i = dispatch_id();
        p.direct.write(i, make_RadType4(p.diffuse.read(i).xyz() + p.bypass.read(i).xyz(), 1.f));
        p.indirect.write(i, make_RadType4(p.specular.read(i).xyz(), 1.f));
    };
    publish_ = device().compile(publish, "RELAX-PublishPT");
    filter_->compile(device()); coverage_->compile();
    compiled_ = true;
    record_time("CompileCPU", 0u, 0u, compile_clock.elapse_ms());
    frame_ = InvalidUI32;
}

CommandBatch ReLAX::dispatch(RealTimeDenoiseInput &input) noexcept {
    CommandBatch ret;
    if (!enabled_) return ret;
    // A pipeline may start in raw mode and enable its selected denoiser later.
    // At dispatch the producer resources and scene context are already ready.
    if (!has_prepared_resources() || !compiled_) { prepare(); compile(); }
    Filter::PassSubmit submit;
    if (profile_file()) submit = [&](CommandBatch commands, string_view stage) {
        Clock clock;
        pipeline()->stream() << commands << synchronize() << commit();
        record_time(stage, input.frame_index, input.layer_index, clock.elapse_ms());
    };
    auto coverage = [&](RealTimeDenoiseInput &value) {
        auto commands = coverage_->dispatch(value);
        if (submit) {
            submit(commands, "Coverage");
            auto &fb = frame_buffer();
            size_t producer = fb.denoiser_diffuse().view().size_in_byte() + fb.denoiser_specular().view().size_in_byte() +
                fb.denoiser_bypass().view().size_in_byte() + fb.denoiser_di_specular().view().size_in_byte() +
                fb.denoiser_gi_specular().view().size_in_byte() + fb.denoiser_di_bypass().view().size_in_byte();
            // Coverage-only owns four float4 resolve images, plus four cached guides,
            // one edge mask and four Poisson scratch guides when geometry is guided.
            const size_t coverage_bytes = size_t(input.resolution.x) * input.resolution.y *
                (4u * sizeof(float4) + ((value.use_stable_planes || value.composed_coverage) ? 8u * sizeof(float4) + sizeof(uint) : 0u));
            std::ofstream out(profile_file(), std::ios::app);
            out << "{\"resources\":true,\"frame\":" << input.frame_index << ",\"filter_bytes\":" << filter_->allocated_bytes()
                << ",\"producer_bytes\":" << producer << ",\"coverage_logical_bytes\":" << coverage_bytes << "}\n";
            commands.clear();
        }
        return commands;
    };
    if (input.composed_coverage) {
        auto coverage_input = input;
        coverage_input.composed_coverage = input.coverage_planes.size() != 0u;
        return coverage(coverage_input);
    }
    if (!input.has_lobe_signals) {
        OC_ERROR("RELAX requires explicit diffuse/specular radiance and world-space hit distances");
        return ret;
    }
    filter_->prepare(device(), input.resolution, input.layer_count);
    const auto &camera = scene().sensor();
    if (frame_ != input.frame_index) {
        if (frame_ == InvalidUI32 || frame_ + 1u != input.frame_index || fov_ != camera->fov_y()) filter_->reset();
        previous_raster_ = current_raster_; current_raster_ = camera->host_world_to_raster();
        previous_view_ = current_view_; current_view_ = inverse(camera->host_c2w());
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
    if (submit) { submit(ret, "PrepareGuidesIncludingRays"); ret.clear(); }
    FilterParam p;
    p.frame = input.frame_index; p.history_valid = input.frame_index != 0u;
    p.raw_diffuse = gp.diffuse; p.raw_specular = gp.specular;
    p.output_diffuse = input.lobe_signals.output_diffuse.descriptor();
    p.output_specular = input.lobe_signals.output_specular.descriptor();
    p.camera = input.camera_pos; p.prev_camera = input.prev_camera_pos;
    p.world_to_raster = current_raster_; p.prev_world_to_raster = previous_raster_;
    p.prev_world_to_view = previous_view_;
    double dt = pipeline()->frame_delta_ms();
    if (!std::isfinite(dt) || dt <= 0.0) dt = 1000.0 / 60.0;
    p.framerate_scale = std::clamp(16.66f / static_cast<float>(dt), .25f, 4.f);
    // MathLib v1.3 Sequence::Weyl1D(0.5, frame), Geometry::GetRotator.
    float phase = .5f + static_cast<float>(input.frame_index * 10368889u) / 16777216.f;
    float angle = (phase - std::floor(phase)) * 1.57079632679f;
    p.rotator = make_float4(std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle));
    p.unproject = 2.f * std::tan(radians(camera->fov_y()) * .5f) / static_cast<float>(input.resolution.y);
    ret << filter_->dispatch(p, input.layer_index, submit);
    if (input.channel_kind == RealTimeDenoiseInput::ChannelKind::DiffuseSpecular) {
        PublishParam pp;
        pp.diffuse = p.output_diffuse; pp.specular = p.output_specular;
        pp.bypass = frame_buffer().denoiser_layer_view(frame_buffer().denoiser_bypass(), input.layer_index).descriptor();
        pp.direct = input.direct.descriptor(); pp.indirect = input.indirect.descriptor();
        ret << publish_(pp).dispatch(input.resolution);
        if (submit) { submit(ret, "PublishPT"); ret.clear(); }
        if (input.layer_count == 1u) ret << coverage(input);
    }
    return ret;
}
}

VS_MAKE_CLASS_CREATOR_HOTFIX_DIRECTORY(vision::relax, ReLAX, 1)

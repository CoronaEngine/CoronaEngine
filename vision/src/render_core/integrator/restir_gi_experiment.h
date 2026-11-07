#pragma once

#include "ReSTIR/indirect.h"
#include "restir_gi/history.h"
#include "restir_gi/literal.h"
#include <array>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace vision {

// Cache-disabled diffuse ReSTIR algorithm from D:/work/utils/RestirGI.
// Deliberate framework adaptations: Corona traversal, light sampling and material
// evaluation; no reference roughness bias, SSAO (AO=1), or world cache. Feedback
// uses raw resolved irradiance, not the reference's separate GI denoiser.
// DI owns primary environment/emitter transport, which these GI candidates omit.
// Reservoir exchange/validation/near-far resolve retain the reference equations.
class RestirGIExperiment final : private Toolkit {
    using SampleVar = restir_gi::SampleVar;
    using Reservoir = restir_gi::Reservoir;
    using Math = restir_gi::DeviceMath;
    uint2 extent_{}, sampling_{};
    uint scale_{}, pixels_{}, cells_{};
    FrameBuffer *frame_buffer_{};
    std::string mode_;
    std::string diagnostic_;
    bool feedback_{};
    bool aligned_lambert_{};
    bool multibounce_bench_{}, multibounce_diagnostics_{};
    mutable Buffer<float4> multibounce_path_stats_;
    uint multibounce_sample_offset_{};
    uint previous_frame_{InvalidUI32};
    WP<const GeometryGpuResource> previous_geometry_;
    uint64_t previous_geometry_revision_{};
    Buffer<float4> positions_, normals_, albedo_, feedback_history_, reprojected_;
    std::array<restir_gi::HistoryImages,2> history_;
    Texture2D guide_depth_, guide_view_normal_, guide_world_normal_;
    Texture2D spatial_samples_, selected_radiance_, candidate_hit_, candidate_radiance_, candidate_normals_;
    std::array<Texture2D,3> reservoirs_;
    Buffer<float> invalidity_;
    std::string literal_header_;
    std::array<Shader<void(uint)>,2> validate_;
    Shader<void(uint,float4x4)> prepare_;
    Shader<void(uint,float4x4,float4x4)> resolve_;
    Shader<void(uint,uint)> feedback_reproject_, candidates_shader_;
    std::array<Shader<void(uint,uint)>,2> temporal_;
    Shader<void(uint,uint,float4x4,float4x4,float4x4)> spatial0_, spatial1_;

    [[nodiscard]] static bool aligned_lambert_requested() noexcept {
        const char *value=std::getenv("VISION_RESTIR_GI_ALIGNED_LAMBERT");
        return value&&value[0]=='1';
    }

    [[nodiscard]] static std::string_view diagnostic_requested() noexcept {
        const char *value=std::getenv("VISION_RESTIR_GI_DIAGNOSTIC");
        return value?std::string_view{value}:std::string_view{};
    }

    [[nodiscard]] Uint full_index(const Uint2 &p) const { return p.x+p.y*extent_.x; }
    [[nodiscard]] Uint cell_index(const Uint2 &p) const { return p.x+p.y*sampling_.x; }
    [[nodiscard]] Bool in_full(const Int2 &p) const { return all(p>=make_int2(0))&&all(p<make_int2(extent_)); }
    [[nodiscard]] Bool in_cells(const Int2 &p) const { return all(p>=make_int2(0))&&all(p<make_int2(sampling_)); }
    [[nodiscard]] Uint2 subpixel(const Uint &frame) const {
        if (scale_==1u) return make_uint2(0u);
        Uint phase=frame&3u;
        return make_uint2(select(phase<2u,1u,0u),select(phase==0u||phase==3u,1u,0u));
    }
    [[nodiscard]] Uint2 full_pixel(const Uint2 &p,const Uint &frame) const {
        if (scale_==1u) return p;
        return min(p*scale_+subpixel(frame),make_uint2(extent_)-1u);
    }
    [[nodiscard]] Float4 position(const Uint2 &p,const Uint &frame) const {
        return positions_.read((frame&1u)*pixels_+full_index(full_pixel(p,frame)),false);
    }
    [[nodiscard]] Float3 normal(const Uint2 &p,const Uint &frame) const {
        return normals_.read((frame&1u)*pixels_+full_index(full_pixel(p,frame)),false).xyz();
    }
    [[nodiscard]] Float near_end(const Float &depth) const { return depth*(40.f/float(extent_.y)); }

    [[nodiscard]] Float3 reconstruct(const Uint2 &full_pixel,const Float &depth,const Float4x4 &raster_to_camera) const {
        Float3 ray=transform_point(raster_to_camera,make_float3(make_float2(full_pixel)+.5f,0.f));
        return transform_point(scene().sensor()->device_c2w(),ray*(depth/ray.z));
    }

    template<typename T> void bind(Buffer<T> &buffer,const char *name,uint scale,uint slices) {
        auto bytes=frame_buffer_->prepare_screen_storage(name,make_uint2(scale),sizeof(T)*slices);
        buffer=Buffer<T>{bytes.template view_as<T>()};
    }

    // Screen feedback is irradiance/pi. Diffuse body reflectance converts it
    // back to incident-path radiance at the secondary surface.
    [[nodiscard]] SampleVar trace(const Float3 &origin,const Float3 &dir,const Float &maximum,
                                  const Uint &frame,const Uint &light_seed,bool record_path_stats=false) const {
        auto &sampler=renderer().sampler();
        auto &camera=scene().sensor();
        auto &geo=geometry();
        auto &lights=renderer().light_sampler();
        auto &spec=spectrum();
        // The source's hard-sun evaluation is deterministic for a fixed path.
        // Replay this path's area-light draw during validation as well, so
        // Monte Carlo noise is not interpreted as changing scene radiance.
        sampler->set_seed(make_uint2(0u),light_seed,Dimension::PathTracing);
        const auto swl=spec->sample_wavelength(sampler);
        RayVar ray;
        ray.org_min=make_float4(origin,0.f);
        ray.dir_max=make_float4(dir,maximum);
        auto hit=geo.trace_closest(ray);
        SampleVar result;
        result.light_seed=light_seed;
        Float3 radiance=make_float3(0.f);
        Float distance=1e4f;
        Float3 hit_normal=-dir;
        $if(hit->is_hit()) {
            auto it=geo.compute_surface_interaction(hit,ray);
            distance=length(it.pos-origin);
            hit_normal=it.ng;
            if(aligned_lambert_) {
                // Shared controlled Sponza benchmark: exact Lambert, opaque,
                // unit delta sun from +Y, black sky/emission. No material/NEE
                // sampling difference may enter this cross-renderer GI test.
                hit_normal=normalize(multibounce_bench_?Float3{it.ng}:Float3{it.shading.normal()});
                hit_normal=ocarina::select(dot(hit_normal,dir)>0.f,-hit_normal,hit_normal);
                if(multibounce_bench_) {
                    // Uniform first proposal is retained. RR uses the physical
                    // camera throughput, including rho*cos/pdf at that proposal.
                    Int2 source=clamp(make_int2(floor(camera->raster_coord(origin).xy())),make_int2(0),make_int2(extent_)-1);
                    Float3 primary_normal=normals_.read((frame&1u)*pixels_+full_index(make_uint2(source)),false).xyz();
                    Float camera_beta=1.4f*max(0.f,dot(primary_normal,dir));
                    Float weight=1.f;
                    Uint rng=restir_gi::hash(light_seed^0x6c8e9cf5u);
                    RayVar path_ray=ray;
                    TriangleHitVar path_hit=hit;
                    Uint last_depth=1u,rr_tests=0u,rr_survivals=0u,reason=1u;
                    $for(depth,2u,17u) {
                        $if(path_hit->is_miss()) { reason=1u; $break; };
                        auto path_it=geo.compute_surface_interaction(path_hit,path_ray);
                        Float3 path_n=normalize(path_it.ng);
                        path_n=ocarina::select(dot(path_n,path_ray.dir_max.xyz())>0.f,-path_n,path_n);
                        path_it.ng=path_n;
                        last_depth=depth;
                        // Corona's Mitsuba/OBJ import reflects canonical world Z.
                        const Float3 sun=make_float3(0.f,.4472135954999579f,.8944271909999159f);
                        Float cosine=max(0.f,dot(path_n,sun));
                        $if(cosine>0.f&&!geo.occluded(path_it,path_it.pos+sun*1000.f)) {
                            radiance+=make_float3(weight*(.7f/3.14159265358979323846f)*cosine);
                        };
                        $if(depth==16u) { reason=3u; $break; };
                        Float before=camera_beta;
                        camera_beta*=.7f;
                        weight*=.7f;
                        $if(depth>=6u&&before<1.f) {
                            Float q=min(.95f,before);
                            rr_tests+=1u;
                            $if(restir_gi::random(rng)>q) { reason=2u; $break; };
                            rr_survivals+=1u;
                            camera_beta/=q; weight/=q;
                        };
                        Float2 random=make_float2(restir_gi::random(rng),restir_gi::random(rng));
                        Float radius=sqrt(random.y),angle=6.2831853071795864769f*random.x;
                        Float3 local=make_float3(radius*cos(angle),radius*sin(angle),sqrt(max(0.f,1.f-random.y)));
                        Float3 axis=ocarina::select(abs(path_n.z)<.999f,Float3{make_float3(0.f,0.f,1.f)},Float3{make_float3(0.f,1.f,0.f)});
                        Float3 tangent=restir_gi::direction(cross(axis,path_n));
                        Float3 next=tangent*local.x+cross(path_n,tangent)*local.y+path_n*local.z;
                        path_ray.org_min=make_float4(Interaction::custom_offset_ray_origin(path_it.pos,path_n),0.f);
                        path_ray.dir_max=make_float4(next,1000.f);
                        path_hit=geo.trace_closest(path_ray);
                    };
                    if(record_path_stats&&multibounce_diagnostics_)
                        multibounce_path_stats_.write(dispatch_id(),make_float4(cast<float>(last_depth),cast<float>(rr_tests),cast<float>(rr_survivals),cast<float>(reason)),false);
                } else {
                Float cosine=max(hit_normal.y,0.f);
                $if(cosine>0.f&&!geo.occluded(it,it.pos+make_float3(0.f,1e4f,0.f))) {
                    radiance=make_float3(cosine*(.7f/3.14159265358979323846f));
                };
                }
            } else {
            $if(it.has_material()) {
                scene().materials().dispatch(it.material_id(),[&](const Material *material) {
                    auto bsdf=material->create_evaluator(it,swl);
                    hit_normal=bsdf.shading_frame().normal();
                    hit_normal=ocarina::select(dot(hit_normal,dir)>0.f,-hit_normal,hit_normal);
                    LightSample light=lights->sample_wi(it,sampler,swl);
                    Float pdf=abs(light.eval.pdf);
                    $if(light.valid()&&pdf>0.f) {
                        Float3 wi=restir_gi::direction(light.p_light-it.pos);
                        auto eval=bsdf.evaluate(it.wo,wi);
                        $if(eval.valid()&&!geo.occluded(it,light.p_light)) {
                            radiance=spec->linear_srgb(light.eval.L*eval.f/pdf,swl);
                        };
                    };
                    if (feedback_) {
                        Float2 projected=camera->raster_coord(it.pos).xy();
                        Int2 p=make_int2(floor(projected));
                        $if(in_full(p)) {
                            Uint address=full_index(make_uint2(p));
                            Float4 screen=positions_.read((frame&1u)*pixels_+address,false);
                            Float depth=camera->linear_depth(it.pos);
                            Float4 old=reprojected_.read(address,false);
                            $if(screen.w>0.f&&abs(screen.w-depth)<.005f*max(depth,1e-6f)&&old.w>0.f) {
                                SampledSpectrum diffuse{swl.dimension()},specular{swl.dimension()};
                                bsdf.albedo_split(it.wo,diffuse,specular);
                                radiance+=old.xyz()*spec->linear_srgb(diffuse,swl);
                            };
                        };
                    }
                });
            };
            }
        };
        result.hit_distance=make_float4(origin+dir*distance,distance);
        result.origin_cosine=make_float4(origin,-dot(hit_normal,dir));
        result.radiance_count=make_float4(max(zero_if_nan_inf(radiance),make_float3(0.f)),1.f);
        result.normal_weight=make_float4(hit_normal,1.f);
        return result;
    }

    void compile_surface() {
        auto &fb=*frame_buffer_;
        auto &camera=scene().sensor();
        auto &sampler=renderer().sampler();
        auto &spec=spectrum();
        Kernel prepare=[&](Uint frame,Float4x4 world_to_camera) {
            camera->load_data(); sampler->load_data();
            Uint index=dispatch_id();
            Uint offset=(frame&1u)*pixels_+index;
            Float4 p=make_float4(0.f),n=make_float4(0.f),a=make_float4(0.f);
            auto hit=fb.cur_visibility_buffer_var(frame).read(index);
            $if(hit->is_hit()) {
                auto ray=fb.rays().read(index)->to_ray_state().ray;
                auto it=geometry().compute_surface_interaction(hit,ray);
                p=make_float4(it.pos,transform_point(world_to_camera,it.pos).z);
                n=make_float4(it.shading.normal(),0.f);
                if(aligned_lambert_) {
                    Float3 oriented=normalize(multibounce_bench_?Float3{it.ng}:Float3{it.shading.normal()});
                    oriented=ocarina::select(dot(oriented,ray.dir_max.xyz())>0.f,-oriented,oriented);
                    n=make_float4(oriented,0.f);
                    a=make_float4(.7f,.7f,.7f,0.f);
                } else {
                const auto swl=spec->sample_wavelength(sampler);
                scene().materials().dispatch(it.material_id(),[&](const Material *material) {
                    auto bsdf=material->create_evaluator(it,swl);
                    SampledSpectrum diffuse{swl.dimension()},specular{swl.dimension()};
                    bsdf.albedo_split(it.wo,diffuse,specular);
                    a=make_float4(spec->linear_srgb(diffuse,swl),0.f);
                    n=make_float4(bsdf.shading_frame().normal(),0.f);
                });
                }
            };
            positions_.write(offset,p,false); normals_.write(offset,n,false); albedo_.write(index,a,false);
            // Native guide: GBuffer UNORM11/10/11 normal, then view-space SNORM8.
            Uint2 pixel=dispatch_idx().xy();
            Uint packed_normal=restir_gi::pack_normal_11_10_11(n.xyz());
            Float3 view_normal=normalize(transform_vector(world_to_camera,restir_gi::unpack_normal_11_10_11(packed_normal)));
            guide_depth_.write_native(pixel,make_float4(p.w,0.f,0.f,0.f));
            guide_view_normal_.write_native(pixel,make_float4(view_normal,1.f));
            guide_world_normal_.write_native(pixel,make_uint4(packed_normal,0u,0u,0u));
        };
        prepare.function()->set_block_dim(8u,8u);
        prepare_=device().compile(prepare,"RestirGI reference surface preparation");
        if (!feedback_) return;
        Kernel reproj=[&](Uint frame,Uint history) {
            camera->load_data();
            Uint i=dispatch_id();
            Float4 output=make_float4(0.f);
            Float4 p=positions_.read((frame&1u)*pixels_+i,false);
            $if(history!=0u&&p.w>0.f) {
                Int2 previous=make_int2(floor(camera->prev_raster_coord(p.xyz()).xy()));
                $if(in_full(previous)) {
                    Uint old_index=((frame&1u)^1u)*pixels_+full_index(make_uint2(previous));
                    Float4 old_position=positions_.read(old_index,false);
                    Float3 n=normals_.read((frame&1u)*pixels_+i,false).xyz();
                    Float3 old_n=normals_.read(old_index,false).xyz();
                    $if(old_position.w>0.f&&length(old_position.xyz()-p.xyz())<.1f*p.w&&dot(n,old_n)>.2f) {
                        output=feedback_history_.read(old_index,false);
                    };
                };
            };
            reprojected_.write(i,output,false);
        };
        reproj.function()->set_block_dim(8u,8u);
        feedback_reproject_=device().compile(reproj,"RestirGI reference feedback reprojection");
    }

    void compile_tracing() {
        auto &sampler=renderer().sampler();
        auto &camera=scene().sensor();
        Kernel candidates=[&](Uint frame,Uint validation) {
            Function::current()->require_ray_query_compute_workgroups();
            sampler->load_data(); camera->load_data();
            Uint2 pixel=dispatch_idx().xy(); Uint index=cell_index(pixel);
            sampler->set_seed(pixel,frame,Dimension::PathTracing);
            SampleVar result;
            Float offset_side=1.f;
            Float4 receiver=position(pixel,frame);
            if(multibounce_diagnostics_) multibounce_path_stats_.write(index,make_float4(0.f),false);
            $if(receiver.w>0.f) {
                Float3 n=normal(pixel,frame);
                Uint rng=restir_gi::seed(pixel,frame+multibounce_sample_offset_);
                Float3 local=square_to_hemisphere(make_float2(restir_gi::random(rng),restir_gi::random(rng)));
                Float3 axis=ocarina::select(abs(n.z)<.999f,Float3{make_float3(0.f,0.f,1.f)},Float3{make_float3(0.f,1.f,0.f)});
                Float3 tangent=restir_gi::direction(cross(axis,n));
                Float3 dir=tangent*local.x+cross(n,tangent)*local.y+n*local.z;
                Float3 receiver_position=receiver.xyz();
                offset_side=select(dot(dir,n*Interaction::ray_offset_factor())>0.f,1.f,-1.f);
                Float3 origin=Interaction::custom_offset_ray_origin(receiver_position,n,dir);
                if(multibounce_diagnostics_) multibounce_path_stats_.write(index,make_float4(1.f,0.f,0.f,1.f),false);
                result=trace(origin,dir,multibounce_bench_?Float{1000.f}:select(validation!=0u,near_end(receiver.w),Float{1e4f}),frame,
                             restir_gi::hash(restir_gi::seed(pixel,frame+multibounce_sample_offset_)^0xa511e9b3u),true);
            };
            // W preserves the tracing-origin side exactly. Temporal can rebuild
            // that origin from the same FP32 receiver and primary normal.
            candidate_hit_.write_native(pixel,make_float4(result.hit_distance.xyz()-receiver.xyz(),offset_side));
            candidate_radiance_.write_native(pixel,make_float4(result.radiance_count.xyz(),0.f));
            candidate_normals_.write_native(pixel,make_float4(result.normal_weight.xyz(),result.origin_cosine.w));
            invalidity_.write(index,0.f,false);
        };
        candidates.function()->set_block_dim(8u,8u);
        candidates_shader_=device().compile(candidates,"RestirGI reference candidates");
        for(uint phase=0u;phase<2u;++phase) {
        auto &previous_history=history_[phase^1u];
        Kernel validate=[&](Uint frame) {
            Function::current()->require_ray_query_compute_workgroups();
            sampler->load_data(); camera->load_data();
            Uint2 pixel=dispatch_idx().xy(); Uint i=cell_index(pixel);
            SampleVar sample=previous_history.read(pixel);
            Float invalidity=1.f;
            $if(sample.radiance_count.w>0.f&&position(pixel,frame).w>0.f) {
                Float3 old_dir=restir_gi::direction(sample.hit_distance.xyz()-sample.origin_cosine.xyz());
                SampleVar fresh=trace(sample.origin_cosine.xyz(),old_dir,Float{1e4f},frame,sample.light_seed);
                Float3 old=max(Float3{sample.radiance_count.xyz()},make_float3(0.f));
                Float3 now=max(Float3{fresh.radiance_count.xyz()},make_float3(0.f));
                Float difference=length(abs(old-now)/max(old+now,make_float3(1e-3f)))/1.732050807568877f;
                invalidity=restir_gi::smooth(Float{.1f},Float{.5f},difference);
                Float distance=length(sample.hit_distance.xyz()-sample.origin_cosine.xyz());
                $if(abs(fresh.hit_distance.w-distance)<.4f*distance) {
                    Float old_lum=restir_gi::luminance(old),new_lum=restir_gi::luminance(now);
                    sample.radiance_count=make_float4(now,sample.radiance_count.w*Math::validation_m_scale(old_lum,new_lum));
                    sample.normal_weight.w*=Math::validation_w_scale(old_lum,new_lum);
                    Float2 mw=unpack_storage_half2(pack_storage_half2(make_float2(sample.radiance_count.w,sample.normal_weight.w)));
                    sample.radiance_count.w=mw.x; sample.normal_weight.w=mw.y;
                    previous_history.write(pixel,sample);
                };
            };
            invalidity_.write(i,invalidity,false);
        };
        validate.function()->set_block_dim(8u,8u);
        validate_[phase]=device().compile(validate,"RestirGI reference validation");
        }
    }

    void compile_temporal() {
        auto &camera=scene().sensor();
        for(uint phase=0u;phase<2u;++phase) {
        auto &current_history=history_[phase];
        auto &previous_history=history_[phase^1u];
        Kernel temporal=[&](Uint frame,Uint flags) {
            camera->load_data();
            Uint2 pixel=dispatch_idx().xy();
            Float4 receiver=position(pixel,frame);
            Float3 n=normal(pixel,frame);
            SampleVar selected;
            restir_gi::Stream stream;
            Float center_count=0.f;
            Float raw_invalidity=0.f;
            Uint rng=restir_gi::seed(pixel,frame+multibounce_sample_offset_);
            $if((flags&2u)!=0u) {
                Float2 center_motion=frame_buffer_->motion_vectors().read(full_index(full_pixel(pixel,frame)));
                Int2 previous=make_int2(floor(make_float2(pixel)-center_motion/float(scale_)+.5f));
                raw_invalidity=1.f;
                $if(in_cells(previous)) { raw_invalidity=invalidity_.read(cell_index(make_uint2(previous)),false); };
            };
            $if(receiver.w>0.f) {
                $if((flags&2u)==0u) {
                    Float4 hit=candidate_hit_.read_native<float4>(pixel);
                    Float4 secondary_normal=candidate_normals_.read_native<float4>(pixel);
                    selected.hit_distance=make_float4(receiver.xyz()+hit.xyz(),0.f);
                    Float3 receiver_position=receiver.xyz();
                    Float3 offset_normal=n*hit.w;
                    selected.origin_cosine=make_float4(Interaction::custom_offset_ray_origin(receiver_position,offset_normal),secondary_normal.w);
                    selected.radiance_count=make_float4(candidate_radiance_.read_native<float4>(pixel).xyz(),1.f);
                    selected.normal_weight=make_float4(secondary_normal.xyz(),1.f);
                    selected.light_seed=restir_gi::hash(restir_gi::seed(pixel,frame+multibounce_sample_offset_)^0xa511e9b3u);
                    Float3 dir=restir_gi::direction(selected.hit_distance.xyz()-receiver.xyz());
                    Float target=select(dot(n,dir)>=0.f,restir_gi::luminance(selected.radiance_count.xyz()),0.f);
                    stream.initialize(target);
                };
                $if((flags&1u)!=0u) {
                    $for(slot,5u) {
                        $if(stream.count>=25.f) { $break; };
                        // The deterministic permutation table is shared with the source.
                        auto offset_for=[](const Uint &p) {
                            return make_int2(select((p&1u)==0u,-1,1),select((p&3u)==0u||(p&3u)==3u,-1,1));
                        };
                        Int2 offset=make_int2(0);
                        $if(slot!=0u) { offset=offset_for(frame&3u)+offset_for((slot+(frame^1u))&3u); };
                        $if(slot!=0u&&all(offset==make_int2(0))) { $continue; };
                        Int2 lookup=make_int2(pixel)+offset;
                        $if(!in_cells(lookup)) { $continue; };
                        Uint phase=frame&3u;
                        Int2 xor_value=make_int2(select(phase==1u,2,select(phase==2u,1,3)),select(phase==1u,1,select(phase==2u,2,3)));
                        Int2 permuted=select(slot==0u,make_int2(pixel),((make_int2(pixel)+offset)^xor_value));
                        Int2 neighbor=permuted+offset;
                        Uint2 lookup_full=full_pixel(make_uint2(lookup),frame);
                        Float2 motion=frame_buffer_->motion_vectors().read(full_index(lookup_full));
                        Int2 previous=make_int2(floor(make_float2(permuted)-motion/float(scale_)+.5f))+offset;
                        $if(!in_cells(previous)||!in_cells(neighbor)) { $continue; };
                        Float4 neighbor_position=position(make_uint2(neighbor),frame);
                        SampleVar old=previous_history.read(make_uint2(previous));
                        Float3 delta=old.hit_distance.xyz()-receiver.xyz();
                        Float distance=length(delta); Float3 dir=restir_gi::direction(delta);
                        Float normal_similarity=max(0.f,dot(normal(make_uint2(neighbor),frame),n));
                        Float relevance=(1.f-restir_gi::smooth(Float{0.f},Float{.1f},Math::inverse_depth_relative_difference(receiver.w,neighbor_position.w)))*normal_similarity;
                        Bool valid=neighbor_position.w>0.f&&old.radiance_count.w>0.f&&distance>1e-6f&&dot(dir,n)>=1e-3f&&
                            length(old.origin_cosine.xyz()-receiver.xyz())<=.1f*receiver.w;
                        valid=valid&&(slot==0u||normal_similarity>=.2f);
                        $if(!valid) { $continue; };
                        Float count=Math::temporal_count(old.radiance_count.w,raw_invalidity,relevance);
                        $if(slot==0u) { center_count=count; };
                        Float target=restir_gi::luminance(old.radiance_count.xyz());
                        Float jacobian=Math::temporal_jacobian(old.hit_distance.w,distance,old.origin_cosine.w,-dot(old.normal_weight.xyz(),dir));
                        Reservoir r{Uint{0u},count,old.normal_weight.w};
                        $if(stream.merge(r,target,jacobian,rng)) { selected=old; };
                    };
                };
            };
            Float3 dir=restir_gi::direction(selected.hit_distance.xyz()-receiver.xyz());
            Float weight=stream.finish();
            Float count=select(receiver.w>0.f,center_count+.5f,0.f);
            selected.hit_distance.w=length(selected.hit_distance.xyz()-receiver.xyz());
            selected.origin_cosine.w=-dot(selected.normal_weight.xyz(),dir);
            // Reference reservoir storage rounds M and W to FP16 between passes.
            Float2 stored=unpack_storage_half2(pack_storage_half2(make_float2(count,weight)));
            selected.radiance_count.w=stored.x;
            selected.normal_weight.w=stored.y;
            current_history.write(pixel,selected);
            spatial_samples_.write_native(pixel,restir_gi::pack_temporal_output(receiver.w,
                selected.hit_distance.xyz()-receiver.xyz(),restir_gi::luminance(selected.radiance_count.xyz()),
                selected.normal_weight.xyz()));
            selected_radiance_.write_native(pixel,make_float4(selected.radiance_count.xyz(),dot(n,dir)));
            reservoirs_[0].write_native(pixel,restir_gi::pack(Reservoir{restir_gi::pack_pixel(pixel),count,weight}));
        };
        temporal.function()->set_block_dim(8u,8u);
        temporal_[phase]=device().compile(temporal,"RestirGI reference temporal reuse");
        }
    }

    [[nodiscard]] const ocarina::Expression *image_argument(const Texture2D &image,ocarina::Usage usage) const {
        const auto *expression=image.expression();
        expression->mark(usage);
        return expression;
    }

    void compile_spatial(uint pass) {
        auto &camera=scene().sensor();
        Kernel spatial=[&](Uint frame,Uint unused,Float4x4 world_to_camera,Float4x4 camera_to_raster,Float4x4 raster_to_camera) {
            camera->load_data();
            auto *function=Function::current();
            function->add_header(literal_header_);
            Uint2 pixel=dispatch_idx().xy(),extent=make_uint2(extent_),sampling=make_uint2(sampling_);
            Uint pass_index=pass,scale=scale_;
            Uint random_frame=frame+multibounce_sample_offset_;
            Float4x4 camera_to_world=camera->device_c2w();
            Float3 eye=camera->device_position();
            function->expr_statement(function->call(nullptr,"restir_gi_literal_spatial",{
                OC_EXPR(pixel),OC_EXPR(frame),OC_EXPR(random_frame),OC_EXPR(pass_index),OC_EXPR(extent),OC_EXPR(sampling),OC_EXPR(scale),
                OC_EXPR(camera_to_world),OC_EXPR(world_to_camera),OC_EXPR(camera_to_raster),OC_EXPR(raster_to_camera),OC_EXPR(eye),
                image_argument(guide_depth_,ocarina::Usage::READ),
                image_argument(guide_view_normal_,ocarina::Usage::READ),
                image_argument(spatial_samples_,ocarina::Usage::READ),
                image_argument(reservoirs_[pass],ocarina::Usage::READ),
                image_argument(reservoirs_[pass+1u],ocarina::Usage::WRITE)}));
        };
        spatial.function()->set_block_dim(8u,8u);
        if(pass==0u) spatial0_=device().compile(spatial,"RestirGI reference spatial reuse 0");
        else spatial1_=device().compile(spatial,"RestirGI reference spatial reuse 1");
    }

    void compile_resolve(ReSTIRGI &output) {
        auto &camera=scene().sensor();
        Kernel resolve=[&](Uint frame,Float4x4 raster_to_camera,Float4x4 world_to_camera) {
            camera->load_data();
            auto *function=Function::current();
            function->add_header(literal_header_);
            Uint i=dispatch_id();
            Uint2 pixel=dispatch_idx().xy(),extent=make_uint2(extent_),sampling=make_uint2(sampling_);
            Uint scale=scale_;
            Uint random_frame=frame+multibounce_sample_offset_;
            Float4x4 camera_to_world=camera->device_c2w();
            auto evaluate=[&]() -> Float3 {
            if(diagnostic_=="fresh"||diagnostic_=="fresh_visible") {
                // Uniform-hemisphere candidate estimator before any reuse or
                // reconstruction. Use the actual proposal's FP32 normal.
                Float4 receiver=position(pixel,frame);
                Float3 delta=candidate_hit_.read_native<float4>(pixel).xyz();
                Float3 radiance=candidate_radiance_.read_native<float4>(pixel).xyz();
                Float3 result=max(zero_if_nan_inf(radiance*(2.f*max(0.f,dot(normal(pixel,frame),restir_gi::direction(delta))))),make_float3(0.f));
                $if(receiver.w<=0.f) { result=make_float3(0.f); };
                return result;
            }
            return ocarina::eval<float3>(function->call(Type::of<float3>(),"restir_gi_literal_resolve",{
                OC_EXPR(pixel),OC_EXPR(frame),OC_EXPR(random_frame),OC_EXPR(extent),OC_EXPR(sampling),OC_EXPR(scale),
                OC_EXPR(camera_to_world),OC_EXPR(world_to_camera),OC_EXPR(raster_to_camera),
                image_argument(guide_depth_,ocarina::Usage::READ),
                image_argument(guide_view_normal_,ocarina::Usage::READ),
                image_argument(guide_world_normal_,ocarina::Usage::READ),
                image_argument(spatial_samples_,ocarina::Usage::READ),
                image_argument(candidate_hit_,ocarina::Usage::READ),
                image_argument(candidate_radiance_,ocarina::Usage::READ),
                image_argument(selected_radiance_,ocarina::Usage::READ),
                image_argument(reservoirs_[2],ocarina::Usage::READ)}));
            };
            Float3 result=evaluate();
            if(diagnostic_=="fresh_visible"||diagnostic_=="center_visible") {
                Function::current()->require_ray_query_compute_workgroups();
                // Conservative visibility probe only: shorten the segment to
                // avoid treating the FP16 reconstructed endpoint as a blocker.
                // Mask blocked transport without renormalizing any weights.
                $if(any(result>make_float3(0.f))) {
                    Float3 receiver=position(pixel,frame).xyz();
                    Float3 receiver_normal=normal(pixel,frame);
                    Float3 hit;
                    if(diagnostic_=="fresh_visible") {
                        hit=receiver+candidate_hit_.read_native<float4>(pixel).xyz();
                    } else {
                        auto reservoir=restir_gi::unpack(reservoirs_[2].read_native<uint2>(pixel));
                        Uint2 source_pixel=restir_gi::unpack_pixel(reservoir.payload);
                        auto sample=restir_gi::unpack_temporal_output(spatial_samples_.read_native<uint4>(source_pixel));
                        hit=reconstruct(source_pixel,sample.depth,raster_to_camera)+sample.hit_offset;
                    }
                    Float3 delta=hit-receiver;
                    Float side=select(dot(receiver_normal*Interaction::ray_offset_factor(),delta)>0.f,1.f,-1.f);
                    Float3 origin=Interaction::custom_offset_ray_origin(receiver,receiver_normal*side);
                    RayVar probe;
                    probe.org_min=make_float4(origin,0.f);
                    probe.dir_max=make_float4(hit-origin,.995f);
                    $if(geometry().trace_occlusion(probe)) { result=make_float3(0.f); };
                };
            }
            if(feedback_) {
                Float depth=guide_depth_.read_native<float>(pixel);
                feedback_history_.write((frame&1u)*pixels_+i,make_float4(result,select(depth>0.f,1.f,0.f)),false);
            }
            output.write_radiance(i,make_float4(result*albedo_.read(i,false).xyz(),1.f));
        };
        resolve.function()->set_block_dim(8u,8u);
        resolve_=device().compile(resolve,"RestirGI reference resolve");
    }

public:
    RestirGIExperiment(ReSTIRGI &output,std::string_view mode)
        : extent_{pipeline()->resolution()},frame_buffer_{&frame_buffer()},mode_{mode},diagnostic_{diagnostic_requested()} {
        if(mode!="half"&&mode!="full"&&mode!="half_feedback"&&mode!="full_feedback")
            throw std::runtime_error("VISION_RESTIR_GI_MODE must be half, full, half_feedback, or full_feedback");
        scale_=mode.starts_with("half")?2u:1u; feedback_=mode.ends_with("_feedback");
        aligned_lambert_=aligned_lambert_requested();
        const char *mb=std::getenv("VISION_RESTIR_GI_MULTIBOUNCE_BENCH");
        multibounce_bench_=mb&&mb[0]=='1';
        const char *mbdiag=std::getenv("VISION_RESTIR_GI_MB_DIAGNOSTIC");
        multibounce_diagnostics_=multibounce_bench_&&mbdiag&&mbdiag[0]=='1';
        if(multibounce_bench_) {
            const char *offset=std::getenv("VISION_BENCH_SAMPLE_OFFSET");
            if(offset) multibounce_sample_offset_=uint(std::strtoul(offset,nullptr,10));
        }
        if(multibounce_bench_&&(!aligned_lambert_||feedback_||mode_!="full"))
            throw std::runtime_error("Multibounce benchmark requires full aligned Lambert without feedback");
        if(aligned_lambert_&&feedback_) throw std::runtime_error("Aligned Lambert benchmark requires feedback off");
        uint diagnostic_index=0u;
        if(!diagnostic_.empty()) {
            if(diagnostic_=="fresh"||diagnostic_=="fresh_visible") diagnostic_index=1u;
            else if(diagnostic_=="fresh_resolve") diagnostic_index=2u;
            else if(diagnostic_=="temporal") diagnostic_index=3u;
            else if(diagnostic_=="spatial") diagnostic_index=4u;
            else if(diagnostic_=="center"||diagnostic_=="center_visible") diagnostic_index=5u;
            else throw std::runtime_error("VISION_RESTIR_GI_DIAGNOSTIC must be fresh, fresh_resolve, temporal, spatial, center, fresh_visible, or center_visible");
            if(mode_!="full"||!aligned_lambert_||feedback_)
                throw std::runtime_error("ReSTIR GI diagnostic ablation requires full, aligned Lambert, and feedback off");
        }
        sampling_=make_uint2(restir_gi::sampling_extent(extent_.x,scale_),restir_gi::sampling_extent(extent_.y,scale_));
        pixels_=extent_.x*extent_.y; cells_=sampling_.x*sampling_.y;
        // Names include sampling scale because a FrameBuffer retains named allocations.
        std::string prefix="RestirGI reference "+std::to_string(scale_)+" ";
        auto name=[&](const char *suffix){return prefix+suffix;};
        bind(positions_,name("receiver positions").c_str(),1u,2u);
        bind(normals_,name("receiver normals").c_str(),1u,2u);
        bind(albedo_,name("diffuse reflectance").c_str(),1u,1u);
        if(multibounce_diagnostics_) bind(multibounce_path_stats_,name("multibounce path stats").c_str(),1u,1u);
        for(uint phase=0u;phase<2u;++phase) {
            history_[phase].allocate(device(),sampling_,prefix+"history "+std::to_string(phase));
        }
        guide_depth_=device().create_texture2d(extent_,PixelStorage::FLOAT1,name("guide depth"));
        guide_view_normal_=device().create_texture2d(extent_,PixelStorage::SNORM4,name("guide view normal"));
        guide_world_normal_=device().create_texture2d(extent_,PixelStorage::UINT1,name("guide world normal"));
        spatial_samples_=device().create_texture2d(sampling_,PixelStorage::UINT4,name("packed spatial samples"));
        candidate_hit_=device().create_texture2d(sampling_,PixelStorage::HALF4,name("candidate hit"));
        candidate_radiance_=device().create_texture2d(sampling_,PixelStorage::HALF4,name("candidate radiance"));
        candidate_normals_=device().create_texture2d(sampling_,PixelStorage::HALF4,name("candidate normal and cosine"));
        selected_radiance_=device().create_texture2d(sampling_,PixelStorage::HALF4,name("selected radiance"));
        for(uint i=0u;i<reservoirs_.size();++i) {
            reservoirs_[i]=device().create_texture2d(sampling_,PixelStorage::UINT2,prefix+"reservoir "+std::to_string(i));
        }
        bind(invalidity_,name("raw invalidity").c_str(),scale_,1u);
        if(feedback_) {
            bind(feedback_history_,name("feedback irradiance").c_str(),1u,2u);
            bind(reprojected_,name("reprojected irradiance").c_str(),1u,1u);
        }
        auto *context=device().context();
        std::string literal_source{restir_gi::LiteralSource};
        if(diagnostic_index!=0u) literal_source="#define CORONA_RESTIR_GI_DIAGNOSTIC "+std::to_string(diagnostic_index)+"\n"+literal_source;
        std::string library_name="restir_gi_literal_"+std::to_string(hash64(std::string_view{literal_source}))+".slangh";
        context->write_global_cache(library_name,literal_source);
        literal_header_=(context->cache_directory()/library_name).generic_string();
        compile_surface(); compile_tracing(); compile_temporal(); compile_spatial(0u); compile_spatial(1u); compile_resolve(output);
        OC_INFO("RestirGI literal compute kernels active: spatial and resolve");
        OC_INFO_FORMAT("RestirGI reference route active: mode={}, full={}x{}, sampling={}x{}, cache=off, feedback={}",
                       mode_,extent_.x,extent_.y,sampling_.x,sampling_.y,feedback_);
        OC_INFO("RestirGI reference adapters: diffuse resolve, Corona material/light sampling, primary emission/sky in DI, AO=1, no roughness bias, raw feedback");
        if(aligned_lambert_) OC_INFO("RestirGI aligned Lambert benchmark active: albedo=0.7, sun_incoming=(0,1,0), irradiance=1, sky=0, opaque, AO=1, cache=off, feedback=off; native DI excluded");
        if(!diagnostic_.empty()) OC_INFO_FORMAT("RestirGI diagnostic ablation active: mode={}, temporal={}, spatial={}, resolve={}",diagnostic_,
            diagnostic_=="temporal"||diagnostic_=="center"||diagnostic_=="center_visible",diagnostic_=="spatial"||diagnostic_=="center"||diagnostic_=="center_visible",
            diagnostic_=="fresh"||diagnostic_=="fresh_visible"?"own candidate":diagnostic_=="fresh_resolve"?"original near/far gather":"center reservoir, no attenuation");
        if(diagnostic_=="fresh_visible"||diagnostic_=="center_visible")
            OC_INFO("RestirGI conservative visibility diagnostic active: one receiver-to-hit geometry segment, FP32 receiver/normal, FP16 hit, endpoint_fraction=0.995, occlusion masks output, no renormalization");
        OC_INFO("RestirGI formatted 2D textures active: R32F depth, RGBA8_SNORM view normal, R32_UINT world normal, RGBA16F hit/radiance, RGBA32_UINT spatial samples, RG32_UINT reservoirs");
        if(multibounce_bench_) OC_INFO("RestirGI multibounce benchmark: min_depth=5 max_depth=16 rr_threshold=1; primary_surface=1; first_RR_after_surface=6; q=min(0.95,beta_before_BSDF); all_secondary_sun_NEE; geometric_normals; uniform_primary; full_candidate_every_frame; internal_sun=(0,0.4472135955,0.894427191); canonical_to_internal_Z_reflection");
        const size_t receiver_bytes=size_t(pixels_)*92u;
        const size_t history_bytes=size_t(cells_)*48u*2u;
        const size_t candidate_bytes=size_t(cells_)*sizeof(uint2);
        const size_t sample_bytes=history_bytes+candidate_bytes+size_t(cells_)*(sizeof(uint4)*2u+sizeof(uint2));
        const size_t reservoir_bytes=size_t(cells_)*sizeof(uint2)*3u;
        const size_t invalidity_bytes=size_t(cells_)*sizeof(float);
        const size_t feedback_bytes=feedback_?size_t(pixels_)*48u:0u;
        const size_t extra_bytes=receiver_bytes+sample_bytes+reservoir_bytes+invalidity_bytes+feedback_bytes;
        const size_t output_bytes=size_t(pixels_)*(output.compact_radiance()?sizeof(uint2):sizeof(float4));
        OC_INFO_FORMAT("RestirGI reference storage bytes: positions={}, normals={}, albedo={}, temporal={}, candidates={}, spatial_samples={}, spatial_reservoirs={}, invalidity={}, feedback={}, extra={}, active_including_shared_output={}; native GI allocation retained; packed current guides and resolve copies included in extra",
            size_t(pixels_)*32u,size_t(pixels_)*32u,size_t(pixels_)*16u,
            history_bytes,candidate_bytes,
            size_t(cells_)*sizeof(uint4),reservoir_bytes,invalidity_bytes,feedback_bytes,extra_bytes,extra_bytes+output_bytes);
    }
    [[nodiscard]] bool matches(uint2 extent,std::string_view mode) const noexcept {
        return frame_buffer_==&frame_buffer()&&extent.x==extent_.x&&extent.y==extent_.y&&mode==mode_&&
            aligned_lambert_==aligned_lambert_requested()&&diagnostic_==diagnostic_requested();
    }
    // Called only by the existing selected-frame diagnostic path, after GI.
    void download_guides(uint frame,const fs::path &directory) const {
        vector<float4> positions(size_t(pixels_)*2u),normals(size_t(pixels_)*2u),albedo(pixels_);
        pipeline()->stream()<<positions_.view().download(positions.data())
            <<normals_.view().download(normals.data())<<albedo_.view().download(albedo.data())
            <<synchronize()<<commit();
        auto write=[&](const char *name,const float4 *data) {
            std::ofstream out(directory/name,std::ios::binary);
            out.write(reinterpret_cast<const char *>(data),size_t(pixels_)*sizeof(float4));
            if(!out) throw std::runtime_error("Reference GI guide diagnostic write failed");
        };
        write("reference_position_depth.f32",positions.data()+(frame&1u)*pixels_);
        write("reference_world_normal.f32",normals.data()+(frame&1u)*pixels_);
        write("reference_diffuse_albedo.f32",albedo.data());
        if(multibounce_diagnostics_) {
            vector<float4> stats(pixels_);
            pipeline()->stream()<<multibounce_path_stats_.download(stats.data())<<synchronize()<<commit();
            write("multibounce_path_stats.f32",stats.data());
        }
        vector<uint2> selected(cells_);
        pipeline()->stream()<<selected_radiance_.download(selected.data())<<synchronize()<<commit();
        size_t zero_radiance=0u;
        for(auto value:selected) {
            auto rgb=unpack_storage_half4_host(value).xyz();
            if(all(rgb==make_float3(0.f))) ++zero_radiance;
        }
        std::ofstream stats(directory/"reference_selected_radiance_stats.json");
        stats<<"{\"cells\":"<<cells_<<",\"zero_rgb\":"<<zero_radiance<<"}";
    }
    [[nodiscard]] CommandBatch dispatch(uint frame) {
        auto resource=geometry().gpu_resource();
        uint64_t revision=resource->visibility_revision();
        bool history=restir_gi::consecutive_history(previous_frame_,frame)&&previous_geometry_.lock()==resource&&previous_geometry_revision_==revision;
        if(!diagnostic_.empty()&&diagnostic_!="temporal"&&diagnostic_!="center"&&diagnostic_!="center_visible") history=false;
        bool validation=history&&frame%3u==0u;
        previous_frame_=frame; previous_geometry_=resource; previous_geometry_revision_=revision;
        auto &camera=scene().sensor();
        float4x4 camera_to_world=camera->host_c2w();
        float4x4 world_to_camera=inverse(camera_to_world);
        float4x4 raster_to_camera=camera->host_raster_to_sensor();
        float4x4 camera_to_raster=inverse(raster_to_camera);
        CommandBatch batch;
        batch<<prepare_(frame,world_to_camera).dispatch(extent_);
        if(feedback_) batch<<feedback_reproject_(frame,uint(history)).dispatch(extent_);
        batch<<candidates_shader_(frame,uint(multibounce_bench_?false:validation)).dispatch(sampling_);
        if(validation) batch<<validate_[frame&1u](frame).dispatch(sampling_);
        batch<<temporal_[frame&1u](frame,uint(history)|(uint(validation)<<1u)).dispatch(sampling_);
        // Camera transforms are uniform. Upload current matrices once instead
        // of expanding eight general inverses inside every spatial donor loop.
        batch<<spatial0_(frame,0u,world_to_camera,camera_to_raster,raster_to_camera).dispatch(sampling_);
        batch<<spatial1_(frame,0u,world_to_camera,camera_to_raster,raster_to_camera).dispatch(sampling_);
        batch<<resolve_(frame,raster_to_camera,world_to_camera).dispatch(extent_);
        return batch;
    }
};
} // namespace vision

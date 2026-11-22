// Check generic guide-storage and the actual Falcor / RTXPT output codecs,
// including GPU allocation sizes, readback, resize and restore.
#define VISION_PLUGIN_NAME "ReSTIR"
#define VISION_CATEGORY "integrator"
#include "render_core/integrator/ReSTIR/rtxpt_gi.h"
#include "render_core/integrator/ReSTIR/di_radiance.h"
#include "hotfix/hotfix.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

// H2 production SP accessors, literal codec and physical current/history storage.
// The current record is packed; history remains FP32 after decoding.
#include <array>
#include <bit>
#include <cstddef>

namespace half_motion_storage_checks {
using namespace vision;
using namespace ocarina;
struct Literal { uint x, y, packed, decoded_x, decoded_y; };
inline constexpr Literal literals[] = {
    {0x00000000u,0x80000000u,0x80000000u,0x00000000u,0x80000000u},
    {0x3f800000u,0xbf800000u,0xbc003c00u,0x3f800000u,0xbf800000u},
    {0x3f801000u,0x3f803000u,0x3c023c00u,0x3f800000u,0x3f804000u},
    {0x33800000u,0x33000000u,0x00000001u,0x33800000u,0x00000000u},
    {0x33000001u,0x38800000u,0x04000001u,0x33800000u,0x38800000u},
    {0x477fe000u,0xc77fe000u,0xfbff7bffu,0x477fe000u,0xc77fe000u},
    {0x477ff000u,0xc77ff000u,0xfc007c00u,0x7f800000u,0xff800000u},
    {0x3d23d70au,0x3e3851ecu,0x31c3291fu,0x3d23e000u,0x3e386000u},
    {0x447a0000u,0xc47a0000u,0xe3d063d0u,0x447a0000u,0xc47a0000u},
    {0x7f800000u,0xff800000u,0xfc007c00u,0x7f800000u,0xff800000u},
    {0x7fc12345u,0xffc54321u,0xfe007e00u,0x7fc00000u,0xffc00000u}
};
inline bool half_matches(uint actual,uint expected) {
    if((expected&0x7c00u)==0x7c00u && (expected&0x3ffu)!=0u)
        return (actual&0x7c00u)==0x7c00u && (actual&0x3ffu)!=0u;
    return actual==expected;
}
inline bool packed_matches(uint actual,uint expected) {
    return half_matches(actual&65535u,expected&65535u) && half_matches(actual>>16u,expected>>16u);
}
inline bool fp32_matches(uint actual,uint expected) {
    if((expected&0x7f800000u)==0x7f800000u && (expected&0x7fffffu)!=0u)
        return (actual&0x7f800000u)==0x7f800000u && (actual&0x7fffffu)!=0u;
    return actual==expected;
}
struct RestoreProbe final : FrameBuffer {
    [[nodiscard]] string_view impl_type() const noexcept override { return "H2RestoreProbe"; }
    [[nodiscard]] string_view category() const noexcept override { return "framebuffer"; }
    [[nodiscard]] RayState custom_generate_ray(const Sensor*,const SensorSample&) const noexcept override { return {}; }
};
inline void check_restore(Pipeline& pipeline) {
    // sizeof(T) changes GPU records, not the host Buffer<T> metadata layout.
    static_assert(sizeof(RegistrableBuffer<StablePlaneData>)==sizeof(RegistrableBuffer<float4>));
    static_assert(alignof(RegistrableBuffer<StablePlaneData>)==alignof(RegistrableBuffer<float4>));
    const uint first=pipeline.bindless_array()->buffer_num();
    RestoreProbe fresh, restored_empty;
    restored_empty.restore(&fresh);
    if(restored_empty.stable_planes().size()!=0u || !restored_empty.stable_raw_outputs_retained() ||
       !restored_empty.stable_planes().name().ends_with(" [motion-half-coverage-history-v3]"))
        throw std::runtime_error("Compatible empty v3 framebuffer restore failed");
    RestoreProbe previous, restored;
    const char* negative_value=std::getenv("VISION_H2_STORAGE_RESTORE_NEGATIVE");
    const string_view negative=negative_value ? negative_value : "";
    if(negative=="empty" || negative=="v2empty") {
        previous.stable_planes().set_name(negative=="v2empty"
            ? "FrameBuffer::stable_planes [motion-half-coverage-history-v2]" : "");
        restored.restore(&previous);
        throw std::runtime_error("H2 restore wrongly accepted a legacy empty framebuffer");
    }
    previous.init_buffer_impl(previous.stable_planes(),false,"H2 restore fixture",StablePlaneCount,1u);
    previous.manage_stable_plane_buffers(false,false,true,false);
    const auto handle=previous.stable_planes().handle();
    const auto count=previous.stable_planes().size();
    const auto direct_handle=previous.stable_direct().handle(), indirect_handle=previous.stable_indirect().handle();
    const auto direct_index=previous.stable_direct().index().hv(), indirect_index=previous.stable_indirect().index().hv();
    if(!negative.empty()) {
        // Root runs this as an expected-failing subprocess. No GPU work follows
        // this deliberate legacy tag; the real restore must reject before move.
        previous.stable_planes().set_name(negative=="v2allocated"
            ? "FrameBuffer::stable_planes [motion-half-coverage-history-v2]" : "FrameBuffer::stable_planes");
        restored.restore(&previous);
        throw std::runtime_error("H2 restore wrongly accepted a legacy SP allocation tag");
    }
    restored.restore(&previous);
    if(restored.stable_planes().handle()!=handle || restored.stable_planes().size()!=count ||
       !restored.stable_planes().name().ends_with(" [motion-half-coverage-history-v3]"))
        throw std::runtime_error("H2 compatible restore failed to move the existing SP allocation");
    if(restored.stable_raw_outputs_retained() || !restored.compact_coverage_history() || restored.stable_plane_history_retained() ||
       restored.stable_direct().handle()!=direct_handle || restored.stable_indirect().handle()!=indirect_handle ||
       restored.stable_direct().index().hv()!=direct_index || restored.stable_indirect().index().hv()!=indirect_index ||
       restored.stable_direct().size()!=StablePlaneCount || restored.stable_indirect().size()!=StablePlaneCount)
        throw std::runtime_error("Compatible v3 restore must preserve suppressed raw mode and registered stubs");
    ByteBufferView empty{handle_ty{0u},0u};
    const uint end=pipeline.bindless_array()->buffer_num();
    for(uint slot=first;slot<end;++slot) pipeline.bindless_array().set_buffer(slot,empty);
}
inline void run(Pipeline& pipeline) {
    static_assert(sizeof(SurfaceData)==160u && sizeof(StablePlaneData)==240u);
    static_assert(sizeof(StablePlaneHistory)==96u);
    static_assert(offsetof(StablePlaneData,motion_storage)==204u);
    static_assert(offsetof(StablePlaneData,depth_position)==208u);
    static_assert(offsetof(StablePlaneData,material_id)==232u);
    static_assert(offsetof(StablePlaneData,flags)==236u);
    check_restore(pipeline);
    auto require=[](bool okay,const char* message) { if(!okay) throw std::runtime_error(message); };
    for(const auto& row:literals) {
        StablePlaneData p;
        p.set_motion(make_float2(std::bit_cast<float>(row.x),std::bit_cast<float>(row.y)));
        require(packed_matches(p.motion_storage,row.packed),"H2 host motion encoder literal mismatch");
        const auto motion=p.motion();
        require(fp32_matches(std::bit_cast<uint>(motion.x),row.decoded_x) && fp32_matches(std::bit_cast<uint>(motion.y),row.decoded_y),
                "H2 host motion decoder literal mismatch");
        p.flags=0xa5a50000u;
        p.set_valid(1u); p.set_reservoir_eligible(1u); p.set_valid(0u);
        require(p.flags==0xa5a50002u && p.valid()==0u && p.reservoir_eligible()==1u,
                "H2 validity setter clobbered eligibility or reserved bits");
        p.set_valid(1u); p.set_reservoir_eligible(0u);
        require(p.flags==0xa5a50001u && p.valid()==1u && p.reservoir_eligible()==0u,
                "H2 eligibility setter clobbered validity or reserved bits");
    }
    Kernel store=[](BufferVar<uint2> input,BufferVar<StablePlaneData> output,Uint count) {
        configure_block(64u); Uint i=dispatch_id();
        $if(i<count) {
            StablePlaneDataVar p;
            p->set_motion(as<float2>(input.read(i)));
            p->set_valid(i&1u); p->set_reservoir_eligible((i>>1u)&1u);
            output.write(i+1u,p);
        };
    };
    Kernel load=[](BufferVar<StablePlaneData> input,BufferVar<uint4> output,
                   BufferVar<StablePlaneHistory> history,Uint count) {
        configure_block(64u); Uint i=dispatch_id();
        $if(i<count) {
            auto p=input.read((i+1u)%count+1u);
            output.write(i,make_uint4(as<uint2>(p->motion()),p->valid(),p->reservoir_eligible()));
            history.write(i,p->history());
        };
    };
    auto store_shader=pipeline.device().compile(store,"H2 SP motion literal physical store");
    auto load_shader=pipeline.device().compile(load,"H2 SP motion cross-pixel decode and history");
    for(uint width:{63u,64u,65u}) {
        const uint count=width*3u;
        std::vector<uint2> inputs(count);
        std::vector<uint4> decoded(count);
        std::vector<uint> words((count+2u)*60u,0xdeadbeefu);
        std::vector<StablePlaneHistory> histories(count);
        for(uint i=0;i<count;++i) { const auto& row=literals[i%std::size(literals)]; inputs[i]=make_uint2(row.x,row.y); }
        auto input=pipeline.device().create_buffer<uint2>(count,"H2 motion literal input");
        auto physical=pipeline.device().create_buffer<StablePlaneData>(count+2u,"H2 motion physical record");
        auto output=pipeline.device().create_buffer<uint4>(count,"H2 motion decoded output");
        auto history=pipeline.device().create_buffer<StablePlaneHistory>(count,"H2 motion decoded history");
        require(physical.size_in_byte()==size_t(count+2u)*240u,"H2 physical SP stride must be240B");
        pipeline.stream()<<input.upload(inputs.data())<<physical.view_as<uint>().upload(words.data())
            <<store_shader(input,physical,count).dispatch(count)
            <<load_shader(physical,output,history,count).dispatch(count)
            <<physical.view_as<uint>().download(words.data())<<output.download(decoded.data())
            <<history.download(histories.data())<<synchronize()<<commit();
        for(uint word=0;word<60u;++word)
            require(words[word]==0xdeadbeefu && words[(count+1u)*60u+word]==0xdeadbeefu,
                    "H2 SP physical store crossed record canaries");
        for(uint i=0;i<count;++i) {
            const auto& written=literals[i%std::size(literals)];
            require(packed_matches(words[(i+1u)*60u+51u],written.packed) && words[(i+1u)*60u+59u]==(i&3u),
                    "H2 physical motion/flags words mismatch");
            const uint source=(i+1u)%count;
            const auto& expected=literals[source%std::size(literals)];
            require(fp32_matches(decoded[i].x,expected.decoded_x) && fp32_matches(decoded[i].y,expected.decoded_y) &&
                    decoded[i].z==(source&1u) && decoded[i].w==((source>>1u)&1u),
                    "H2 cross-pixel motion/flags decode mismatch");
            require(fp32_matches(std::bit_cast<uint>(histories[i].motion.x),expected.decoded_x) &&
                    fp32_matches(std::bit_cast<uint>(histories[i].motion.y),expected.decoded_y) && histories[i].valid==(source&1u),
                    "H2 history must preserve decoded motion/validity exactly");
        }
    }
    std::cout<<"PASS: H2 motion literals, flags,240B physical records, cross-pixel decode and96B history\n";
}
} // namespace half_motion_storage_checks


namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename Restir>
void check_radiance_storage(vision::Pipeline& pipeline, const char *kind) {
    using namespace vision;
    // Independent physical words: catches a mutually consistent pack/unpack
    // permutation, rounding bias, HDR clipping, or lost sign/validity bit.
    constexpr uint input[][4] = {
        {0x00000000u,0x80000000u,0x00000001u,0x00000000u},
        {0x000003ffu,0x00000400u,0x00000401u,0x3f800000u},
        {0x000007ffu,0x00000800u,0x007fffffu,0x00000000u},
        {0x00800000u,0x3f800000u,0x3f8003ffu,0x3f800000u},
        {0x3f800400u,0x3f800401u,0x3f800bffu,0x00000000u},
        {0x3f800c00u,0x3f800c01u,0xbf800400u,0x3f800000u},
        {0xbf800c00u,0x477fe000u,0x47c35000u,0x00000000u},
        {0x7f7ff800u,0x7f7ffc00u,0x7f7fffffu,0x3f800000u},
        {0xff7fffffu,0x7f800000u,0xff800000u,0x00000000u},
        {0x7f800001u,0x7fc12345u,0xffa12345u,0x3f800000u}};
    constexpr uint packed[][2] = {
        {0x00000000u,0x00000200u},
        {0x00000000u,0x80000400u},
        {0x00200001u,0x00400000u},
        {0x00001000u,0x9fc000feu},
        {0x0027f000u,0x1fc004feu},
        {0x0047f002u,0xdfc000feu},
        {0xff97f002u,0x23e1a91du},
        {0xffefefffu,0xbfbffdfdu},
        {0x001fefffu,0x7fc001feu},
        {0x048ff800u,0xfff091ffu}};
    constexpr uint decoded[][4] = {
        {0x00000000u,0x80000000u,0x00000000u,0x00000000u},
        {0x00000000u,0x00000000u,0x00000800u,0x3f800000u},
        {0x00000800u,0x00000800u,0x00800000u,0x00000000u},
        {0x00800000u,0x3f800000u,0x3f800000u,0x3f800000u},
        {0x3f800000u,0x3f800800u,0x3f800800u,0x00000000u},
        {0x3f801000u,0x3f801000u,0xbf800000u,0x3f800000u},
        {0xbf801000u,0x477fe000u,0x47c35000u,0x00000000u},
        {0x7f7ff800u,0x7f7ff800u,0x7f7ff800u,0x3f800000u},
        {0xff7ff800u,0x7f800000u,0xff800000u,0x00000000u},
        {0x7fc00000u,0x7fc12000u,0xffe12000u,0x3f800000u}};
    constexpr uint literal_count = sizeof(input) / sizeof(input[0]);
    const auto original_resolution = pipeline.resolution();
    const uint first_slot = pipeline.bindless_array().buffer_num();
    auto di = std::make_unique<Restir>();
    if constexpr (std::is_same_v<Restir, RtxptGI>) {
        auto unprepared = std::make_unique<Restir>();
        if (std::getenv("CORONA_TEST_GI_RADIANCE_OLD_ABI"))
            unprepared->radiance()->set_name("ReSTIRGI::radiance_ legacy");
        di->restore(unprepared.get());
    }
    di->radiance()->set_name(std::string(kind) + "_radiance_storage_test");
    di->prepare(true);
    auto exercise = [&](bool compact) {
        pipeline.stream() << synchronize() << commit();
        di->set_compact_radiance(compact);
        pipeline.upload_bindless_array();
        if (!compact) {
            auto initial = di->download_radiance();
            for (const auto &value : initial) for (uint k=0u;k<4u;++k)
                require(value[k]==0.f,"New FP32 ReSTIR output must be zero before a disabled producer can skip it");
        }
        const uint n = pipeline.pixel_num();
        std::vector<uint> host(n*4u), actual(n*4u);
        for (uint i=0u;i<n;++i) for(uint k=0u;k<4u;++k) host[4u*i+k]=input[i%literal_count][k];
        auto source=pipeline.device().create_buffer<uint>(host.size(),"radiance_literal_source");
        auto dest=pipeline.device().create_buffer<uint>(host.size(),"radiance_literal_result");
        source.upload_immediately(host.data());
        const bool corrupt = compact && std::getenv((std::string("CORONA_TEST_") + kind + "_RADIANCE_WRONG_WORD").c_str()) != nullptr;
        Kernel store_kernel = [&](BufferVar<uint> src) {
            Uint i=dispatch_id();
            Float4 value=make_float4(as<float>(src.read(i*4u)),as<float>(src.read(i*4u+1u)),
                                    as<float>(src.read(i*4u+2u)),as<float>(src.read(i*4u+3u)));
            di->write_radiance(i,value);
            if(corrupt) {
                auto raw=pipeline.bindless_array().buffer_var<uint2>(di->raw_radiance().index_var());
                Uint2 words=raw.read(i); words.x ^= 1u; raw.write(i,words);
            }
        };
        Kernel load_kernel = [&](BufferVar<uint> dst) {
            Uint i=dispatch_id(); Float4 value=di->read_radiance(i);
            dst.write(i*4u,as<uint>(value.x)); dst.write(i*4u+1u,as<uint>(value.y));
            dst.write(i*4u+2u,as<uint>(value.z)); dst.write(i*4u+3u,as<uint>(value.w));
        };
        auto store=pipeline.device().compile(store_kernel,"ReSTIR radiance literal store");
        auto load=pipeline.device().compile(load_kernel,"ReSTIR radiance literal load");
        pipeline.stream()<<store(source).dispatch(n)<<load(dest).dispatch(n)
                         <<dest.download(actual.data())<<synchronize()<<commit();
        for(uint i=0u;i<n;++i) for(uint k=0u;k<4u;++k)
            require(actual[4u*i+k]==(compact?decoded[i%literal_count][k]:input[i%literal_count][k]),
                    "ReSTIR radiance decoded values differ from independent literals");
        auto downloaded=di->download_radiance();
        for(uint i=0u;i<n;++i) for(uint k=0u;k<4u;++k)
            require(std::bit_cast<uint>(downloaded[i][k])==actual[4u*i+k],"ReSTIR radiance CPU diagnostic decode differs");
        if(compact) {
            const auto desc=pipeline.bindless_array()->buffer_view(di->raw_radiance().index().hv());
            require(desc.size==size_t(n)*8u && di->radiance()->view().size()==0u,
                    "Compact ReSTIR output must own only one 8-byte-per-pixel buffer");
            std::vector<uint2> words(n);
            pipeline.stream()<<di->raw_radiance().view().download(words.data())<<synchronize()<<commit();
            for(uint i=0u;i<n;++i) require(words[i].x==packed[i%literal_count][0]&&words[i].y==packed[i%literal_count][1],
                                          "ReSTIR radiance physical words differ from independent literals");
        } else {
            require(di->radiance()->view().size_in_byte()==size_t(n)*16u && di->raw_radiance().size()==0u,
                    "FP32 denoiser output must release compact storage and preserve all four channels");
        }
    };
    exercise(true); exercise(false); exercise(true);
    if constexpr (std::is_same_v<Restir, RtxptGI>) {
        auto restored = std::make_unique<Restir>();
        restored->restore(di.get());
        di = std::move(restored);
        exercise(true);
    }
    pipeline.change_resolution(make_uint2(9u,7u)); di->update_resolution(pipeline.resolution()); exercise(true);
    for (uint width : {63u, 64u, 65u}) {
        pipeline.change_resolution(make_uint2(width, 3u));
        di->update_resolution(pipeline.resolution());
        exercise(true); exercise(false); exercise(true);
    }
    pipeline.change_resolution(original_resolution); di->update_resolution(original_resolution); exercise(true);
    ByteBufferView empty{handle_ty{0u},0u};
    for(uint slot=first_slot;slot<pipeline.bindless_array().buffer_num();++slot)
        pipeline.bindless_array().set_buffer(slot,empty);
    di.reset(); pipeline.invalidate();
    std::cout<<kind<<" PASS: ReSTIR compact radiance literals, native capacity, diagnostics, mode switches and resize\n";
}

} // namespace

void check_stable_plane_storage_restore(vision::Pipeline& pipeline) {
    pipeline.activate_global_context();
    vision::Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    half_motion_storage_checks::check_restore(pipeline);
    std::cout << "PASS: stable-plane storage ABI guard and raw allocation mode restoration\n";
}

void check_restir_reservoir_lifetimes(vision::Pipeline& pipeline) {
    pipeline.activate_global_context();
    vision::Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    half_motion_storage_checks::run(pipeline);
    check_radiance_storage<vision::DIRadiance>(pipeline, "DI");
    check_radiance_storage<vision::RtxptGI>(pipeline, "GI");
}

void check_populated_gi_restore(vision::Pipeline& pipeline) {
    using namespace vision;
    pipeline.activate_global_context();
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    pipeline.stream() << synchronize() << commit();

    struct Constructor final : ObjectConstructor<RtxptGI> {
        mutable std::weak_ptr<RtxptGI> replacement;
        SP<RuntimeObject> construct_shared_impl() const override {
            auto gi = std::static_pointer_cast<RtxptGI>(ObjectConstructor<RtxptGI>::construct_shared_impl());
            replacement = gi;
            return gi;
        }
    } constructor;
    auto *observer = dynamic_cast<Observer*>(pipeline.renderer().integrator().get());
    require(observer != nullptr, "ReSTIR integrator must observe GI hotfixes");
    observer->update_runtime_object(&constructor);
    const auto gi = constructor.replacement.lock();
    require(bool(gi), "ReSTIR integrator must retain the replacement GI object");
    const auto previous = gi->download_radiance();
    pipeline.upload_data();
    pipeline.display(1.0 / 60.0);
    const auto current = gi->download_radiance();
    double energy = 0.;
    bool changed = false;
    for (unsigned y = 4; y < 12; ++y) {
        for (unsigned x = 4; x < 12; ++x) {
            const auto i = y * pipeline.resolution().x + x;
            for (unsigned c = 0; c < 3; ++c) {
                const float value = current[i][c];
                require(std::isfinite(value) && value >= 0.f, "restored GI must produce finite nonnegative radiance");
                energy += value;
                changed |= value != previous[i][c];
            }
        }
    }
    require(energy > 0. && changed, "restored GI must write fresh indirect light on the next frame");
    std::cout << "restored_two_plane_gi_mean=" << energy / (8 * 8 * 3) << '\n';
}

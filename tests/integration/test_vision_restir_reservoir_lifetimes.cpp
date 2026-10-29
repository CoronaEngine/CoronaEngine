// Exercise the production reservoir bindings without ray queries. Cross-pixel
// gathers make an in-dispatch read/write alias observable; device allocation
// queries check capacity instead of relying on untracked MemoryStats counters.
#define VISION_PLUGIN_NAME "ReSTIR"
#define VISION_CATEGORY "integrator"
#include "render_core/integrator/ReSTIR/direct.h"
#include "render_core/integrator/ReSTIR/indirect.h"

#include <algorithm>
#include <bit>
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
    RestoreProbe previous, restored;
    previous.init_buffer_impl(previous.stable_planes(),false,"H2 restore fixture",StablePlaneCount,1u);
    const auto handle=previous.stable_planes().handle();
    const auto count=previous.stable_planes().size();
    if(const char* negative=std::getenv("VISION_H2_STORAGE_RESTORE_NEGATIVE"); negative && *negative) {
        // Root runs this as an expected-failing subprocess. No GPU work follows
        // this deliberate legacy tag; the real restore must reject before move.
        previous.stable_planes().set_name("FrameBuffer::stable_planes");
        restored.restore(&previous);
        throw std::runtime_error("H2 restore wrongly accepted a legacy SP allocation tag");
    }
    restored.restore(&previous);
    if(restored.stable_planes().handle()!=handle || restored.stable_planes().size()!=count ||
       !restored.stable_planes().name().ends_with(" [motion-half-flags-v1]"))
        throw std::runtime_error("H2 compatible restore failed to move the existing SP allocation");
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
    auto unprepared = std::make_unique<Restir>();
    if (std::getenv((std::string("CORONA_TEST_") + kind + "_RADIANCE_OLD_ABI").c_str()) != nullptr)
        unprepared->radiance()->set_name("ReSTIRDI::radiance_ legacy");
    di->restore(unprepared.get());
    di->radiance() = make_shared<ScreenBuffer>("di_radiance_storage_test");
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
    auto restored=std::make_unique<Restir>();
    restored->restore(di.get()); di=std::move(restored); exercise(true);
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
    std::cout<<kind<<" PASS: ReSTIR compact radiance literals, native capacity, diagnostics, mode switches, restore and resize\n";
}

// Called with the production ReSTIRDI instance and its registered slices.
// Literal input words are independent of the production codec in both directions.
void check_di_storage_words(vision::Pipeline& pipeline, vision::ReSTIRDI& restir) {
    using namespace vision;
    const uint count = pipeline.pixel_num();
    constexpr uint patterns[] = {0u, 0x80000000u, 1u, 0x007fffffu, 0x00800000u,
        0x3f800000u, 0xbf800000u, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u,
        0xff800000u, 0x7fc12345u, 0xffffffffu, 0x01234567u, 0x89abcdefu, 0x7fffffffu};
    std::vector<uint> expected(count * 8u);
    for (uint p = 0u; p < count; ++p) {
        for (uint word = 0u; word < 8u; ++word) expected[p * 8u + word] = patterns[(p + 3u * word) % 16u];
    }
    constexpr uint defaults[8] = {0u, 0u, 0u, 0xffffffffu, 0u, 0u, 0u, 0u};
    DIReservoirStorage initial;
    for (uint word = 0u; word < 8u; ++word) {
        expected[word] = defaults[word];
        require(initial.words[word] == defaults[word], "DI physical default must match the logical invalid reservoir");
    }
    auto input = pipeline.device().create_buffer<uint>(expected.size(), "di32_input_words");
    auto output = pipeline.device().create_buffer<uint>(expected.size(), "di32_output_words");
    auto metadata = pipeline.device().create_buffer<uint>(2u, "di32_metadata");
    input.upload_immediately(expected.data());
    const bool corrupt_word = std::getenv("CORONA_TEST_DI_STORAGE_WRONG_WORD") != nullptr;
    Kernel store_kernel = [&](BufferVar<uint> source) {
        Env::instance().clear_global_vars();
        restir.emplace_frame_index(0u);
        Uint p = dispatch_id(), base = p * 8u;
        DIReservoirVar value;
        value.W = as<float>(source.read(base));
        value.C = as<float>(source.read(base + 1u));
        value.sample.p_hat = as<float>(source.read(base + 2u));
        value.sample.light_index = source.read(base + 3u);
        value.sample.prim_id = source.read(base + 4u);
        value.sample.bary = make_float2(as<float>(source.read(base + 5u)), as<float>(source.read(base + 6u)));
        value.sample.age = source.read(base + 7u);
        value.weight_sum = 123.f;
        value.sample->set_pos(make_float3(17.f, 19.f, 23.f));
        restir.passthrough_reservoirs().write(p, value);
        if (corrupt_word) {
            auto raw = pipeline.bindless_array().buffer_var<uint>(restir.reservoir_base() + 1u);
            raw.write(base + 5u, source.read(base + 5u) ^ 1u);
        }
    };
    Kernel load_kernel = [&](BufferVar<uint> destination, BufferVar<uint> binding) {
        Env::instance().clear_global_vars();
        restir.emplace_frame_index(0u);
        Uint p = dispatch_id(), base = p * 8u;
        DIReservoirVar value = restir.passthrough_reservoirs().read(p);
        destination.write(base, as<uint>(value.W));
        destination.write(base + 1u, as<uint>(value.C));
        destination.write(base + 2u, as<uint>(value.sample.p_hat));
        destination.write(base + 3u, value.sample.light_index);
        destination.write(base + 4u, value.sample.prim_id);
        destination.write(base + 5u, as<uint>(value.sample.bary.x));
        destination.write(base + 6u, as<uint>(value.sample.bary.y));
        destination.write(base + 7u, value.sample.age);
        $if(p == 0u) {
            binding.write(0u, restir.reservoir_base());
            binding.write(1u, as<uint>(value.weight_sum) | as<uint>(value.sample.pos[0]) |
                as<uint>(value.sample.pos[1]) | as<uint>(value.sample.pos[2]));
        };
    };
    auto store = pipeline.device().compile(store_kernel, "DI32 production storage write");
    auto read = pipeline.device().compile(load_kernel, "DI32 production storage read");
    std::vector<uint> actual(expected.size());
    uint binding_words[2]{};
    pipeline.stream() << store(input).dispatch(count) << read(output, metadata).dispatch(count)
                      << output.download(actual.data()) << metadata.download(binding_words)
                      << synchronize() << commit();
    require(actual == expected, "DI storage changed a live FP32 bit pattern, full-width ID or age");
    require(binding_words[1] == 0u, "DI transient streaming weight and endpoint must decode to logical defaults");
    const auto b = pipeline.bindless_array()->buffer_view(binding_words[0] + 1u);
    require(b.size == size_t(count) * 32u, "DI persistent storage must use exactly 32 bytes per pixel");
    BufferView<uint> physical{b.head(), b.offset / sizeof(uint), b.size / sizeof(uint),
                              (b.offset + b.size) / sizeof(uint)};
    pipeline.stream() << physical.download(actual.data()) << synchronize() << commit();
    require(actual == expected, "DI physical word order differs from the independent 8-word storage contract");
    std::cout << "PASS: DI32 live-field bit roundtrip and independent physical word order\n";
}


template<typename Impl, typename Reservoir, typename ReservoirVar>
void check_slices(vision::Pipeline& pipeline, const char* label) {
    using namespace vision;
    const auto original_resolution = pipeline.resolution();
    auto restir = std::make_unique<Impl>();
    restir->radiance() = make_shared<ScreenBuffer>(std::string("reservoir_lifetime_") + label);
    const uint first_temporary_slot = pipeline.bindless_array().buffer_num();
    restir->prepare();
    const uint after_temporary_slots = pipeline.bindless_array().buffer_num();
    pipeline.upload_bindless_array();

    // These shaders retain captured EncodedData parameters through restore and
    // resize, matching the lifetime required of the actual precompiled passes.
    Kernel temporal_kernel = [&](Uint frame, Uint reuse, Uint count) {
        Env::instance().clear_global_vars();
        restir->emplace_frame_index(frame);
        Uint p = dispatch_id();
        ReservoirVar value;
        value.W = cast<float>(frame * 1000u + p + 1u);
        value.C = 0.f;
        $if(reuse != 0u) {
            value.C = restir->prev_reservoirs().read((p + count - 1u) % count).W;
        };
        restir->passthrough_reservoirs().write(p, value);
    };
    auto temporal = pipeline.device().compile(temporal_kernel, std::string("reservoir_lifetime_temporal_") + label);
    Kernel spatial_kernel = [&](Uint frame, Uint spatial, Uint count) {
        Env::instance().clear_global_vars();
        restir->emplace_frame_index(frame);
        Uint p = dispatch_id();
        Uint donor = ocarina::select(spatial != 0u, (p + 1u) % count, p);
        restir->cur_reservoirs().write(p, restir->passthrough_reservoirs().read(donor));
    };
    auto spatial = pipeline.device().compile(spatial_kernel, std::string("reservoir_lifetime_spatial_") + label);
    Kernel read_kernel = [&](Uint frame, BufferVar<float2> output, BufferVar<uint> metadata) {
        Env::instance().clear_global_vars();
        restir->emplace_frame_index(frame);
        auto value = restir->cur_reservoirs().read(dispatch_id());
        output.write(dispatch_id(), make_float2(value.W, value.C));
        $if(dispatch_id() == 0u) { metadata.write(0u, restir->reservoir_base()); };
    };
    auto read = pipeline.device().compile(read_kernel, std::string("reservoir_lifetime_read_") + label);
    auto output = pipeline.device().create_buffer<float2>(pipeline.pixel_num(), "reservoir_lifetime_output");
    auto metadata = pipeline.device().create_buffer<uint>(1u, "reservoir_lifetime_metadata");
    std::vector<float2> values(pipeline.pixel_num());
    std::vector<float2> previous(values.size(), make_float2(0.f));
    uint base = 0u;
    auto run = [&](uint frame, bool reuse, bool spatial_reuse, bool open) {
        const uint count = pipeline.pixel_num();
        pipeline.stream() << temporal(frame, uint(reuse), count).dispatch(pipeline.resolution());
        if (open) pipeline.stream() << spatial(frame, uint(spatial_reuse), count).dispatch(pipeline.resolution());
        pipeline.stream() << read(frame, output, metadata).dispatch(pipeline.resolution())
                          << output.view(0u, count).download(values.data()) << metadata.download(&base)
                          << synchronize() << commit();
        for (uint p = 0u; p < count; ++p) {
            const uint donor = spatial_reuse ? (p + 1u) % count : p;
            const float expected_w = open ? float(frame * 1000u + donor + 1u) : previous[p].x;
            const float expected_c = open ? (reuse ? previous[(donor + count - 1u) % count].x : 0.f) : previous[p].y;
            require(values[p].x == expected_w && values[p].y == expected_c,
                    "ReSTIR temporal/spatial bindings changed a cross-pixel history value or overwrote closed DI history");
        }
        previous = values;
        const auto a = pipeline.bindless_array()->buffer_view(base);
        const auto b = pipeline.bindless_array()->buffer_view(base + 1u);
        const auto allocated_bytes = pipeline.device().get_aligned_memory_size(a.head());
        constexpr size_t stride = std::is_same_v<Impl, ReSTIRDI> ? 32u : sizeof(Reservoir);
        const size_t slice_bytes = count * stride;
        require(allocated_bytes >= 2u * slice_bytes && allocated_bytes < 3u * slice_bytes,
                "ReSTIR device allocation must contain two reservoir slices, allowing alignment but excluding a third");
        require(a.head() == b.head() && a.offset == 0u && b.offset == slice_bytes &&
                    a.size == slice_bytes && b.size == slice_bytes,
                "ReSTIR final/scratch descriptors must be two disjoint complete screen views after prepare/resize");
        if constexpr (std::is_same_v<Impl, ReSTIRDI>) {
            require(di_reservoir_storage_matches(DIReservoirStorage::Name, a, b, count),
                    "DI storage must accept its exact current ABI");
            require(!di_reservoir_storage_matches("ReSTIRDI::reservoirs_ x 2", a, b, count),
                    "DI storage must reject an unversioned ABI even at the same stride");
            auto aliased = b;
            aliased.offset = 0u;
            require(!di_reservoir_storage_matches(DIReservoirStorage::Name, a, aliased, count),
                    "DI storage must reject overlapping history and scratch");
            auto old_stride = b;
            old_stride.size = count * 56u;
            require(!di_reservoir_storage_matches(DIReservoirStorage::Name, a, old_stride, count),
                    "DI storage must reject a stale 56-byte view");
        }
    };
    run(0u, false, true, true);
    run(1u, true, true, true);
    run(2u, false, false, true); // Temporal disabled, spatial disabled.
    run(3u, true, false, true);  // Temporal reenabled from the fresh final.
    if constexpr (std::is_same_v<Impl, ReSTIRDI>) {
        run(4u, true, true, false); // DI closed: shader0 runs; shader1 is skipped.
        run(5u, true, true, false);
        run(0u, false, true, true); // Caller invalidates when reopening DI.
    }
    run(1u, true, true, true);
    run(0u, false, false, true); // Camera jitter/scene/frame reset rejects history.
    run(1u, true, true, true);

    auto restored = std::make_unique<Impl>();
    if constexpr (std::is_same_v<Impl, ReSTIRDI>) {
        if (std::getenv("CORONA_TEST_DI_STORAGE_RESTORE_MISMATCH") != nullptr) {
            const auto a = pipeline.bindless_array()->buffer_view(base);
            // No GPU access follows this deliberately invalid descriptor.
            // restore must reject it before moving buffers or compiled shaders.
            pipeline.bindless_array()->set_buffer(base + 1u, a.head(), 0u, a.size);
        }
    }
    restored->restore(restir.get());
    restir = std::move(restored);
    run(2u, true, true, true); // Already compiled shaders use moved binding storage.

    const uint2 smaller = make_uint2(std::max(original_resolution.x / 2u, 1u),
                                     std::max(original_resolution.y / 2u, 1u));
    pipeline.change_resolution(smaller);
    restir->update_resolution(smaller);
    pipeline.upload_bindless_array();
    values.resize(pipeline.pixel_num());
    previous.assign(values.size(), make_float2(0.f));
    run(0u, false, true, true);
    run(1u, true, true, true);

    if constexpr (std::is_same_v<Impl, ReSTIRDI>) { check_di_storage_words(pipeline, *restir); }

    pipeline.frame_buffer()->unregister(restir->radiance());
    // The Vulkan allocator appends registrations, while remove_buffer compacts
    // all later indices. Clear the actual temporary range without renumbering
    // live renderer slots; this also includes GI's independently registered sample.
    ByteBufferView empty{handle_ty{0u}, 0u};
    for (uint slot = first_temporary_slot; slot < after_temporary_slots; ++slot) {
        pipeline.bindless_array().set_buffer(slot, empty);
    }
    restir.reset();
    pipeline.change_resolution(original_resolution);
    pipeline.invalidate();
    std::cout << "PASS: ReSTIR " << label << " two-slice capacity, parity, gathers, pass toggles, reset, restore and resize\n";
}
} // namespace

void check_restir_reservoir_lifetimes(vision::Pipeline& pipeline) {
    pipeline.activate_global_context();
    vision::Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    half_motion_storage_checks::run(pipeline);
    check_slices<vision::ReSTIRDI, vision::DIReservoir, vision::DIReservoirVar>(pipeline, "DI");
    check_slices<vision::ReSTIRGI, vision::GIReservoir, vision::GIReservoirVar>(pipeline, "GI");
    check_radiance_storage<vision::ReSTIRDI>(pipeline, "DI");
    check_radiance_storage<vision::ReSTIRGI>(pipeline, "GI");
}

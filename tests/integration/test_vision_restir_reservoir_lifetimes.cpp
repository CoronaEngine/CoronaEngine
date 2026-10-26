// Exercise the production reservoir bindings without ray queries. Cross-pixel
// gathers make an in-dispatch read/write alias observable; device allocation
// queries check capacity instead of relying on untracked MemoryStats counters.
#define VISION_PLUGIN_NAME "ReSTIR"
#define VISION_CATEGORY "integrator"
#include "render_core/integrator/ReSTIR/direct.h"
#include "render_core/integrator/ReSTIR/indirect.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
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
    check_slices<vision::ReSTIRDI, vision::DIReservoir, vision::DIReservoirVar>(pipeline, "DI");
    check_slices<vision::ReSTIRGI, vision::GIReservoir, vision::GIReservoirVar>(pipeline, "GI");
}

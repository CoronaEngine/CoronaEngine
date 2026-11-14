#pragma once
#include "render_core/denoiser/ReBLUR/filter.h"
#include "rhi/device.h"
#include "rhi/resources/stream.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace vision::rtxpt_gi {
static_assert(sizeof(reblur::Guide) == 80u && offsetof(reblur::Guide, identity) == 16u);
// Explicit debug readback only. Views refer to live producer or filtered data;
// caller chooses the stage after all queued writers. No renderer state changes.
struct NativeReblurDiagnosticViews {
    BufferView<float4> di_diffuse, di_specular, di_bypass;
    array<BufferView<float4>, 3> gi_diffuse, gi_specular;
    BufferView<float4> gi_stable;
    array<BufferView<std::byte>, 3> gi_guides; // actual AOS Guide80
};

[[nodiscard]] inline auto native_reblur_signal_gather_kernel() {
    Kernel kernel = [](BufferVar<uint> indices, BufferVar<float4> source,
                       BufferVar<float4> output, Uint count, Uint channel) {
        Uint i = dispatch_id();
        $if(i < count) { output.write(channel * count + i, source.read(indices.read(i))); };
    };
    kernel.function()->set_block_dim(64u, 1u, 1u);
    return kernel;
}
[[nodiscard]] inline auto native_reblur_guide_gather_kernel() {
    Kernel kernel = [](BufferVar<uint> indices, BufferVar<reblur::Guide> source,
                       BufferVar<reblur::Guide> output, Uint count, Uint plane) {
        Uint i = dispatch_id();
        $if(i < count) { output.write(plane * count + i, source.read(indices.read(i))); };
    };
    kernel.function()->set_block_dim(64u, 1u, 1u);
    return kernel;
}
[[nodiscard]] inline vector<uint> native_reblur_diagnostic_indices(uint2 size) {
    const uint64_t pixels = uint64_t(size.x) * size.y;
    if (pixels == 0u || pixels > 0xffffffffull) throw std::runtime_error("Invalid native REBLUR diagnostic resolution");
    const uint count = uint(std::min<uint64_t>(8192u, pixels));
    vector<uint> indices(count);
    for (uint i = 0u; i < count; ++i) indices[i] = uint((uint64_t(2u * i + 1u) * pixels) / (2u * count));
    return indices;
}

inline void download_native_reblur_diagnostics(Device &device, Stream &stream,
                                               const NativeReblurDiagnosticViews &views, uint2 size,
                                               const std::filesystem::path &directory,
                                               const std::string &stage) {
    const auto indices = native_reblur_diagnostic_indices(size);
    const uint count = uint(indices.size()), pixels = size.x * size.y;
    const array<BufferView<float4>, 10> inputs{views.di_diffuse, views.di_specular, views.di_bypass,
        views.gi_diffuse[0], views.gi_specular[0], views.gi_diffuse[1], views.gi_specular[1],
        views.gi_diffuse[2], views.gi_specular[2], views.gi_stable};
    for (const auto &view : inputs) if (view.size() != pixels) throw std::runtime_error("Native diagnostic signal extent mismatch");
    for (const auto &view : views.gi_guides) if (view.size() != size_t(pixels) * sizeof(reblur::Guide))
        throw std::runtime_error("Native diagnostic AOS guide extent mismatch");
    auto index_buffer = device.create_buffer<uint>(count);
    auto signals = device.create_buffer<float4>(size_t(count) * 10u);
    auto guides = device.create_buffer<reblur::Guide>(size_t(count) * 3u);
    auto signal_shader = device.compile(native_reblur_signal_gather_kernel(), "Native REBLUR diagnostic signal gather");
    auto guide_shader = device.compile(native_reblur_guide_gather_kernel(), "Native REBLUR diagnostic AOS guide gather");
    stream << index_buffer.upload(indices.data());
    for (uint channel = 0u; channel < inputs.size(); ++channel)
        stream << signal_shader(index_buffer.view(), inputs[channel], signals.view(), count, channel).dispatch(count);
    for (uint plane = 0u; plane < views.gi_guides.size(); ++plane)
        stream << guide_shader(index_buffer.view(), views.gi_guides[plane].view_as<reblur::Guide>(), guides.view(), count, plane).dispatch(count);
    vector<float4> host_signals(size_t(count) * 10u);
    vector<reblur::Guide> host_guides(size_t(count) * 3u);
    stream << signals.download(host_signals.data()) << guides.download(host_guides.data()) << synchronize() << ocarina::commit();
    std::filesystem::create_directories(directory);
    const auto prefix = directory / ("native_reblur_samples_" + stage);
    auto write = [&](const char *extension, const auto &data) {
        std::ofstream file(std::filesystem::path(prefix.string() + extension), std::ios::binary);
        file.write(reinterpret_cast<const char *>(data.data()), data.size() * sizeof(data[0]));
        file.close();
        if (!file) throw std::runtime_error("Native REBLUR sampled diagnostic write failed");
    };
    write(".indices.u32", indices); write(".signals.f32", host_signals); write(".guides.bin", host_guides);
    std::ofstream metadata(std::filesystem::path(prefix.string() + ".json"));
    metadata << "{\n  \"version\":1,\n  \"width\":" << size.x << ",\n  \"height\":" << size.y
        << ",\n  \"count\":" << count << ",\n  \"guide_stride\":80,\n"
        << "  \"layout\":\"channel-major signals / plane-major AOS guides / original linear indices\",\n"
        << "  \"channels\":[\"di_diffuse\",\"di_specular\",\"di_bypass\",\"gi_diffuse_0\",\"gi_specular_0\","
           "\"gi_diffuse_1\",\"gi_specular_1\",\"gi_diffuse_2\",\"gi_specular_2\",\"gi_stable\"]\n}\n";
    metadata.close();
    if (!metadata) throw std::runtime_error("Native REBLUR diagnostic metadata write failed");
}
}// namespace vision::rtxpt_gi

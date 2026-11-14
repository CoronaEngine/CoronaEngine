#pragma once
#include "path_work_diagnostic.h"
#include "rhi/resources/stream.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace vision::path_cost {
// Diagnostic readback only; never submitted in a timing run.
[[nodiscard]] inline ocarina::CommandBatch capture(
    ocarina::BufferView<PathWorkCounters> counters, uint32_t frame,
    ocarina::uint2 size, const char *kind) {
    ocarina::CommandBatch commands;
    if (!enabled()) return commands;
    const char *selected = std::getenv("VISION_PATH_WORK_FRAME");
    const uint32_t selected_frame = selected ? uint32_t(std::strtoul(selected, nullptr, 10)) : 191u;
    if (frame != selected_frame) return commands;
    const std::filesystem::path directory{std::getenv("VISION_PATH_WORK_OUTPUT")};
    const std::string mode{kind};
    auto host = std::make_shared<std::vector<PathWorkCounters>>(counters.size());
    commands << counters.download(host->data()) << ocarina::synchronize();
    commands << [host, directory, frame, size, mode] {
        std::filesystem::create_directories(directory);
        std::ostringstream name;
        name << "path-work-" << mode << "-frame-" << std::setw(4) << std::setfill('0') << frame;
        const auto stem = directory / name.str();
        std::ofstream binary(stem.string() + ".bin", std::ios::binary);
        binary.write(reinterpret_cast<const char *>(host->data()), host->size() * sizeof(PathWorkCounters));
        binary.close();
        if (!binary) throw std::runtime_error("Path work counter readback failed");
        std::ofstream metadata(stem.string() + ".json");
        metadata << "{\n\"schema\":\"path-work-counters-v1\",\n\"mode\":\"" << mode
            << "\",\n\"width\":" << size.x << ",\n\"height\":" << size.y
            << ",\n\"frame\":" << frame << ",\n\"stride\":64,\n\"words\":16,\n"
            << "\"layout\":\"row-major little-endian uint32, four uint4 per pixel\",\n"
            << "\"fields\":[\"closest_requests\",\"shadow_requests\",\"surface_reconstructions\",\"material_builds\","
               "\"scatter_samples\",\"explicit_F_calls\",\"explicit_PDF_calls\",\"NEE_candidates\","
               "\"full_NEE_visibility\",\"accepted_vertices\",\"max_surface_depth\",\"delta_scatters\","
               "\"RR_decisions\",\"RR_scheduled_deaths\",\"rejected_interiors\",\"reserved\"],\n"
            << "\"notes\":\"API-level calls; sample-internal F/PDF and hardware traversal are not counted. Counter runs are not performance evidence.\"\n}\n";
        metadata.close();
        if (!metadata) throw std::runtime_error("Path work metadata write failed");
    };
    return commands;
}
}// namespace vision::path_cost

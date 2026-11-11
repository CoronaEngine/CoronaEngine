#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace vision {
struct DdsBC7Mip {
    uint32_t width{}, height{}, dxgi_format{};
    size_t offset{148}, size{};
};

// DDS_HEADER + DDS_HEADER_DXT10, single 2D BC7 image. Later mip levels,
// when present, are deliberately excluded from the base-level upload.
inline std::optional<DdsBC7Mip> parse_dds_bc7(std::span<const uint8_t> bytes) noexcept {
    if (bytes.size() < 148) return std::nullopt;
    auto u32 = [&](size_t p) {
        return uint32_t(bytes[p]) | uint32_t(bytes[p + 1]) << 8u |
               uint32_t(bytes[p + 2]) << 16u | uint32_t(bytes[p + 3]) << 24u;
    };
    if (u32(0) != 0x20534444u || u32(4) != 124u || u32(76) != 32u ||
        !(u32(80) & 4u) || u32(84) != 0x30315844u ||
        (u32(128) != 98u && u32(128) != 99u) || u32(132) != 3u ||
        (u32(136) & 4u) || u32(140) != 1u || u32(24) > 1u ||
        (u32(112) & (0xfe00u | 0x200000u))) return std::nullopt;
    DdsBC7Mip mip{u32(16), u32(12), u32(128)};
    if (!mip.width || !mip.height) return std::nullopt;
    const uint64_t columns = (uint64_t(mip.width) + 3u) / 4u;
    const uint64_t rows = (uint64_t(mip.height) + 3u) / 4u;
    if (columns > std::numeric_limits<size_t>::max() / 16u / rows) return std::nullopt;
    mip.size = size_t(columns * rows * 16u);
    if (mip.size > bytes.size() - mip.offset) return std::nullopt;
    return mip;
}
}// namespace vision

// Horizon API 兼容层 - 处理 API 变化
#pragma once

#include <corona/systems/optics/hardware.h>

namespace Corona::Systems::HorizonCompat {

// 替代已删除的 has_flag 函数
inline bool has_flag(uint32_t flags, uint32_t flag) {
    return (flags & flag) != 0;
}

// 由于新版 Horizon 移除了 HardwareImage::extent()，
// 我们需要在创建图像时保存 extent 信息。
// 这个结构体用于包装图像及其 extent。
struct ImageWithExtent {
    Corona::Horizon::HardwareImage image;
    Corona::Horizon::ImageExtent extent;

    ImageWithExtent() = default;

    ImageWithExtent(Corona::Horizon::HardwareImage img, Corona::Horizon::ImageExtent ext)
        : image(std::move(img)), extent(ext) {}

    // 从 desc 创建
    ImageWithExtent(const Corona::Horizon::HardwareImageDesc& desc, std::span<const std::byte> data = {})
        : image(desc, data), extent(desc.extent) {}

    explicit operator bool() const { return static_cast<bool>(image); }
};

} // namespace Corona::Systems::HorizonCompat

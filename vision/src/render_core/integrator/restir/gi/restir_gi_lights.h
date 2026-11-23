#pragma once

#include "../common.h"

namespace vision::restir_gi {

// Validates scene lights and retains the native light-table diagnostic buffers.
class NativeLightSampler final : private Toolkit {
    uint light_count_{}, triangle_count_{};
    bool feedback_enabled_{};
    Buffer<uint4> lights_, environment_nodes_;
    Buffer<uint> proxy_counts_, proxy_indices_, control_;
    Buffer<uint2> environment_cube_;
    Buffer<float4> environment_mips_;

public:
    void prepare();
    void update_resolution(uint2 resolution) noexcept;
    void download_validation(const fs::path &directory) const;
};
}// namespace vision::restir_gi

#pragma once

#include "base/mgr/pipeline.h"

namespace vision {

// Supplies the physical primary's material flags when GI runs without DI.
class GIReceiver final : public Toolkit {
private:
    Buffer<uint> primary_flags_;
    Shader<void(uint)> prepare_;

public:
    void compile() noexcept;
    [[nodiscard]] CommandBatch dispatch(uint frame) const noexcept;
    [[nodiscard]] BufferView<uint> primary_flags() const noexcept { return primary_flags_.view(); }
};

}// namespace vision

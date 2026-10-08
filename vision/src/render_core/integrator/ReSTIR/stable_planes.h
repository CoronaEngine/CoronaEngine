#pragma once

#include "common.h"

namespace vision {
// Camera-prefix decomposition. Only the exported dominant endpoint participates
// in reservoirs; all remaining paths retain their original sampling measure.
class StablePlanes final : public ReSTIR {
private:
    uint max_recursion_{};
    Shader<void(Buffer<uint4>)> clear_children_;
    Shader<void(uint, uint)> build_;
    Shader<void(uint)> fill_;

public:
    StablePlanes() = default;
    StablePlanes(IntegratorPtr integrator, uint max_recursion)
        : max_recursion_(max_recursion) { set_integrator(integrator); }
    VS_HOTFIX_MAKE_RESTORE(ReSTIR, max_recursion_, clear_children_, build_, fill_)
    void prepare() noexcept;
    void set_max_recursion(uint value) noexcept { max_recursion_ = value; }
    void compile() noexcept;
    [[nodiscard]] CommandBatch build(uint frame, bool jitter) const noexcept;
    [[nodiscard]] CommandBatch fill(uint frame) const noexcept;
};
}// namespace vision

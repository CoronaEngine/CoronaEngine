#pragma once

#include "common.h"

namespace vision {
// Camera-prefix decomposition. Only the exported dominant endpoint participates
// in reservoirs; all remaining paths retain their original sampling measure.
class StablePlanes final : public ReSTIR {
private:
    uint max_recursion_{};
    Shader<void(Buffer<StablePlaneData>)> clear_children_;
    Shader<void(uint, uint)> build_;
    Shader<void(uint, float, float)> fill_;
    Shader<void(Buffer<StablePlaneData>, Buffer<StablePlaneHistory>, Buffer<uint>)> save_history_;

public:
    StablePlanes() = default;
    StablePlanes(IntegratorPtr integrator, uint max_recursion)
        : max_recursion_(max_recursion) { set_integrator(integrator); }
    VS_HOTFIX_MAKE_RESTORE(ReSTIR, max_recursion_, clear_children_, build_, fill_, save_history_)
    void prepare() noexcept;
    void set_max_recursion(uint value) noexcept { max_recursion_ = value; }
    void compile() noexcept;
    [[nodiscard]] CommandBatch build(uint frame, bool jitter, bool retain_history) const noexcept;
    [[nodiscard]] CommandBatch fill(uint frame, float direct_factor = 1.f, float indirect_factor = 1.f) const noexcept;
};
}// namespace vision

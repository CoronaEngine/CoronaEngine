#pragma once

namespace Corona::Geometry {

struct NativeRenderLoadGuard {
    [[nodiscard]] static constexpr bool is_loading(bool pending_import,
                                                    bool pending_build) noexcept {
        return pending_import || pending_build;
    }

    [[nodiscard]] static constexpr bool effective_ssao(bool loading,
                                                        bool requested,
                                                        bool debug_output) noexcept {
        return !loading && (requested || debug_output);
    }
};

} // namespace Corona::Geometry

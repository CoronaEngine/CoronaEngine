#pragma once

namespace vision {

// Cached secondary radiance is evaluated toward the source receiver. Preserve
// that geometric side, including when both receivers view a two-sided back face.
template<typename T>
[[nodiscard]] inline auto gi_reconnection_supported(const T &target_cosine,
                                                    const T &source_cosine) noexcept {
    return target_cosine * source_cosine > 0.f;
}

}// namespace vision

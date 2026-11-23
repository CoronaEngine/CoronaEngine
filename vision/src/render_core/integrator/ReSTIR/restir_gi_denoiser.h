#pragma once
#include "render_core/denoiser/ReBLUR/filter.h"

// Typed DSL translation of the StablePlanes helper mathematics.
namespace vision::restir_gi {
static_assert(sizeof(reblur::Guide) == 80u);
static_assert(offsetof(reblur::Guide, position) == 0u && offsetof(reblur::Guide, identity) == 16u);
static_assert(offsetof(reblur::Guide, normal_roughness) == 32u && offsetof(reblur::Guide, diffuse_factor) == 48u && offsetof(reblur::Guide, specular_factor) == 64u);

// Host pointers are only dereferenced while building an enabled kernel AST.
// Disabled storage captures no resource and creates no extra GPU expressions.
struct NativeReblurStorage {
    Buffer<float4> *diffuse{}, *specular{}, *bypass{};
    Buffer<float> *motion_y{};
    uint pixels{};
    [[nodiscard]] bool enabled() const noexcept { return diffuse != nullptr; }
    [[nodiscard]] Uint address(const Uint &pixel, const Uint &plane) const noexcept { return plane * pixels + pixel; }
    void write(const Uint &pixel, const Uint &plane, const Float4 &d, const Float4 &s) const noexcept {
        const Uint i = address(pixel, plane); diffuse->write(i, d); specular->write(i, s);
    }
    [[nodiscard]] Float4 read_diffuse(const Uint &pixel, const Uint &plane) const noexcept { return diffuse->read(address(pixel, plane)); }
    [[nodiscard]] Float4 read_specular(const Uint &pixel, const Uint &plane) const noexcept { return specular->read(address(pixel, plane)); }
    void write_motion_y(const Uint &pixel, const Uint &plane, const Float &value) const noexcept { motion_y->write(address(pixel, plane), value); }
};

[[nodiscard]] inline Float native_reblur_hit_t(const Float &previous, const Float &segment,
                                               const Uint &bounces, const Bool &delta_only) noexcept {
    Float result = previous;
    $if(bounces == 1u) { result = segment; }
    $elif(bounces > 1u && bounces <= 3u && delta_only) { result = previous + segment; };
    return result;
}

[[nodiscard]] inline Float native_reblur_combine_hit(const Float3 &previous, const Float &previous_hit,
                                                    const Float3 &incoming, const Float &new_hit) noexcept {
    Float result = previous_hit;
    Float l_new = dot(incoming, make_float3(.2126f, .7152f, .0722f));
    $if(!(l_new < 1e-5f)) {
        Float l_old = dot(previous, make_float3(.2126f, .7152f, .0722f));
        Float w = l_new / (l_old + l_new);
        result = abs(previous_hit) * (1.f - w) + new_hit * w;
    };
    return result;
}
[[nodiscard]] inline Float4 native_reblur_commit(const Float4 &previous, const Float3 &incoming,
                                                const Float &new_hit) noexcept {
    Float4 result = previous;
    Float l_new = dot(incoming, make_float3(.2126f, .7152f, .0722f));
    $if(!(l_new < 1e-5f)) {
        Float h = native_reblur_combine_hit(Float3{previous.xyz()}, Float{previous.w}, incoming, new_hit);
        result = make_float4(previous.xyz() + incoming, h);
    };
    return result;
}
[[nodiscard]] inline Float native_reblur_half_signed(const Float &value) noexcept {
    return f16tof32(f32tof16(clamp(value, -65504.f, 65504.f)));
}
[[nodiscard]] inline Float4 native_reblur_half(const Float4 &value) noexcept {
    return make_float4(native_reblur_half_signed(Float{value.x}), native_reblur_half_signed(Float{value.y}),
        native_reblur_half_signed(Float{value.z}), native_reblur_half_signed(Float{value.w}));
}
[[nodiscard]] inline Uint native_reblur_motion_flags(const Float &depth_motion) noexcept {
    return 3u | (f32tof16(clamp(depth_motion, -65504.f, 65504.f)) << 16u);
}

}// namespace vision::restir_gi

#pragma once
#include "edsl_nrd_params.h"

namespace vision::reblur::edsl {
enum class Format { R8Unorm, RG8Unorm, R16Float, R16Uint, R32Float, R32Uint, RGBA16Float, RGB10A2Unorm };
template<Format F> inline constexpr uint pixel_bytes =
    F == Format::R8Unorm ? 1u :
    (F == Format::RG8Unorm || F == Format::R16Float || F == Format::R16Uint) ? 2u :
    F == Format::RGBA16Float ? 8u : 4u;

// Native image formats use the same GPU format conversion and texture units
// as the HLSL oracle. No software half or UNORM rounding is inserted.
template<typename T, Format F> class Image {
    Ulong texture_, surface_;
    Uint2 size_;
    static constexpr bool integer = F == Format::R16Uint || F == Format::R32Uint;
    [[nodiscard]] static Var<T> narrow(const Float4 &value) {
        if constexpr (std::is_same_v<T, float>) return value.x;
        else if constexpr (std::is_same_v<T, float2>) return value.xy();
        else return value;
    }
public:
    explicit Image(const Var<ImageDesc> &descriptor)
        : texture_(descriptor.texture), surface_(descriptor.surface), size_(descriptor.size) {}
    [[nodiscard]] Uint2 size() const { return size_; }
    // Caller guarantees 0 <= pixel < size(); used only after an explicit clamp.
    [[nodiscard]] Var<T> read_in_bounds(const Int2 &pixel) const {
        if constexpr (integer) return call<uint4>("oc_nrd_tex2d_read_uint", texture_, pixel).x;
        else return narrow(call<float4>("oc_nrd_tex2d_read", texture_, pixel));
    }
    [[nodiscard]] Var<T> read(const Int2 &pixel) const {
        // ReBLUR descriptors have nonzero extents; preserve zero-border loads
        // without issuing an out-of-bounds image access or multiplying NaNs.
        Bool inside = all(pixel >= 0) && all(pixel < make_int2(size_));
        Int2 q = clamp(pixel, make_int2(0), make_int2(size_) - 1);
        Var<T> value = read_in_bounds(q);
        return select(inside, value, Var<T>{T{}});
    }
    [[nodiscard]] Var<T> nearest(const Int2 &pixel) const {
        return read(clamp(pixel, make_int2(0), make_int2(size_) - 1));
    }
    // Caller guarantees 0 <= pixel < size(); other coordinates use checked write.
    void write_in_bounds(const Int2 &pixel, const Var<T> &value) const {
        if constexpr (integer) call("oc_nrd_tex2d_write_uint", surface_, pixel, make_uint4(value, 0u, 0u, 0u));
        else {
            Float4 expanded = make_float4(0.f);
            if constexpr (std::is_same_v<T, float>) expanded.x = value;
            else if constexpr (std::is_same_v<T, float2>) expanded.xy() = value;
            else expanded = value;
            call("oc_nrd_tex2d_write", surface_, pixel, expanded);
        }
    }
    void write(const Int2 &pixel, const Var<T> &value) const {
        $if(all(pixel >= 0) && all(pixel < make_int2(size_))) {
            if constexpr (integer) call("oc_nrd_tex2d_write_uint", surface_, pixel, make_uint4(value, 0u, 0u, 0u));
            else {
                Float4 expanded = make_float4(0.f);
                if constexpr (std::is_same_v<T, float>) expanded.x = value;
                else if constexpr (std::is_same_v<T, float2>) expanded.xy() = value;
                else expanded = value;
                call("oc_nrd_tex2d_write", surface_, pixel, expanded);
            }
        };
    }
    [[nodiscard]] Var<T> sample(const Float2 &uv, bool linear = true) const {
        if constexpr (integer) return nearest(make_int2(floor(uv * make_float2(size_))));
        if constexpr (!integer) {
            return narrow(call<float4>(linear ? "oc_nrd_tex2d_sample" : "oc_nrd_tex2d_sample_point", texture_, uv));
        }
    }
    template<uint Component>
    [[nodiscard]] Float4 gather(const Float2 &uv) const {
        static_assert(Component == 0u || Component == 2u);
        if constexpr (!integer) {
            if constexpr (Component == 0u) return call<float4>("oc_nrd_tex2d_gather_red", texture_, uv);
            else return call<float4>("oc_nrd_tex2d_gather_blue", texture_, uv);
        } else {
            return make_float4(call<uint4>("oc_nrd_tex2d_gather_uint", texture_, uv));
        }
    }
    [[nodiscard]] Float4 gather(const Float2 &uv, uint component = 0u) const {
        if constexpr (!integer) {
            return call<float4>("oc_nrd_tex2d_gather", texture_, uv, component);
        } else {
            return make_float4(call<uint4>("oc_nrd_tex2d_gather_uint", texture_, uv));
        }
    }
};
}

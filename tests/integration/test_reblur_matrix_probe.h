#pragma once

// Diagnostic for the actual SharedConstants upload and Ocarina matrix codegen.
// The CPU reference below reads raw column-major floats independently of the
// Ocarina host matrix operators. The chosen dyadic inputs make every transform
// except the final perspective divide exactly representable in FP32.
#include "render_core/denoiser/ReBLUR/edsl_nrd_common.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace vision::reblur::edsl {
namespace matrix_probe_detail {
using RawMatrix = std::array<float, 16>;
inline float4 multiply(const RawMatrix &matrix, const float4 &vector) {
    float4 result = make_float4(0.f);
    for (uint row = 0u; row != 4u; ++row)
        for (uint column = 0u; column != 4u; ++column)
            result[row] += matrix[column * 4u + row] * vector[column];
    return result;
}
inline float4 rotate(const RawMatrix &matrix, const float3 &vector, bool inverse) {
    float4 result = make_float4(0.f);
    for (uint row = 0u; row != 3u; ++row)
        for (uint column = 0u; column != 3u; ++column)
            result[row] += matrix[inverse ? row * 4u + column : column * 4u + row] * vector[column];
    return result;
}
inline float4 transpose_multiply(const RawMatrix &matrix, const float4 &vector) {
    float4 result = make_float4(0.f);
    for (uint row = 0u; row != 4u; ++row)
        for (uint column = 0u; column != 4u; ++column)
            result[row] += matrix[row * 4u + column] * vector[column];
    return result;
}
}

inline void check_reblur_matrix_contract(Device &device, Stream &stream) {
    using namespace matrix_probe_detail;
    constexpr uint count = 23u;
    const RawMatrix projective{1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f,
                               9.f, 10.f, 11.f, 12.f, 13.f, 14.f, 15.f, 16.f};
    const RawMatrix affine{2.f, -3.f, 5.f, 0.f, 7.f, 11.f, -13.f, 0.f,
                           17.f, -19.f, 23.f, 0.f, 29.f, 31.f, 37.f, 1.f};
    const float3 point = make_float3(.25f, -.5f, 2.f);
    const float3 direction = make_float3(1.5f, -2.f, .25f);
    auto output = device.create_buffer<float4>(count, "REBLUR matrix contract");
    Dispatch parameters{};
    static_assert(sizeof(parameters.constants.gWorldToClip) == sizeof(projective));
    std::memcpy(&parameters.constants.gWorldToClip, projective.data(), sizeof(projective));
    std::memcpy(&parameters.constants.gViewToWorld, affine.data(), sizeof(affine));
    parameters.external.output_diffuse = output.descriptor();
    Kernel probe = [](Var<Dispatch> p) {
        configure_block(1u);
        auto &out = p.external.output_diffuse;
        Float4x4 m = p.constants.gWorldToClip;
        Float4x4 a = p.constants.gViewToWorld;
        Float3 point = make_float3(.25f, -.5f, 2.f);
        Float3 direction = make_float3(1.5f, -2.f, .25f);
        out.write(0u, m[0u]); out.write(1u, m[1u]); out.write(2u, m[2u]); out.write(3u, m[3u]);
        out.write(4u, a[0u]); out.write(5u, a[1u]); out.write(6u, a[2u]); out.write(7u, a[3u]);
        out.write(8u, ProjectiveTransform(m, point));
        out.write(9u, make_float4(AffineTransform(m, point), 1.f));
        out.write(10u, make_float4(RotateVector(m, direction), 0.f));
        out.write(11u, make_float4(RotateVectorInverse(m, direction), 0.f));
        out.write(12u, transpose(m) * make_float4(point, 1.f));
        out.write(13u, make_float4(AffineTransform(a, point), 1.f));
        out.write(14u, make_float4(RotateVector(a, direction), 0.f));
        out.write(15u, make_float4(RotateVectorInverse(a, direction), 0.f));
        Float3x3 truncated = make_float3x3(m);
        out.write(16u, make_float4(truncated[0u], 0.f));
        out.write(17u, make_float4(truncated[1u], 0.f));
        out.write(18u, make_float4(truncated[2u], 0.f));
        out.write(19u, make_float4(GetScreenUv(m, point), 0.f, 0.f));
        // Separate the parameter upload path from the AST matrix constructor.
        Float4x4 literal = make_float4x4(Float4{make_float4(1.f, 2.f, 3.f, 4.f)},
                                        Float4{make_float4(5.f, 6.f, 7.f, 8.f)},
                                        Float4{make_float4(9.f, 10.f, 11.f, 12.f)},
                                        Float4{make_float4(13.f, 14.f, 15.f, 16.f)});
        out.write(20u, ProjectiveTransform(literal, point));
        out.write(21u, make_float4(RotateVector(literal, direction), 0.f));
        out.write(22u, make_float4(RotateVectorInverse(literal, direction), 0.f));
    };
    auto shader = device.compile(probe, "REBLUR-EDSL-matrix-contract");
    std::array<float4, count> actual{}, expected{};
    for (uint column = 0u; column != 4u; ++column) {
        for (uint row = 0u; row != 4u; ++row) {
            expected[column][row] = projective[column * 4u + row];
            expected[4u + column][row] = affine[column * 4u + row];
        }
    }
    expected[8] = multiply(projective, make_float4(point, 1.f));
    expected[9] = expected[8]; expected[9].w = 1.f;
    expected[10] = rotate(projective, direction, false);
    expected[11] = rotate(projective, direction, true);
    expected[12] = transpose_multiply(projective, make_float4(point, 1.f));
    expected[13] = multiply(affine, make_float4(point, 1.f)); expected[13].w = 1.f;
    expected[14] = rotate(affine, direction, false);
    expected[15] = rotate(affine, direction, true);
    for (uint column = 0u; column != 3u; ++column)
        expected[16u + column] = make_float4(projective[column * 4u], projective[column * 4u + 1u], projective[column * 4u + 2u], 0.f);
    expected[19] = make_float4(expected[8].x / expected[8].w * .5f + .5f,
                              expected[8].y / expected[8].w * -.5f + .5f, 0.f, 0.f);
    expected[20] = expected[8]; expected[21] = expected[10]; expected[22] = expected[11];
    constexpr std::array<const char *, count> labels{
        "parameter M column0", "parameter M column1", "parameter M column2", "parameter M column3",
        "parameter affine column0", "parameter affine column1", "parameter affine column2", "parameter affine column3",
        "M * point", "AffineTransform(M,point)", "RotateVector(M,direction)", "RotateVectorInverse(M,direction)",
        "transpose(M) * point", "AffineTransform(affine,point)", "RotateVector(affine,direction)", "RotateVectorInverse(affine,direction)",
        "truncate3x3 column0", "truncate3x3 column1", "truncate3x3 column2", "GetScreenUv(M,point)",
        "literal M * point", "literal RotateVector", "literal RotateVectorInverse"};
    stream << shader(parameters).dispatch(1u) << output.download(actual.data()) << synchronize() << commit();
    bool correct = true;
    std::cout << std::setprecision(9);
    for (uint index = 0u; index != count; ++index) {
        float maximum = 0.f;
        for (uint channel = 0u; channel != 4u; ++channel) {
            const float difference = std::abs(actual[index][channel] - expected[index][channel]);
            correct &= std::isfinite(actual[index][channel]) && difference <= (index == 19u ? 2e-6f : 0.f);
            maximum = std::max(maximum, difference);
        }
        std::cout << "MATRIX " << index << ' ' << labels[index] << " GPU=(";
        for (uint channel = 0u; channel != 4u; ++channel) std::cout << actual[index][channel] << (channel == 3u ? ") CPU=(" : ",");
        for (uint channel = 0u; channel != 4u; ++channel) std::cout << expected[index][channel] << (channel == 3u ? ")" : ",");
        std::cout << " max_abs=" << maximum << '\n';
    }
    if (!correct) throw std::runtime_error("REBLUR SharedConstants/matrix GPU semantics differ from independent CPU column-major reference");
    std::cout << "PASS: REBLUR uploaded/literal matrix columns, projective/affine/rotation/inverse/truncation match CPU column-major math\n";
}
}

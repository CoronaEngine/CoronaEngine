#pragma once
#include "render_core/denoiser/ReBLUR/edsl_nrd_params.h"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace vision::reblur::edsl {
// Exercise the NRD opt-in and its fallback with immutable queued arguments.
// Consecutive UBO dispatches require a new dynamic offset without rebinding the
// pipeline; the BDA interleaves exercise mixed arena alignment and set layouts.
inline void check_reblur_ubo_parameter_contract(Device &device, Stream &stream) {
    static_assert(sizeof(SharedConstants) == 832u);
    constexpr uint count = 12u;
    const std::array<float, 16> matrix{1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f,
                                      9.f, 10.f, 11.f, 12.f, 13.f, 14.f, 15.f, 16.f};
    auto parameters_for = [&](float base, uint slot, float range) {
        SharedConstants p{};
        p.gFrustum = make_float4(base + 2.f, base + 3.f, base + 5.f, base + 7.f);
        p.gFrameIndex = slot;
        p.gDenoisingRange = range;
        std::memcpy(&p.gViewToWorld, matrix.data(), sizeof(matrix));
        return p;
    };
    std::array<SharedConstants, 3> ubo_values{
        parameters_for(0.f, 0u, 23.f), parameters_for(100.f, 2u, 24.f),
        parameters_for(200.f, 4u, 25.f)};
    std::array<SharedConstants, 2> bda_values{
        parameters_for(1000.f, 6u, 33.f), parameters_for(2000.f, 8u, 34.f)};
    SharedConstants mutable_value = parameters_for(0.f, 10u, 23.f);
    auto output = device.create_buffer<float4>(count, "NRD UBO mixed parameter semantics");
    std::array<float4, count> actual;
    actual.fill(make_float4(std::numeric_limits<float>::quiet_NaN()));

    Kernel readonly_nrd = [](Var<SharedConstants> value, BufferVar<float4> out) {
        configure_block(1u);
        const auto &p = value;
        Uint slot = p.gFrameIndex;
        Float4 frustum = p.gFrustum;
        Float4 matrix_and_range = make_float4(p.gViewToWorld[2u].xyz(), p.gDenoisingRange);
        out.write(slot, frustum);
        out.write(slot + 1u, matrix_and_range);
    };
    Kernel readonly_bda = [](Var<SharedConstants> value, BufferVar<float4> out) {
        configure_block(1u);
        const auto &p = value;
        Uint slot = p.gFrameIndex;
        Float4 frustum = p.gFrustum;
        Float4 matrix_and_range = make_float4(p.gViewToWorld[2u].xyz(), p.gDenoisingRange);
        out.write(slot, frustum);
        out.write(slot + 1u, matrix_and_range);
    };
    Kernel mutated_nrd = [](Var<SharedConstants> p, BufferVar<float4> out) {
        configure_block(1u);
        Float before = p.gFrustum.x;
        p.gFrustum.x += 7.f;
        out.write(p.gFrameIndex, make_float4(before, p.gFrustum.x, p.gFrustum.y, p.gDenoisingRange));
        out.write(p.gFrameIndex + 1u, p.gFrustum);
    };
    auto ubo_shader = device.compile(readonly_nrd, "NRD-EDSL-REBLUR-ParameterContract-Readonly");
    auto bda_shader = device.compile(readonly_bda, "parameter-contract-bda-interleave");
    auto mutable_shader = device.compile(mutated_nrd, "NRD-EDSL-REBLUR-ParameterContract-Mutated");
    stream << output.upload(actual.data())
           << ubo_shader(ubo_values[0], output).dispatch(1u)
           << bda_shader(bda_values[0], output).dispatch(1u)
           << ubo_shader(ubo_values[1], output).dispatch(1u)
           << ubo_shader(ubo_values[2], output).dispatch(1u)
           << bda_shader(bda_values[1], output).dispatch(1u)
           << mutable_shader(mutable_value, output).dispatch(1u);
    for (auto &p : ubo_values) p = parameters_for(-1000.f, 0u, -1.f);
    for (auto &p : bda_values) p = parameters_for(-2000.f, 0u, -1.f);
    mutable_value = parameters_for(-3000.f, 0u, -1.f);
    stream << output.download(actual.data()) << synchronize() << commit();
    const std::array<float4, count> expected{
        make_float4(2.f, 3.f, 5.f, 7.f), make_float4(9.f, 10.f, 11.f, 23.f),
        make_float4(102.f, 103.f, 105.f, 107.f), make_float4(9.f, 10.f, 11.f, 24.f),
        make_float4(202.f, 203.f, 205.f, 207.f), make_float4(9.f, 10.f, 11.f, 25.f),
        make_float4(1002.f, 1003.f, 1005.f, 1007.f), make_float4(9.f, 10.f, 11.f, 33.f),
        make_float4(2002.f, 2003.f, 2005.f, 2007.f), make_float4(9.f, 10.f, 11.f, 34.f),
        make_float4(2.f, 9.f, 3.f, 23.f), make_float4(9.f, 3.f, 5.f, 7.f)};
    for (uint row = 0u; row != count; ++row) {
        for (uint component = 0u; component != 4u; ++component) {
            if (!std::isfinite(actual[row][component]) || actual[row][component] != expected[row][component]) {
                std::cerr << "NRD UBO parameter contract row=" << row << " component=" << component
                          << " actual=" << actual[row][component] << " expected=" << expected[row][component] << '\n';
                throw std::runtime_error("NRD UBO/BDA argument snapshots or mutable fallback differ");
            }
        }
    }
    std::cout << "PASS: NRD UBO/BDA parameter contract 3 queued UBO snapshots / 2 BDA interleaves / mutable fallback / 48 exact components\n";
}

// Exercise generic parameter semantics in the already-configured ReBLUR target.
// All expected values are exactly representable; no numerical tolerance is used.
inline void check_reblur_parameter_contract(Device &device, Stream &stream) {
    static_assert(sizeof(SharedConstants) > 96u);
    SharedConstants parameters{};
    parameters.gFrustum = make_float4(2.f, 3.f, 5.f, 7.f);
    parameters.gDenoisingRange = 23.f;
    parameters.gFrameIndex = 17u;
    const std::array<float, 16> matrix{1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f,
                                      9.f, 10.f, 11.f, 12.f, 13.f, 14.f, 15.f, 16.f};
    static_assert(sizeof(parameters.gViewToWorld) == sizeof(matrix));
    std::memcpy(&parameters.gViewToWorld, matrix.data(), sizeof(matrix));
    constexpr uint count = 12u;
    auto output = device.create_buffer<float4>(count, "aggregate parameter semantics");
    std::array<float4, count> actual;
    actual.fill(make_float4(std::numeric_limits<float>::quiet_NaN()));

    Kernel readonly = [](Var<SharedConstants> value, BufferVar<float4> out) {
        const auto &p = value;
        out.write(0u, p.gFrustum);
        out.write(1u, make_float4(p.gViewToWorld[2u].xyz(), p.gDenoisingRange));
        $if(p.gFrameIndex == 17u) { out.write(2u, p.gFrustum * 2.f); };
    };
    Kernel scalar_snapshot = [](Var<SharedConstants> p, BufferVar<float4> out) {
        Float before = p.gFrustum.x;
        p.gFrustum.x += 7.f;
        out.write(3u, make_float4(before, p.gFrustum.x, 1.f, 0.f));
    };
    Kernel record_snapshot = [](Var<SharedConstants> p, BufferVar<float4> out) {
        Var<SharedConstants> snapshot = p;
        p.gFrustum.y += 11.f;
        out.write(4u, snapshot.gFrustum);
        out.write(5u, p.gFrustum);
    };
    Kernel references = [](Var<SharedConstants> p, BufferVar<float4> out) {
        Callable mutate = [](Float &a, Float &b) { a += 3.f; b += a; };
        mutate(p.gFrustum.x, p.gFrustum.x);
        out.write(6u, p.gFrustum);
        mutate(p.gFrustum.y, p.gFrustum.z);
        out.write(7u, p.gFrustum);
    };
    Kernel captures = [](Var<SharedConstants> p, BufferVar<float4> out) {
        Lambda update = [&](Float delta) { p.gFrustum.z += delta; return p.gFrustum.z; };
        Float first = update(5.f);
        Float second = update(7.f);
        out.write(8u, make_float4(first, second, p.gFrustum.z, p.gFrustum.w));
    };
    Kernel queued_snapshot = [](Var<SharedConstants> p, Uint slot, BufferVar<float4> out) {
        out.write(slot, p.gFrustum);
    };
    auto readonly_shader = device.compile(readonly, "parameter-contract-readonly-leaves");
    auto scalar_shader = device.compile(scalar_snapshot, "parameter-contract-scalar-snapshot");
    auto record_shader = device.compile(record_snapshot, "parameter-contract-record-snapshot");
    auto reference_shader = device.compile(references, "parameter-contract-reference-alias");
    auto capture_shader = device.compile(captures, "parameter-contract-lambda-capture");
    auto queued_shader = device.compile(queued_snapshot, "parameter-contract-queued-snapshot");
    stream << output.upload(actual.data())
           << readonly_shader(parameters, output).dispatch(1u)
           << scalar_shader(parameters, output).dispatch(1u)
           << record_shader(parameters, output).dispatch(1u)
           << reference_shader(parameters, output).dispatch(1u)
           << capture_shader(parameters, output).dispatch(1u);
    for (uint i = 0u; i != 3u; ++i) {
        SharedConstants temporary = parameters;
        temporary.gFrustum.x += float(i * 100u);
        stream << queued_shader(temporary, 9u + i, output).dispatch(1u);
        temporary.gFrustum = make_float4(-999.f);
    }
    // The six kernels and all command snapshots were constructed before commit.
    // These host values must not become late GPU parameter sources.
    parameters.gFrustum = make_float4(-1000.f);
    parameters.gFrameIndex = 0u;
    stream << output.download(actual.data()) << synchronize() << commit();
    const std::array<float4, count> expected{
        make_float4(2.f, 3.f, 5.f, 7.f), make_float4(9.f, 10.f, 11.f, 23.f),
        make_float4(4.f, 6.f, 10.f, 14.f), make_float4(2.f, 9.f, 1.f, 0.f),
        make_float4(2.f, 3.f, 5.f, 7.f), make_float4(2.f, 14.f, 5.f, 7.f),
        make_float4(10.f, 3.f, 5.f, 7.f), make_float4(10.f, 6.f, 11.f, 7.f),
        make_float4(10.f, 17.f, 17.f, 7.f), make_float4(2.f, 3.f, 5.f, 7.f),
        make_float4(102.f, 3.f, 5.f, 7.f), make_float4(202.f, 3.f, 5.f, 7.f)};
    for (uint row = 0u; row != count; ++row) {
        for (uint component = 0u; component != 4u; ++component) {
            if (!std::isfinite(actual[row][component]) || actual[row][component] != expected[row][component]) {
                std::cerr << "parameter contract row=" << row << " component=" << component
                          << " actual=" << actual[row][component] << " expected=" << expected[row][component] << '\n';
                throw std::runtime_error("kernel aggregate parameter semantics differ");
            }
        }
    }
    std::cout << "PASS: aggregate parameter contract 6 semantic cases / 48 exact components\n";
    check_reblur_ubo_parameter_contract(device, stream);
}
}

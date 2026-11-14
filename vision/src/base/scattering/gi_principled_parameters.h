#pragma once

#include "base/color/spectrum.h"

namespace vision::gi_principled {
// Lexical shader-compilation bridge, enabled only by ReSTIR GI. Env is shared
// across material/plugin DLLs; no process/thread-local state can leak between
// their separate copies. Ordinary PT/DI never install these keys.
inline constexpr const char *ReadKey = "GI Principled parameter decode";
inline constexpr const char *WriteKey = "GI Principled parameter capture";
inline constexpr const char *SourceKey = "GI Principled parameter source";
inline constexpr const char *ReadyKey = "GI Principled parameter ready";
inline constexpr uint ValidBit = 1u << 10u;
inline constexpr uint BaseNearBit = 1u << 8u;
inline constexpr uint CoatNearBit = 1u << 9u;

[[nodiscard]] inline string key(uint row) { return ocarina::format("GI Principled row {}", row); }
[[nodiscard]] inline Float4 &row(uint index) { return ocarina::Env::instance().get<Float4>(key(index)); }
[[nodiscard]] inline bool reading() { return ocarina::Env::instance().has(ReadKey); }
[[nodiscard]] inline bool writing() { return ocarina::Env::instance().has(WriteKey); }
[[nodiscard]] inline Float value(uint index) { return row(index / 4u)[index % 4u]; }
[[nodiscard]] inline Uint flags() { return ocarina::as<uint>(row(5u).w); }
inline void set_flags(const Uint &value) { row(5u).w = ocarina::as<float>(value); }

class CompilationScope {
public:
    explicit CompilationScope(bool decode) {
        auto &env = ocarina::Env::instance();
        env.clear_global_vars();
        for (uint i = 0u; i < 6u; ++i) {
            Float4 initial = ocarina::make_float4(0.f);
            env.set(key(i), initial);
        }
        Bool enabled = true, ready = false;
        Uint source = InvalidUI32;
        env.set(decode ? ReadKey : WriteKey, enabled);
        env.set(SourceKey, source);
        env.set(ReadyKey, ready);
    }
    ~CompilationScope() { ocarina::Env::instance().clear_global_vars(); }
    CompilationScope(const CompilationScope &) = delete;
    CompilationScope &operator=(const CompilationScope &) = delete;
};

inline void begin_material(const Uint &material_id, uint configuration, bool eligible) {
    if (!writing()) return;
    for (uint i = 0u; i < 6u; ++i) row(i) = ocarina::make_float4(0.f);
    set_flags(Uint{configuration});
    ocarina::Env::instance().get<Uint>(SourceKey) = material_id;
    ocarina::Env::instance().get<Bool>(ReadyKey) = eligible;
}
[[nodiscard]] inline Bool captured(const Uint &material_id) {
    return ocarina::Env::instance().get<Bool>(ReadyKey) &&
           ocarina::Env::instance().get<Uint>(SourceKey) == material_id;
}
template<typename F>
[[nodiscard]] Float scalar(uint index, F &&original) {
    if (reading()) return value(index);
    Float result = original();
    if (writing()) row(index / 4u)[index % 4u] = result;
    return result;
}
template<typename F>
[[nodiscard]] SampledSpectrum color(uint index, F &&original) {
    if (reading()) return SampledSpectrum{row(index / 4u).xyz()};
    SampledSpectrum result = original();
    if (writing() && result.dimension() == 3u) row(index / 4u).xyz() = result.vec3();
    return result;
}
}// namespace vision::gi_principled

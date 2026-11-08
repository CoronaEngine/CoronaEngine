#pragma once

#include <horizon.h>
#include <Compiler/ShaderLanguageConverter.h>
#include <memory>
#include <source_location>
#include <utility>

#include "gpu_performance.h"

namespace Corona::Systems {

// Mechanics executes through Vulkan. Do not compile unused GLSL/HLSL outputs.
// Keep Horizon's EDSL reflection, binding mode and column-major matrices.
template <class F>
Horizon::ComputePipeline<> make_compute_pipeline(
    F&& shader, ktm::uvec3 threads,
    const std::source_location& location = std::source_location::current()) {
    GpuPerformance::Scope timing{"pipeline.compile"};
    EmbeddedShader::CompilerOption options;
    options.compileGLSL = false;
    options.compileHLSL = false;
    options.enableMatrixColumnMajor = true;
    Horizon::ComputePipelineShaders shaders;
    // Preparation workers can exit before their pipelines are consumed or
    // destroyed. Retain the compiler's parent session past its thread-local
    // owner, and release the compiled modules before releasing that session.
    struct CompiledShader {
        Slang::ComPtr<slang::IGlobalSession> session;
        EmbeddedShader::ComputePipelineObject object;
    };
    auto session = EmbeddedShader::ShaderLanguageConverter::getGlobalSession();
    auto compiled = std::make_shared<CompiledShader>(std::move(session),
        EmbeddedShader::ComputePipelineObject::compile(
            std::forward<F>(shader), threads, options, location));
    shaders.object = {compiled, &compiled->object};
    shaders.compute = shaders.object->compute->getShaderCode(
        EmbeddedShader::ShaderLanguage::SpirV, options.enableBindless);
    shaders.auto_bind_entries = shaders.object->autoBindEntries;
    return Horizon::ComputePipeline<>(Horizon::ComputePipelineDesc{threads}, std::move(shaders), location);
}

}  // namespace Corona::Systems

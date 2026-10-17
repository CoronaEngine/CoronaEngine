#pragma once

#include <horizon.h>
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
    shaders.object = std::make_shared<EmbeddedShader::ComputePipelineObject>(
        EmbeddedShader::ComputePipelineObject::compile(
            std::forward<F>(shader), threads, options, location));
    shaders.compute = shaders.object->compute->getShaderCode(
        EmbeddedShader::ShaderLanguage::SpirV, options.enableBindless);
    shaders.auto_bind_entries = shaders.object->autoBindEntries;
    return Horizon::ComputePipeline<>(Horizon::ComputePipelineDesc{threads}, std::move(shaders), location);
}

}  // namespace Corona::Systems

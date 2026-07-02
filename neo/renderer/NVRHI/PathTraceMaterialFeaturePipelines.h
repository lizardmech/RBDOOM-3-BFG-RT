#pragma once

// Generic RT pipeline helpers for material-feature shader passes.
// Render-path facades own when a feature is enabled; this module owns the
// repeated shader-library and ray-tracing pipeline mechanics.

#include "PathTraceMaterialFeaturePasses.h"

#include <nvrhi/nvrhi.h>

bool LoadPathTraceMaterialFeatureShaderLibrary(
    nvrhi::IDevice* device,
    const char* shaderPath,
    const char* label,
    nvrhi::ShaderLibraryHandle& shaderLibrary);
bool CreatePathTraceMaterialFeatureRayTracingPipeline(
    nvrhi::IDevice* device,
    nvrhi::ShaderLibraryHandle shaderLibrary,
    nvrhi::BindingLayoutHandle bindingLayout,
    nvrhi::BindingLayoutHandle textureBindlessLayout,
    const RtPathTraceMaterialFeatureShaderDesc& shaderDesc,
    nvrhi::rt::PipelineHandle& pipeline,
    nvrhi::rt::ShaderTableHandle& shaderTable);

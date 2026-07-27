#pragma once

// Generic RT pipeline helpers for material-feature shader passes.
// Render-path facades own when a feature is enabled; this module owns the
// repeated shader-library and ray-tracing pipeline mechanics.

#include "PathTraceMaterialFeaturePasses.h"
#include "PathTraceMaterialFeatureRuntime.h"

#include <nvrhi/nvrhi.h>

struct RtPathTraceMaterialFeaturePipelineContext
{
    RtPathTraceMaterialFeatureShaderState* shaderState = nullptr;
    nvrhi::IDevice* device = nullptr;
    nvrhi::GraphicsAPI graphicsApi = nvrhi::GraphicsAPI::D3D12;
    bool runtimeInitialized = false;
    nvrhi::BindingLayoutHandle bindingLayout;
    nvrhi::BindingLayoutHandle textureBindlessLayout;
    nvrhi::ShaderLibraryHandle bucketHitShaderLibrary;
};

typedef RtPathTraceMaterialFeaturePipelineContext (*RtPathTraceMaterialFeaturePipelineContextCallback)(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const void* userContext);

bool LoadPathTraceMaterialFeatureShaderLibrary(
    nvrhi::IDevice* device,
    const char* shaderPath,
    const char* label,
    nvrhi::ShaderLibraryHandle& shaderLibrary);
bool CreatePathTraceMaterialFeatureRayTracingPipeline(
    nvrhi::IDevice* device,
    nvrhi::ShaderLibraryHandle shaderLibrary,
    nvrhi::ShaderLibraryHandle bucketHitShaderLibrary,
    nvrhi::BindingLayoutHandle bindingLayout,
    nvrhi::BindingLayoutHandle textureBindlessLayout,
    const RtPathTraceMaterialFeatureShaderDesc& shaderDesc,
    nvrhi::rt::PipelineHandle& pipeline,
    nvrhi::rt::ShaderTableHandle& shaderTable);
bool InitPathTraceMaterialFeaturePipeline(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const RtPathTraceMaterialFeaturePipelineContext& context);
bool EnsurePathTraceMaterialFeatureRuntimePassPipeline(
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const RtPathTraceMaterialFeaturePipelineContext& context);
bool EnsurePathTraceMaterialFeatureRegistrationListPipelines(
    const RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCount,
    RtPathTraceMaterialFeaturePipelineContextCallback contextCallback,
    const void* userContext,
    const char* ownerLabel);

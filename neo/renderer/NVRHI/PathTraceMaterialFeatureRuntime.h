#pragma once

// Runtime pairing for a material feature pass descriptor and its shader state.

#include "PathTraceMaterialFeaturePasses.h"

#include <cstddef>

#include <nvrhi/nvrhi.h>

struct RtPathTraceMaterialFeatureShaderState
{
    nvrhi::ShaderLibraryHandle shaderLibrary;
    nvrhi::rt::PipelineHandle pipeline;
    nvrhi::rt::ShaderTableHandle shaderTable;
};

static constexpr size_t RT_PATH_TRACE_MATERIAL_FEATURE_SHADER_TABLE_COUNT =
    static_cast<size_t>(RtPathTraceMaterialFeatureShaderTable::Count);

struct RtPathTraceMaterialFeatureRuntimePass
{
    RtPathTraceMaterialFeaturePassDesc desc;
    const RtPathTraceMaterialFeatureShaderState* shader = nullptr;
    bool ready = false;
};

struct RtPathTraceMaterialFeaturePipelineRequest
{
    RtPathTraceMaterialFeatureShaderState* shaderState = nullptr;
    RtPathTraceMaterialFeatureShaderDesc shaderDesc;
    nvrhi::BindingLayoutHandle bindingLayout;
    const char* shaderPath = nullptr;
};

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassDesc& desc,
    const RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount);
RtPathTraceMaterialFeatureRuntimePass BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(
    bool cleanRouteRequested,
    int cleanView,
    bool producerRequested,
    bool debugOutputRequested,
    const RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount);
RtPathTraceMaterialFeatureShaderState* PathTraceMaterialFeatureShaderStateForPass(
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount);
const char* PathTraceMaterialFeatureShaderPathForGraphicsApi(
    const RtPathTraceMaterialFeatureShaderDesc& shaderDesc,
    nvrhi::GraphicsAPI graphicsApi);
RtPathTraceMaterialFeaturePipelineRequest BuildPathTraceMaterialFeaturePipelineRequest(
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount,
    nvrhi::BindingLayoutHandle coreSmokeBindingLayout,
    nvrhi::BindingLayoutHandle cleanRtxdiDiBindingLayout,
    nvrhi::GraphicsAPI graphicsApi);
void SetPathTraceMaterialFeatureRuntimeInfo(float runtimeInfo[4], const RtPathTraceMaterialFeaturePassDesc& desc, bool passReady);
void SetPathTraceMaterialFeatureRuntimeInfo(float runtimeInfo[4], const RtPathTraceMaterialFeatureRuntimePass& pass);

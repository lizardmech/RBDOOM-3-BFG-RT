#pragma once

// Runtime pairing for a material feature pass descriptor and its shader state.

#include "PathTraceMaterialFeaturePasses.h"

#include <array>
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

struct RtPathTraceMaterialFeatureShaderTableState
{
    std::array<RtPathTraceMaterialFeatureShaderState, RT_PATH_TRACE_MATERIAL_FEATURE_SHADER_TABLE_COUNT> shaders;
};

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
RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassDesc& desc,
    const RtPathTraceMaterialFeatureShaderTableState& shaderTableState);
RtPathTraceMaterialFeatureShaderState* PathTraceMaterialFeatureShaderStateForPass(
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount);
RtPathTraceMaterialFeatureShaderState* PathTraceMaterialFeatureShaderStateForPass(
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    RtPathTraceMaterialFeatureShaderTableState& shaderTableState);
const char* PathTraceMaterialFeatureShaderPathForGraphicsApi(
    const RtPathTraceMaterialFeatureShaderDesc& shaderDesc,
    nvrhi::GraphicsAPI graphicsApi);
RtPathTraceMaterialFeaturePipelineRequest BuildPathTraceMaterialFeaturePipelineRequest(
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceMaterialFeatureShaderDesc& shaderDesc,
    RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount,
    nvrhi::BindingLayoutHandle bindingLayout,
    nvrhi::GraphicsAPI graphicsApi);
RtPathTraceMaterialFeaturePipelineRequest BuildPathTraceMaterialFeaturePipelineRequest(
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceMaterialFeatureShaderDesc& shaderDesc,
    RtPathTraceMaterialFeatureShaderTableState& shaderTableState,
    nvrhi::BindingLayoutHandle bindingLayout,
    nvrhi::GraphicsAPI graphicsApi);
void SetPathTraceMaterialFeatureRuntimeInfo(float runtimeInfo[4], const RtPathTraceMaterialFeaturePassDesc& desc, bool passReady);
void SetPathTraceMaterialFeatureRuntimeInfo(float runtimeInfo[4], const RtPathTraceMaterialFeatureRuntimePass& pass);

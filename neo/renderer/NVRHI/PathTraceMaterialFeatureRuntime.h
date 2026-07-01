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

struct RtPathTraceMaterialFeatureRuntimeInfo
{
    float writesOutputColor = 0.0f;
    float ready = 0.0f;
    float debugMode = 0.0f;
    float frameIndex = 0.0f;
};

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassDesc& desc,
    const RtPathTraceMaterialFeatureShaderState* shaderState);
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
    const RtPathTraceMaterialFeatureShaderDesc& shaderDesc,
    RtPathTraceMaterialFeatureShaderState* shaderState,
    nvrhi::BindingLayoutHandle bindingLayout,
    nvrhi::GraphicsAPI graphicsApi);
RtPathTraceMaterialFeatureRuntimeInfo BuildPathTraceMaterialFeatureRuntimeInfo(const RtPathTraceMaterialFeaturePassDesc& desc, bool passReady);
RtPathTraceMaterialFeatureRuntimeInfo BuildPathTraceMaterialFeatureRuntimeInfo(const RtPathTraceMaterialFeatureRuntimePass& pass);
void PackPathTraceMaterialFeatureRuntimeInfo(float runtimeInfo[4], const RtPathTraceMaterialFeatureRuntimeInfo& typedInfo);
void SetPathTraceMaterialFeatureRuntimeInfo(float runtimeInfo[4], const RtPathTraceMaterialFeaturePassDesc& desc, bool passReady);
void SetPathTraceMaterialFeatureRuntimeInfo(float runtimeInfo[4], const RtPathTraceMaterialFeatureRuntimePass& pass);

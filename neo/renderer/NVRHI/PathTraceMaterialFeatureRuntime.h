#pragma once

// Runtime pairing for a material feature pass descriptor and its shader state.

#include "PathTraceMaterialFeaturePasses.h"

#include <array>
#include <cstddef>
#include <string>

#include <nvrhi/nvrhi.h>

struct RtPathTraceMaterialFeatureShaderState
{
    nvrhi::ShaderLibraryHandle shaderLibrary;
    nvrhi::rt::PipelineHandle pipeline;
    nvrhi::rt::ShaderTableHandle shaderTable;
};

static constexpr size_t RT_PATH_TRACE_MATERIAL_FEATURE_LEGACY_SHADER_TABLE_COUNT =
    static_cast<size_t>(RtPathTraceMaterialFeatureShaderTable::Count);

struct RtPathTraceMaterialFeatureShaderTableState
{
    std::array<RtPathTraceMaterialFeatureShaderState, RT_PATH_TRACE_MATERIAL_FEATURE_SHADER_STATE_CAPACITY> shaders;
};

struct RtPathTraceMaterialFeatureRuntimePass
{
    RtPathTraceMaterialFeaturePassDesc desc;
    RtPathTraceMaterialFeatureRuntimeInfoCallback runtimeInfoCallback = nullptr;
    const RtPathTraceMaterialFeatureShaderState* shader = nullptr;
    bool pipelineRequested = false;
    bool ready = false;
};

struct RtPathTraceMaterialFeaturePipelineRequest
{
    RtPathTraceMaterialFeatureShaderState* shaderState = nullptr;
    RtPathTraceMaterialFeatureShaderDesc shaderDesc;
    nvrhi::BindingLayoutHandle bindingLayout;
    std::string shaderPath;
};

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassDesc& desc,
    const RtPathTraceMaterialFeatureShaderState* shaderState);
RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const RtPathTraceMaterialFeatureShaderState* shaderState);
RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount);
RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const RtPathTraceMaterialFeatureShaderTableState& shaderTableState);
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
RtPathTraceMaterialFeatureShaderState* PathTraceMaterialFeatureShaderStateForRegistration(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount);
RtPathTraceMaterialFeatureShaderState* PathTraceMaterialFeatureShaderStateForRegistration(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    RtPathTraceMaterialFeatureShaderTableState& shaderTableState);
std::string PathTraceMaterialFeatureShaderPathForGraphicsApi(
    const RtPathTraceMaterialFeatureShaderDesc& shaderDesc,
    nvrhi::GraphicsAPI graphicsApi);
RtPathTraceMaterialFeaturePipelineRequest BuildPathTraceMaterialFeaturePipelineRequest(
    const RtPathTraceMaterialFeatureShaderDesc& shaderDesc,
    RtPathTraceMaterialFeatureShaderState* shaderState,
    nvrhi::BindingLayoutHandle bindingLayout,
    nvrhi::GraphicsAPI graphicsApi);
bool ValidatePathTraceMaterialFeatureRegistration(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const char* ownerLabel);
RtPathTraceMaterialFeatureRuntimeInfo BuildPathTraceMaterialFeatureRuntimeInfo(const RtPathTraceMaterialFeaturePassDesc& desc, bool passReady);
RtPathTraceMaterialFeatureRuntimeInfo BuildPathTraceMaterialFeatureRuntimeInfo(const RtPathTraceMaterialFeatureRuntimePass& pass);
RtPathTraceMaterialFeatureRuntimeConstants BuildPathTraceMaterialFeatureRuntimeConstants(
    const RtPathTraceMaterialFeatureRuntimeInfo& typedInfo);
RtPathTraceMaterialFeatureRuntimeConstants BuildPathTraceMaterialFeatureRuntimeConstants(
    const RtPathTraceMaterialFeaturePassDesc& desc,
    bool passReady);
RtPathTraceMaterialFeatureRuntimeConstants BuildPathTraceMaterialFeatureRuntimeConstants(
    const RtPathTraceMaterialFeatureRuntimePass& pass);

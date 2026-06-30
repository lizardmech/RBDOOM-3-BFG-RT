#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiMaterialFeaturesInternal.h"
#include "PathTraceMaterialFeatureBindings.h"
#include "PathTraceMaterialFeatureOutputs.h"

#include <cstring>

struct RtPathTraceCleanRtxdiDiMaterialFeatureState::Impl
{
    RtPathTraceMaterialFeatureShaderTableState shaderTableState;
};

RtPathTraceCleanRtxdiDiMaterialFeatureState::RtPathTraceCleanRtxdiDiMaterialFeatureState()
    : m_impl(new Impl())
{
}

RtPathTraceCleanRtxdiDiMaterialFeatureState::~RtPathTraceCleanRtxdiDiMaterialFeatureState() = default;

RtPathTraceCleanRtxdiDiMaterialFeatureState::RtPathTraceCleanRtxdiDiMaterialFeatureState(
    RtPathTraceCleanRtxdiDiMaterialFeatureState&&) noexcept = default;

RtPathTraceCleanRtxdiDiMaterialFeatureState& RtPathTraceCleanRtxdiDiMaterialFeatureState::operator=(
    RtPathTraceCleanRtxdiDiMaterialFeatureState&&) noexcept = default;

RtPathTraceCleanRtxdiDiMaterialFeaturePipelineResources BuildPathTraceCleanRtxdiDiMaterialFeaturePipelineResources(
    RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState,
    bool smokeTestInitialized,
    nvrhi::BindingLayoutHandle smokeBindingLayout,
    nvrhi::BindingLayoutHandle cleanRtxdiDiBindingLayout,
    nvrhi::BindingLayoutHandle textureBindlessLayout)
{
    return {
        &featureState,
        smokeTestInitialized,
        smokeBindingLayout,
        cleanRtxdiDiBindingLayout,
        textureBindlessLayout
    };
}

RtPathTraceCleanRtxdiDiTransmissionPass BuildPathTraceCleanRtxdiDiTransmissionPass(
    const RtPathTraceCleanRtxdiDiTransmissionSettings& settings,
    const RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState)
{
    RtPathTraceCleanRtxdiDiTransmissionPass pass;
    pass.settings = settings;
    pass.featureState = &featureState;
    return pass;
}

RtPathTraceMaterialFeatureShaderTableState* RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::ShaderTableState(
    RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState)
{
    return featureState.m_impl
        ? &featureState.m_impl->shaderTableState
        : nullptr;
}

const RtPathTraceMaterialFeatureShaderTableState* RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::ShaderTableState(
    const RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState)
{
    return featureState.m_impl
        ? &featureState.m_impl->shaderTableState
        : nullptr;
}

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass)
{
    const RtPathTraceMaterialFeatureShaderTableState* shaderTableState = pass.featureState
        ? RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::ShaderTableState(*pass.featureState)
        : nullptr;
    if (!shaderTableState)
    {
        return RtPathTraceMaterialFeatureRuntimePass();
    }

    return BuildPathTraceMaterialFeatureRuntimePass(
        BuildPathTraceCleanRtxdiDiTransmissionFeaturePassDesc(
            pass.settings.cleanRouteRequested,
            pass.settings.cleanView,
            pass.settings.producerRequested,
            pass.settings.debugOutputRequested),
        *shaderTableState);
}

void AddPathTraceCleanRtxdiDiTransmissionOutputLayoutBindings(nvrhi::BindingLayoutDesc& desc)
{
    AddPathTraceCleanRtxdiDiMaterialFeatureOutputLayoutBindings(desc);
}

void AddPathTraceCleanRtxdiDiTransmissionOutputBindings(
    nvrhi::BindingSetDesc& desc,
    const RtPathTraceFrameResources& frameResources)
{
    AddPathTraceCleanRtxdiDiMaterialFeatureOutputBindings(desc, frameResources);
}

bool PathTraceCleanRtxdiDiTransmissionOutputAvailable(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources)
{
    const RtPathTraceMaterialFeatureRuntimePass featurePass =
        BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(pass);
    return PathTraceMaterialFeaturePrimaryOutputAvailable(featurePass, frameResources);
}

void SetPathTraceCleanRtxdiDiTransmissionOutputState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources,
    nvrhi::ResourceStates state)
{
    const RtPathTraceMaterialFeatureRuntimePass featurePass =
        BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(pass);
    SetPathTraceMaterialFeaturePrimaryOutputState(commandList, featurePass, frameResources, state);
}

void ClearPathTraceCleanRtxdiDiTransmissionOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources,
    const nvrhi::Color& color)
{
    const RtPathTraceMaterialFeatureRuntimePass featurePass =
        BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(pass);
    ClearPathTraceMaterialFeaturePrimaryOutput(commandList, featurePass, frameResources, color);
}

void DispatchPathTraceCleanRtxdiDiTransmissionFeaturePass(
    nvrhi::ICommandList* commandList,
    const nvrhi::rt::State& baseState,
    const nvrhi::rt::DispatchRaysArguments& args,
    nvrhi::BufferHandle constantsBuffer,
    const void* baseConstants,
    size_t baseConstantsSize,
    size_t runtimeInfoOffset,
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources,
    bool nsightGpuMarkers)
{
    const RtPathTraceMaterialFeatureRuntimePass featurePass =
        BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(pass);
    if (!commandList || !baseConstants || !featurePass.ready || !featurePass.shader || !featurePass.shader->shaderTable)
    {
        return;
    }
    if (baseConstantsSize == 0 || baseConstantsSize > 512 || runtimeInfoOffset + sizeof(float) * 4 > baseConstantsSize)
    {
        return;
    }

    nvrhi::rt::State featureState = baseState;
    featureState.shaderTable = featurePass.shader->shaderTable;
    commandList->setRayTracingState(featureState);

    unsigned char featureConstants[512] = {};
    std::memcpy(featureConstants, baseConstants, baseConstantsSize);
    SetPathTraceCleanRtxdiDiTransmissionRuntimeInfo(
        reinterpret_cast<float*>(featureConstants + runtimeInfoOffset),
        pass);
    commandList->writeBuffer(constantsBuffer, featureConstants, baseConstantsSize);

    const bool markerEnabled = nsightGpuMarkers && featurePass.desc.debugLabel && featurePass.desc.debugLabel[0];
    if (markerEnabled)
    {
        commandList->beginMarker(featurePass.desc.debugLabel);
    }
    commandList->dispatchRays(args);
    if (markerEnabled)
    {
        commandList->endMarker();
    }

    BarrierPathTraceMaterialFeatureOutputs(commandList, featurePass, frameResources);
}

void SetPathTraceCleanRtxdiDiTransmissionRuntimeInfo(
    float runtimeInfo[4],
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass)
{
    const RtPathTraceMaterialFeatureRuntimePass featurePass =
        BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(pass);
    SetPathTraceMaterialFeatureRuntimeInfo(runtimeInfo, featurePass);
}

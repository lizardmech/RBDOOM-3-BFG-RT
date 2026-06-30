#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiMaterialFeatures.h"
#include "PathTraceMaterialFeatureBindings.h"
#include "PathTraceMaterialFeatureOutputs.h"

#include <cstring>

RtPathTraceCleanRtxdiDiTransmissionPass BuildPathTraceCleanRtxdiDiTransmissionPass(
    const RtPathTraceCleanRtxdiDiTransmissionSettings& settings,
    const RtPathTraceMaterialFeatureShaderTableState& shaderTableState)
{
    RtPathTraceCleanRtxdiDiTransmissionPass pass;
    pass.featurePass = BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(
        settings.cleanRouteRequested,
        settings.cleanView,
        settings.producerRequested,
        settings.debugOutputRequested,
        shaderTableState);
    return pass;
}

const RtPathTraceMaterialFeatureRuntimePass& PathTraceCleanRtxdiDiTransmissionMaterialFeaturePass(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass)
{
    return pass.featurePass;
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
    return PathTraceMaterialFeaturePrimaryOutputAvailable(pass.featurePass, frameResources);
}

void SetPathTraceCleanRtxdiDiTransmissionOutputState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources,
    nvrhi::ResourceStates state)
{
    SetPathTraceMaterialFeaturePrimaryOutputState(commandList, pass.featurePass, frameResources, state);
}

void ClearPathTraceCleanRtxdiDiTransmissionOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources,
    const nvrhi::Color& color)
{
    ClearPathTraceMaterialFeaturePrimaryOutput(commandList, pass.featurePass, frameResources, color);
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
    if (!commandList || !baseConstants || !pass.featurePass.ready || !pass.featurePass.shader || !pass.featurePass.shader->shaderTable)
    {
        return;
    }
    if (baseConstantsSize == 0 || baseConstantsSize > 512 || runtimeInfoOffset + sizeof(float) * 4 > baseConstantsSize)
    {
        return;
    }

    nvrhi::rt::State featureState = baseState;
    featureState.shaderTable = pass.featurePass.shader->shaderTable;
    commandList->setRayTracingState(featureState);

    unsigned char featureConstants[512] = {};
    std::memcpy(featureConstants, baseConstants, baseConstantsSize);
    SetPathTraceCleanRtxdiDiTransmissionRuntimeInfo(
        reinterpret_cast<float*>(featureConstants + runtimeInfoOffset),
        pass);
    commandList->writeBuffer(constantsBuffer, featureConstants, baseConstantsSize);

    const bool markerEnabled = nsightGpuMarkers && pass.featurePass.desc.debugLabel && pass.featurePass.desc.debugLabel[0];
    if (markerEnabled)
    {
        commandList->beginMarker(pass.featurePass.desc.debugLabel);
    }
    commandList->dispatchRays(args);
    if (markerEnabled)
    {
        commandList->endMarker();
    }

    BarrierPathTraceMaterialFeatureOutputs(commandList, pass.featurePass, frameResources);
}

void SetPathTraceCleanRtxdiDiTransmissionRuntimeInfo(
    float runtimeInfo[4],
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass)
{
    SetPathTraceMaterialFeatureRuntimeInfo(runtimeInfo, pass.featurePass);
}

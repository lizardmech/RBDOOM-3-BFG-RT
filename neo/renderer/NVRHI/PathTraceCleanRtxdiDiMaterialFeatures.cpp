#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiMaterialFeatures.h"
#include "PathTraceMaterialFeatureBindings.h"

RtPathTraceCleanRtxdiDiTransmissionPass BuildPathTraceCleanRtxdiDiTransmissionPass(
    bool cleanRouteRequested,
    int cleanView,
    bool producerRequested,
    bool debugOutputRequested,
    const RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount)
{
    RtPathTraceCleanRtxdiDiTransmissionPass pass;
    pass.featurePass = BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(
        cleanRouteRequested,
        cleanView,
        producerRequested,
        debugOutputRequested,
        shaderStates,
        shaderStateCount);
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

void SetPathTraceCleanRtxdiDiTransmissionRuntimeInfo(
    float runtimeInfo[4],
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass)
{
    SetPathTraceMaterialFeatureRuntimeInfo(runtimeInfo, pass.featurePass);
}

#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiMaterialFeatures.h"
#include "PathTraceMaterialFeatureBindings.h"

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

void SetPathTraceCleanRtxdiDiTransmissionRuntimeInfo(
    float runtimeInfo[4],
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass)
{
    SetPathTraceMaterialFeatureRuntimeInfo(runtimeInfo, pass.featurePass);
}

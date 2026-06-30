#pragma once

// Generic dispatch adapter for material feature raygen passes.
//
// The caller owns its pass constants type; the adapter only requires a
// toyPathInfo[4] runtime-info lane matching the current clean feature contract.

#include "PathTraceFrameResources.h"
#include "PathTraceMaterialFeatureOutputs.h"
#include "PathTraceMaterialFeatureRuntime.h"

#include <nvrhi/nvrhi.h>

template< typename Constants >
void DispatchPathTraceMaterialFeaturePass(
    nvrhi::ICommandList* commandList,
    const nvrhi::rt::State& baseState,
    const nvrhi::rt::DispatchRaysArguments& args,
    nvrhi::BufferHandle constantsBuffer,
    const Constants& baseConstants,
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceFrameResources& frameResources,
    bool nsightGpuMarkers)
{
    if (!commandList || !pass.ready || !pass.shader || !pass.shader->shaderTable)
    {
        return;
    }

    nvrhi::rt::State featureState = baseState;
    featureState.shaderTable = pass.shader->shaderTable;
    commandList->setRayTracingState(featureState);

    Constants featureConstants = baseConstants;
    SetPathTraceMaterialFeatureRuntimeInfo(featureConstants.toyPathInfo, pass);
    commandList->writeBuffer(constantsBuffer, &featureConstants, sizeof(featureConstants));

    const bool markerEnabled = nsightGpuMarkers && pass.desc.debugLabel && pass.desc.debugLabel[0];
    if (markerEnabled)
    {
        commandList->beginMarker(pass.desc.debugLabel);
    }
    commandList->dispatchRays(args);
    if (markerEnabled)
    {
        commandList->endMarker();
    }

    BarrierPathTraceMaterialFeatureOutputs(commandList, pass, frameResources);
}

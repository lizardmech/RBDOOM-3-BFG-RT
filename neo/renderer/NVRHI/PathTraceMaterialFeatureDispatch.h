#pragma once

// Generic dispatch adapter for material feature raygen passes.
//
// The caller owns its pass constants type; the adapter only requires a
// four-float runtime-info lane matching the current material feature contract.

#include "PathTraceFrameResources.h"
#include "PathTraceMaterialFeatureOutputs.h"
#include "PathTraceMaterialFeatureRuntime.h"

#include <cstddef>
#include <cstring>

#include <nvrhi/nvrhi.h>

inline void DispatchPathTraceMaterialFeaturePassWithRuntimeInfo(
    nvrhi::ICommandList* commandList,
    const nvrhi::rt::State& baseState,
    const nvrhi::rt::DispatchRaysArguments& args,
    nvrhi::BufferHandle constantsBuffer,
    const void* baseConstants,
    size_t baseConstantsSize,
    const float* baseRuntimeInfo,
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceFrameResources& frameResources,
    bool nsightGpuMarkers)
{
    if (!commandList || !baseConstants || !baseRuntimeInfo ||
        !pass.ready || !pass.shader || !pass.shader->shaderTable)
    {
        return;
    }
    if (baseConstantsSize == 0 || baseConstantsSize > 512)
    {
        return;
    }

    const unsigned char* baseConstantsBytes = static_cast<const unsigned char*>(baseConstants);
    const unsigned char* baseConstantsEnd = baseConstantsBytes + baseConstantsSize;
    const unsigned char* runtimeInfoBytes = reinterpret_cast<const unsigned char*>(baseRuntimeInfo);
    if (runtimeInfoBytes < baseConstantsBytes ||
        runtimeInfoBytes + sizeof(float) * 4 > baseConstantsEnd)
    {
        return;
    }
    const size_t runtimeInfoOffset = static_cast<size_t>(runtimeInfoBytes - baseConstantsBytes);

    nvrhi::rt::State featureState = baseState;
    featureState.shaderTable = pass.shader->shaderTable;
    commandList->setRayTracingState(featureState);

    unsigned char featureConstants[512] = {};
    std::memcpy(featureConstants, baseConstants, baseConstantsSize);
    SetPathTraceMaterialFeatureRuntimeInfo(
        reinterpret_cast<float*>(featureConstants + runtimeInfoOffset),
        pass);
    commandList->writeBuffer(constantsBuffer, featureConstants, baseConstantsSize);

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

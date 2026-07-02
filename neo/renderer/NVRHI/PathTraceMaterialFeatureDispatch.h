#pragma once

// Generic dispatch adapter for material feature raygen passes.
//
// The caller owns its pass constants type; the adapter only requires a
// 48-byte runtime constants buffer matching the current material feature contract.

#include "PathTraceFrameResources.h"
#include "PathTraceMaterialFeatureOutputs.h"
#include "PathTraceMaterialFeatureRuntime.h"

#include <cstddef>

#include <nvrhi/nvrhi.h>

inline void DispatchPathTraceMaterialFeaturePassWithRuntimeInfo(
    nvrhi::ICommandList* commandList,
    const nvrhi::rt::State& baseState,
    const nvrhi::rt::DispatchRaysArguments& args,
    nvrhi::BufferHandle constantsBuffer,
    const void* baseConstants,
    size_t baseConstantsSize,
    nvrhi::BufferHandle runtimeConstantsBuffer,
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceFrameResources& frameResources,
    bool nsightGpuMarkers)
{
    if (!commandList || !constantsBuffer || !runtimeConstantsBuffer || !baseConstants ||
        !pass.ready || !pass.shader || !pass.shader->shaderTable)
    {
        return;
    }
    if (baseConstantsSize == 0)
    {
        return;
    }

    nvrhi::rt::State featureState = baseState;
    featureState.shaderTable = pass.shader->shaderTable;
    commandList->setRayTracingState(featureState);

    const RtPathTraceMaterialFeatureRuntimeConstants runtimeConstants =
        BuildPathTraceMaterialFeatureRuntimeConstants(pass);
    commandList->writeBuffer(constantsBuffer, baseConstants, baseConstantsSize);
    commandList->writeBuffer(runtimeConstantsBuffer, &runtimeConstants, sizeof(runtimeConstants));

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

inline void DispatchPathTraceMaterialFeaturePassesWithRuntimeInfo(
    nvrhi::ICommandList* commandList,
    const nvrhi::rt::State& baseState,
    const nvrhi::rt::DispatchRaysArguments& args,
    nvrhi::BufferHandle constantsBuffer,
    const void* baseConstants,
    size_t baseConstantsSize,
    nvrhi::BufferHandle runtimeConstantsBuffer,
    const RtPathTraceMaterialFeatureRuntimePass* passes,
    size_t passCount,
    const RtPathTraceFrameResources& frameResources,
    bool nsightGpuMarkers)
{
    for (size_t i = 0; passes && i < passCount; ++i)
    {
        DispatchPathTraceMaterialFeaturePassWithRuntimeInfo(
            commandList,
            baseState,
            args,
            constantsBuffer,
            baseConstants,
            baseConstantsSize,
            runtimeConstantsBuffer,
            passes[i],
            frameResources,
            nsightGpuMarkers);
    }
}

template< typename Constants >
void DispatchPathTraceMaterialFeaturePass(
    nvrhi::ICommandList* commandList,
    const nvrhi::rt::State& baseState,
    const nvrhi::rt::DispatchRaysArguments& args,
    nvrhi::BufferHandle constantsBuffer,
    nvrhi::BufferHandle runtimeConstantsBuffer,
    const Constants& baseConstants,
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceFrameResources& frameResources,
    bool nsightGpuMarkers)
{
    if (!commandList || !constantsBuffer || !runtimeConstantsBuffer ||
        !pass.ready || !pass.shader || !pass.shader->shaderTable)
    {
        return;
    }

    nvrhi::rt::State featureState = baseState;
    featureState.shaderTable = pass.shader->shaderTable;
    commandList->setRayTracingState(featureState);

    Constants featureConstants = baseConstants;
    const RtPathTraceMaterialFeatureRuntimeConstants runtimeConstants =
        BuildPathTraceMaterialFeatureRuntimeConstants(pass);
    commandList->writeBuffer(constantsBuffer, &featureConstants, sizeof(featureConstants));
    commandList->writeBuffer(runtimeConstantsBuffer, &runtimeConstants, sizeof(runtimeConstants));

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

#pragma once

// Clean RTXDI DI material-feature adapters.
//
// The clean render path asks for feature-specific operations here; this facade
// keeps generic material-feature mechanics out of the core dispatch body.

#include "PathTraceMaterialFeatureRuntime.h"

struct RtPathTraceFrameResources;

struct RtPathTraceCleanRtxdiDiMaterialFeatureState
{
    RtPathTraceMaterialFeatureShaderTableState shaderTableState;
};

struct RtPathTraceCleanRtxdiDiTransmissionPass
{
    RtPathTraceMaterialFeatureRuntimePass featurePass;
};

struct RtPathTraceCleanRtxdiDiTransmissionSettings
{
    bool cleanRouteRequested = false;
    int cleanView = 0;
    bool producerRequested = false;
    bool debugOutputRequested = false;
};

RtPathTraceCleanRtxdiDiTransmissionPass BuildPathTraceCleanRtxdiDiTransmissionPass(
    const RtPathTraceCleanRtxdiDiTransmissionSettings& settings,
    const RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState);
RtPathTraceMaterialFeatureShaderTableState& PathTraceCleanRtxdiDiMaterialFeatureShaderTableState(
    RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState);
const RtPathTraceMaterialFeatureRuntimePass& PathTraceCleanRtxdiDiTransmissionMaterialFeaturePass(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass);
void AddPathTraceCleanRtxdiDiTransmissionOutputLayoutBindings(nvrhi::BindingLayoutDesc& desc);
void AddPathTraceCleanRtxdiDiTransmissionOutputBindings(
    nvrhi::BindingSetDesc& desc,
    const RtPathTraceFrameResources& frameResources);
bool PathTraceCleanRtxdiDiTransmissionOutputAvailable(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources);
void SetPathTraceCleanRtxdiDiTransmissionOutputState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources,
    nvrhi::ResourceStates state);
void ClearPathTraceCleanRtxdiDiTransmissionOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources,
    const nvrhi::Color& color);
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
    bool nsightGpuMarkers);
void SetPathTraceCleanRtxdiDiTransmissionRuntimeInfo(
    float runtimeInfo[4],
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass);

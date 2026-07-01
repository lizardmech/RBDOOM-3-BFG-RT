#pragma once

// Clean RTXDI DI material-feature adapters.
//
// The clean render path asks for feature-specific operations here; this facade
// keeps generic material-feature mechanics out of the core dispatch body.

#include <cstddef>
#include <memory>

#include <nvrhi/nvrhi.h>

#include "PathTraceMaterialFeaturePasses.h"

struct RtPathTraceFrameResources;

struct RtPathTraceCleanRtxdiDiMaterialFeatureState
{
    RtPathTraceCleanRtxdiDiMaterialFeatureState();
    ~RtPathTraceCleanRtxdiDiMaterialFeatureState();

    RtPathTraceCleanRtxdiDiMaterialFeatureState(const RtPathTraceCleanRtxdiDiMaterialFeatureState&) = delete;
    RtPathTraceCleanRtxdiDiMaterialFeatureState& operator=(const RtPathTraceCleanRtxdiDiMaterialFeatureState&) = delete;
    RtPathTraceCleanRtxdiDiMaterialFeatureState(RtPathTraceCleanRtxdiDiMaterialFeatureState&&) noexcept;
    RtPathTraceCleanRtxdiDiMaterialFeatureState& operator=(RtPathTraceCleanRtxdiDiMaterialFeatureState&&) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    friend struct RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess;
};

struct RtPathTraceCleanRtxdiDiPipelineContext
{
    bool smokeTestInitialized = false;
    nvrhi::BindingLayoutHandle cleanRtxdiDiBindingLayout;
    nvrhi::BindingLayoutHandle textureBindlessLayout;
};

RtPathTraceCleanRtxdiDiPipelineContext BuildPathTraceCleanRtxdiDiPipelineContext(
    bool smokeTestInitialized,
    nvrhi::BindingLayoutHandle cleanRtxdiDiBindingLayout,
    nvrhi::BindingLayoutHandle textureBindlessLayout);

struct RtPathTraceCleanRtxdiDiTransmissionPass
{
    RtPathTraceCleanRtxdiDiTransmissionPass();
    ~RtPathTraceCleanRtxdiDiTransmissionPass();

    RtPathTraceCleanRtxdiDiTransmissionPass(const RtPathTraceCleanRtxdiDiTransmissionPass&) = delete;
    RtPathTraceCleanRtxdiDiTransmissionPass& operator=(const RtPathTraceCleanRtxdiDiTransmissionPass&) = delete;
    RtPathTraceCleanRtxdiDiTransmissionPass(RtPathTraceCleanRtxdiDiTransmissionPass&&) noexcept;
    RtPathTraceCleanRtxdiDiTransmissionPass& operator=(RtPathTraceCleanRtxdiDiTransmissionPass&&) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    friend struct RtPathTraceCleanRtxdiDiTransmissionPassAccess;
};

RtPathTraceCleanRtxdiDiTransmissionPass BuildPathTraceCleanRtxdiDiTransmissionPass(
    bool cleanRouteRequested,
    int cleanView,
    RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState);
size_t BuildPathTraceCleanRtxdiDiMaterialFeatureRegistrations(
    const RtPathTraceCleanRtxdiDiTransmissionPass& transmissionPass,
    RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCapacity);
bool EnsurePathTraceCleanRtxdiDiTransmissionPassPipeline(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceCleanRtxdiDiPipelineContext& context);
void AddPathTraceCleanRtxdiDiTransmissionOutputLayoutBindings(nvrhi::BindingLayoutDesc& desc);
void AddPathTraceCleanRtxdiDiTransmissionOutputBindings(
    nvrhi::BindingSetDesc& desc,
    const RtPathTraceFrameResources& frameResources);
bool PathTraceCleanRtxdiDiTransmissionOutputAvailable(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources);
void SetPathTraceCleanRtxdiDiTransmissionOutputUnorderedAccess(
    nvrhi::ICommandList* commandList,
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources);
void ClearPathTraceCleanRtxdiDiTransmissionOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources);
void DispatchPathTraceCleanRtxdiDiTransmissionFeaturePass(
    nvrhi::ICommandList* commandList,
    const nvrhi::rt::State& baseState,
    const nvrhi::rt::DispatchRaysArguments& args,
    nvrhi::BufferHandle constantsBuffer,
    const void* baseConstants,
    size_t baseConstantsSize,
    const float* baseRuntimeInfo,
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources,
    bool nsightGpuMarkers);
void SetPathTraceCleanRtxdiDiTransmissionRuntimeInfo(
    float runtimeInfo[4],
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass);

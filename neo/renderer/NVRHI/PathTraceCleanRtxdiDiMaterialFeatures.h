#pragma once

// Clean RTXDI DI material-feature adapters.
//
// The clean render path asks for feature-specific operations here; this facade
// keeps generic material-feature mechanics out of the core dispatch body.

#include <cstddef>
#include <memory>

#include <nvrhi/nvrhi.h>

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

struct RtPathTraceCleanRtxdiDiMaterialFeaturePipelineResources
{
    RtPathTraceCleanRtxdiDiMaterialFeatureState* featureState = nullptr;
    bool smokeTestInitialized = false;
    nvrhi::BindingLayoutHandle smokeBindingLayout;
    nvrhi::BindingLayoutHandle cleanRtxdiDiBindingLayout;
    nvrhi::BindingLayoutHandle textureBindlessLayout;
};

RtPathTraceCleanRtxdiDiMaterialFeaturePipelineResources BuildPathTraceCleanRtxdiDiMaterialFeaturePipelineResources(
    RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState,
    bool smokeTestInitialized,
    nvrhi::BindingLayoutHandle smokeBindingLayout,
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
    const RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState);
bool EnsurePathTraceCleanRtxdiDiTransmissionPassPipeline(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceCleanRtxdiDiMaterialFeaturePipelineResources& resources);
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

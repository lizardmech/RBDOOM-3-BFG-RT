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

struct RtPathTraceCleanRtxdiDiMaterialFeaturePasses
{
    RtPathTraceCleanRtxdiDiMaterialFeaturePasses();
    ~RtPathTraceCleanRtxdiDiMaterialFeaturePasses();

    RtPathTraceCleanRtxdiDiMaterialFeaturePasses(const RtPathTraceCleanRtxdiDiMaterialFeaturePasses&) = delete;
    RtPathTraceCleanRtxdiDiMaterialFeaturePasses& operator=(const RtPathTraceCleanRtxdiDiMaterialFeaturePasses&) = delete;
    RtPathTraceCleanRtxdiDiMaterialFeaturePasses(RtPathTraceCleanRtxdiDiMaterialFeaturePasses&&) noexcept;
    RtPathTraceCleanRtxdiDiMaterialFeaturePasses& operator=(RtPathTraceCleanRtxdiDiMaterialFeaturePasses&&) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    friend struct RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess;
};

RtPathTraceCleanRtxdiDiMaterialFeaturePasses BuildPathTraceCleanRtxdiDiMaterialFeaturePasses(
    bool cleanRouteRequested,
    int cleanView,
    RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState);
size_t BuildPathTraceCleanRtxdiDiMaterialFeatureRegistrations(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& featurePasses,
    RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCapacity);
bool EnsurePathTraceCleanRtxdiDiMaterialFeaturePassPipelines(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    const RtPathTraceCleanRtxdiDiPipelineContext& context);
void AddPathTraceCleanRtxdiDiMaterialFeatureLayoutBindings(nvrhi::BindingLayoutDesc& desc);
void AddPathTraceCleanRtxdiDiMaterialFeatureBindings(
    nvrhi::BindingSetDesc& desc,
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    nvrhi::BufferHandle materialTableBuffer,
    nvrhi::BufferHandle materialFeatureBuffer,
    nvrhi::BufferHandle materialFeatureParameterBuffer,
    nvrhi::BufferHandle runtimeConstantsBuffer,
    const RtPathTraceFrameResources& frameResources);
bool PathTraceCleanRtxdiDiMaterialFeatureOutputsAvailable(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    const RtPathTraceFrameResources& frameResources);
void SetPathTraceCleanRtxdiDiMaterialFeatureOutputsUnorderedAccess(
    nvrhi::ICommandList* commandList,
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    const RtPathTraceFrameResources& frameResources);
void ClearPathTraceCleanRtxdiDiMaterialFeatureOutputs(
    nvrhi::ICommandList* commandList,
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    const RtPathTraceFrameResources& frameResources);
void DispatchPathTraceCleanRtxdiDiMaterialFeaturePasses(
    nvrhi::ICommandList* commandList,
    const nvrhi::rt::State& baseState,
    const nvrhi::rt::DispatchRaysArguments& args,
    nvrhi::BufferHandle constantsBuffer,
    const void* baseConstants,
    size_t baseConstantsSize,
    nvrhi::BufferHandle runtimeConstantsBuffer,
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    const RtPathTraceFrameResources& frameResources,
    bool nsightGpuMarkers);

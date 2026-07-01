#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiMaterialFeaturesInternal.h"
#include "PathTraceCVars.h"
#include "PathTraceMaterialFeatureBindings.h"
#include "PathTraceMaterialFeatureOutputs.h"

#include <cstring>

struct RtPathTraceCleanRtxdiDiMaterialFeatureState::Impl
{
    RtPathTraceMaterialFeatureShaderTableState shaderTableState;
};

struct RtPathTraceCleanRtxdiDiTransmissionPass::Impl
{
    bool cleanRouteRequested = false;
    int cleanView = 0;
    bool producerRequested = false;
    bool debugOutputRequested = false;
    const RtPathTraceCleanRtxdiDiMaterialFeatureState* featureState = nullptr;
};

RtPathTraceCleanRtxdiDiMaterialFeatureState::RtPathTraceCleanRtxdiDiMaterialFeatureState()
    : m_impl(new Impl())
{
}

RtPathTraceCleanRtxdiDiMaterialFeatureState::~RtPathTraceCleanRtxdiDiMaterialFeatureState() = default;

RtPathTraceCleanRtxdiDiMaterialFeatureState::RtPathTraceCleanRtxdiDiMaterialFeatureState(
    RtPathTraceCleanRtxdiDiMaterialFeatureState&&) noexcept = default;

RtPathTraceCleanRtxdiDiMaterialFeatureState& RtPathTraceCleanRtxdiDiMaterialFeatureState::operator=(
    RtPathTraceCleanRtxdiDiMaterialFeatureState&&) noexcept = default;

RtPathTraceCleanRtxdiDiTransmissionPass::RtPathTraceCleanRtxdiDiTransmissionPass()
    : m_impl(new Impl())
{
}

RtPathTraceCleanRtxdiDiTransmissionPass::~RtPathTraceCleanRtxdiDiTransmissionPass() = default;

RtPathTraceCleanRtxdiDiTransmissionPass::RtPathTraceCleanRtxdiDiTransmissionPass(
    RtPathTraceCleanRtxdiDiTransmissionPass&&) noexcept = default;

RtPathTraceCleanRtxdiDiTransmissionPass& RtPathTraceCleanRtxdiDiTransmissionPass::operator=(
    RtPathTraceCleanRtxdiDiTransmissionPass&&) noexcept = default;

RtPathTraceCleanRtxdiDiMaterialFeaturePipelineResources BuildPathTraceCleanRtxdiDiMaterialFeaturePipelineResources(
    RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState,
    bool smokeTestInitialized,
    nvrhi::BindingLayoutHandle smokeBindingLayout,
    nvrhi::BindingLayoutHandle cleanRtxdiDiBindingLayout,
    nvrhi::BindingLayoutHandle textureBindlessLayout)
{
    return {
        &featureState,
        smokeTestInitialized,
        smokeBindingLayout,
        cleanRtxdiDiBindingLayout,
        textureBindlessLayout
    };
}

RtPathTraceCleanRtxdiDiTransmissionPass BuildPathTraceCleanRtxdiDiTransmissionPass(
    bool cleanRouteRequested,
    int cleanView,
    const RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState)
{
    RtPathTraceCleanRtxdiDiTransmissionPass pass;
    RtPathTraceCleanRtxdiDiTransmissionPassAccess::Init(
        pass,
        cleanRouteRequested,
        cleanView,
        r_pathTracingCleanRtxdiDiTransmissionProducer.GetInteger() != 0,
        r_pathTracingCleanRtxdiDiTransmissionDebugView.GetInteger() != 0,
        featureState);
    return pass;
}

RtPathTraceMaterialFeatureShaderTableState* RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::ShaderTableState(
    RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState)
{
    return featureState.m_impl
        ? &featureState.m_impl->shaderTableState
        : nullptr;
}

const RtPathTraceMaterialFeatureShaderTableState* RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::ShaderTableState(
    const RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState)
{
    return featureState.m_impl
        ? &featureState.m_impl->shaderTableState
        : nullptr;
}

void RtPathTraceCleanRtxdiDiTransmissionPassAccess::Init(
    RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    bool cleanRouteRequested,
    int cleanView,
    bool producerRequested,
    bool debugOutputRequested,
    const RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState)
{
    if (!pass.m_impl)
    {
        pass.m_impl.reset(new RtPathTraceCleanRtxdiDiTransmissionPass::Impl());
    }
    pass.m_impl->cleanRouteRequested = cleanRouteRequested;
    pass.m_impl->cleanView = cleanView;
    pass.m_impl->producerRequested = producerRequested;
    pass.m_impl->debugOutputRequested = debugOutputRequested;
    pass.m_impl->featureState = &featureState;
}

bool RtPathTraceCleanRtxdiDiTransmissionPassAccess::CleanRouteRequested(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass)
{
    return pass.m_impl
        ? pass.m_impl->cleanRouteRequested
        : false;
}

int RtPathTraceCleanRtxdiDiTransmissionPassAccess::CleanView(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass)
{
    return pass.m_impl
        ? pass.m_impl->cleanView
        : 0;
}

bool RtPathTraceCleanRtxdiDiTransmissionPassAccess::ProducerRequested(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass)
{
    return pass.m_impl
        ? pass.m_impl->producerRequested
        : false;
}

bool RtPathTraceCleanRtxdiDiTransmissionPassAccess::DebugOutputRequested(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass)
{
    return pass.m_impl
        ? pass.m_impl->debugOutputRequested
        : false;
}

const RtPathTraceCleanRtxdiDiMaterialFeatureState* RtPathTraceCleanRtxdiDiTransmissionPassAccess::FeatureState(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass)
{
    return pass.m_impl
        ? pass.m_impl->featureState
        : nullptr;
}

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass)
{
    const RtPathTraceCleanRtxdiDiMaterialFeatureState* featureState =
        RtPathTraceCleanRtxdiDiTransmissionPassAccess::FeatureState(pass);
    const RtPathTraceMaterialFeatureShaderTableState* shaderTableState = featureState
        ? RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::ShaderTableState(*featureState)
        : nullptr;
    if (!shaderTableState)
    {
        return RtPathTraceMaterialFeatureRuntimePass();
    }

    return BuildPathTraceMaterialFeatureRuntimePass(
        BuildPathTraceCleanRtxdiDiTransmissionFeaturePassDesc(
            RtPathTraceCleanRtxdiDiTransmissionPassAccess::CleanRouteRequested(pass),
            RtPathTraceCleanRtxdiDiTransmissionPassAccess::CleanView(pass),
            RtPathTraceCleanRtxdiDiTransmissionPassAccess::ProducerRequested(pass),
            RtPathTraceCleanRtxdiDiTransmissionPassAccess::DebugOutputRequested(pass)),
        *shaderTableState);
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
    const RtPathTraceMaterialFeatureRuntimePass featurePass =
        BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(pass);
    return PathTraceMaterialFeaturePrimaryOutputAvailable(featurePass, frameResources);
}

void SetPathTraceCleanRtxdiDiTransmissionOutputState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources,
    nvrhi::ResourceStates state)
{
    const RtPathTraceMaterialFeatureRuntimePass featurePass =
        BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(pass);
    SetPathTraceMaterialFeaturePrimaryOutputState(commandList, featurePass, frameResources, state);
}

void ClearPathTraceCleanRtxdiDiTransmissionOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources,
    const nvrhi::Color& color)
{
    const RtPathTraceMaterialFeatureRuntimePass featurePass =
        BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(pass);
    ClearPathTraceMaterialFeaturePrimaryOutput(commandList, featurePass, frameResources, color);
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
    const RtPathTraceMaterialFeatureRuntimePass featurePass =
        BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(pass);
    if (!commandList || !baseConstants || !featurePass.ready || !featurePass.shader || !featurePass.shader->shaderTable)
    {
        return;
    }
    if (baseConstantsSize == 0 || baseConstantsSize > 512 || runtimeInfoOffset + sizeof(float) * 4 > baseConstantsSize)
    {
        return;
    }

    nvrhi::rt::State featureState = baseState;
    featureState.shaderTable = featurePass.shader->shaderTable;
    commandList->setRayTracingState(featureState);

    unsigned char featureConstants[512] = {};
    std::memcpy(featureConstants, baseConstants, baseConstantsSize);
    SetPathTraceCleanRtxdiDiTransmissionRuntimeInfo(
        reinterpret_cast<float*>(featureConstants + runtimeInfoOffset),
        pass);
    commandList->writeBuffer(constantsBuffer, featureConstants, baseConstantsSize);

    const bool markerEnabled = nsightGpuMarkers && featurePass.desc.debugLabel && featurePass.desc.debugLabel[0];
    if (markerEnabled)
    {
        commandList->beginMarker(featurePass.desc.debugLabel);
    }
    commandList->dispatchRays(args);
    if (markerEnabled)
    {
        commandList->endMarker();
    }

    BarrierPathTraceMaterialFeatureOutputs(commandList, featurePass, frameResources);
}

void SetPathTraceCleanRtxdiDiTransmissionRuntimeInfo(
    float runtimeInfo[4],
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass)
{
    const RtPathTraceMaterialFeatureRuntimePass featurePass =
        BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(pass);
    SetPathTraceMaterialFeatureRuntimeInfo(runtimeInfo, featurePass);
}

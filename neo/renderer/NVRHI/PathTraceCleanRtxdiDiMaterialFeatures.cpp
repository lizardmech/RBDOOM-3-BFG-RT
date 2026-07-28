#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiMaterialFeatures.h"
#include "PathTraceCleanRtxdiDiMaterialFeatureRegistry.h"
#include "PathTraceMaterialFeatureBindings.h"
#include "PathTraceMaterialFeatureDispatch.h"
#include "PathTraceMaterialFeatureOutputs.h"
#include "PathTraceMaterialFeaturePipelines.h"
#include "PathTraceMaterialFeatureRuntime.h"

#include "../../sys/DeviceManager.h"

extern DeviceManager* deviceManager;

struct RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess
{
    static RtPathTraceMaterialFeatureShaderState* ShaderStateForRegistration(
        RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState,
        const RtPathTraceMaterialFeaturePassRegistration& registration);
    static const RtPathTraceMaterialFeatureShaderTableState* ShaderTableState(
        const RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState);
};

struct RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess
{
    static void Init(
        RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
        bool cleanRouteRequested,
        int cleanView,
        RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState);
    static bool CleanRouteRequested(const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes);
    static int CleanView(const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes);
    static RtPathTraceCleanRtxdiDiMaterialFeatureState* FeatureState(
        const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes);
};

struct RtPathTraceCleanRtxdiDiMaterialFeatureState::Impl
{
    RtPathTraceMaterialFeatureShaderTableState shaderTableState;
};

struct RtPathTraceCleanRtxdiDiMaterialFeaturePasses::Impl
{
    bool cleanRouteRequested = false;
    int cleanView = 0;
    RtPathTraceCleanRtxdiDiMaterialFeatureState* featureState = nullptr;
};

struct RtPathTraceCleanRtxdiDiMaterialFeaturePipelineEnsureContext
{
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses* passes = nullptr;
    const RtPathTraceCleanRtxdiDiPipelineContext* pipelineContext = nullptr;
};

static RtPathTraceCleanRtxdiDiMaterialFeatureRegistryContext BuildPathTraceCleanRtxdiDiMaterialFeatureRegistryContext(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes)
{
    return {
        RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::CleanRouteRequested(passes),
        RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::CleanView(passes)
    };
}

static RtPathTraceMaterialFeaturePipelineContext BuildPathTraceCleanRtxdiDiMaterialFeaturePipelineContext(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const RtPathTraceCleanRtxdiDiPipelineContext& context)
{
    RtPathTraceCleanRtxdiDiMaterialFeatureState* featureState =
        RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::FeatureState(passes);
    nvrhi::IDevice* device =
        deviceManager ? deviceManager->GetDevice() : nullptr;
    const nvrhi::GraphicsAPI graphicsApi =
        deviceManager
            ? deviceManager->GetGraphicsAPI()
            : nvrhi::GraphicsAPI::D3D12;
    return {
        featureState
            ? RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::ShaderStateForRegistration(*featureState, registration)
            : nullptr,
        device,
        graphicsApi,
        context.smokeTestInitialized,
        context.cleanRtxdiDiBindingLayout,
        context.textureBindlessLayout
    };
}

static RtPathTraceMaterialFeaturePipelineContext BuildPathTraceCleanRtxdiDiMaterialFeaturePipelineContextCallback(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const void* userContext)
{
    const RtPathTraceCleanRtxdiDiMaterialFeaturePipelineEnsureContext* ensureContext =
        static_cast<const RtPathTraceCleanRtxdiDiMaterialFeaturePipelineEnsureContext*>(userContext);
    if (!ensureContext || !ensureContext->passes || !ensureContext->pipelineContext)
    {
        return RtPathTraceMaterialFeaturePipelineContext();
    }

    return BuildPathTraceCleanRtxdiDiMaterialFeaturePipelineContext(
        *ensureContext->passes,
        registration,
        *ensureContext->pipelineContext);
}

static size_t BuildPathTraceCleanRtxdiDiMaterialFeatureRuntimePasses(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    RtPathTraceMaterialFeatureRuntimePass* runtimePasses,
    RtPathTraceMaterialFeaturePassRegistration* registrationsOut,
    size_t passCapacity)
{
    RtPathTraceMaterialFeaturePassRegistration registrations[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_CAPACITY];
    const size_t registrationCount = BuildPathTraceCleanRtxdiDiMaterialFeatureRegistrations(
        passes,
        registrations,
        sizeof(registrations) / sizeof(registrations[0]));
    const RtPathTraceCleanRtxdiDiMaterialFeatureState* featureState =
        RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::FeatureState(passes);

    return BuildPathTraceReadyMaterialFeatureRuntimePasses(
        registrations,
        Min(registrationCount, sizeof(registrations) / sizeof(registrations[0])),
        featureState
            ? RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::ShaderTableState(*featureState)
            : nullptr,
        runtimePasses,
        registrationsOut,
        passCapacity);
}

RtPathTraceCleanRtxdiDiMaterialFeatureState::RtPathTraceCleanRtxdiDiMaterialFeatureState()
    : m_impl(new Impl())
{
}

RtPathTraceCleanRtxdiDiMaterialFeatureState::~RtPathTraceCleanRtxdiDiMaterialFeatureState() = default;

RtPathTraceCleanRtxdiDiMaterialFeatureState::RtPathTraceCleanRtxdiDiMaterialFeatureState(
    RtPathTraceCleanRtxdiDiMaterialFeatureState&&) noexcept = default;

RtPathTraceCleanRtxdiDiMaterialFeatureState& RtPathTraceCleanRtxdiDiMaterialFeatureState::operator=(
    RtPathTraceCleanRtxdiDiMaterialFeatureState&&) noexcept = default;

RtPathTraceCleanRtxdiDiMaterialFeaturePasses::RtPathTraceCleanRtxdiDiMaterialFeaturePasses()
    : m_impl(new Impl())
{
}

RtPathTraceCleanRtxdiDiMaterialFeaturePasses::~RtPathTraceCleanRtxdiDiMaterialFeaturePasses() = default;

RtPathTraceCleanRtxdiDiMaterialFeaturePasses::RtPathTraceCleanRtxdiDiMaterialFeaturePasses(
    RtPathTraceCleanRtxdiDiMaterialFeaturePasses&&) noexcept = default;

RtPathTraceCleanRtxdiDiMaterialFeaturePasses& RtPathTraceCleanRtxdiDiMaterialFeaturePasses::operator=(
    RtPathTraceCleanRtxdiDiMaterialFeaturePasses&&) noexcept = default;

RtPathTraceCleanRtxdiDiPipelineContext BuildPathTraceCleanRtxdiDiPipelineContext(
    bool smokeTestInitialized,
    nvrhi::BindingLayoutHandle cleanRtxdiDiBindingLayout,
    nvrhi::BindingLayoutHandle textureBindlessLayout)
{
    return {
        smokeTestInitialized,
        cleanRtxdiDiBindingLayout,
        textureBindlessLayout
    };
}

RtPathTraceCleanRtxdiDiMaterialFeaturePasses BuildPathTraceCleanRtxdiDiMaterialFeaturePasses(
    bool cleanRouteRequested,
    int cleanView,
    RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState)
{
    RtPathTraceCleanRtxdiDiMaterialFeaturePasses passes;
    RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::Init(
        passes,
        cleanRouteRequested,
        cleanView,
        featureState);
    return passes;
}

RtPathTraceMaterialFeatureShaderState* RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::ShaderStateForRegistration(
    RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState,
    const RtPathTraceMaterialFeaturePassRegistration& registration)
{
    return featureState.m_impl
        ? PathTraceMaterialFeatureShaderStateForRegistration(registration, featureState.m_impl->shaderTableState)
        : nullptr;
}

const RtPathTraceMaterialFeatureShaderTableState* RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::ShaderTableState(
    const RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState)
{
    return featureState.m_impl
        ? &featureState.m_impl->shaderTableState
        : nullptr;
}

void RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::Init(
    RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    bool cleanRouteRequested,
    int cleanView,
    RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState)
{
    if (!passes.m_impl)
    {
        passes.m_impl.reset(new RtPathTraceCleanRtxdiDiMaterialFeaturePasses::Impl());
    }
    passes.m_impl->cleanRouteRequested = cleanRouteRequested;
    passes.m_impl->cleanView = cleanView;
    passes.m_impl->featureState = &featureState;
}

bool RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::CleanRouteRequested(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes)
{
    return passes.m_impl
        ? passes.m_impl->cleanRouteRequested
        : false;
}

int RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::CleanView(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes)
{
    return passes.m_impl
        ? passes.m_impl->cleanView
        : 0;
}

RtPathTraceCleanRtxdiDiMaterialFeatureState* RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::FeatureState(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes)
{
    return passes.m_impl
        ? passes.m_impl->featureState
        : nullptr;
}

size_t BuildPathTraceCleanRtxdiDiMaterialFeatureRegistrations(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& featurePasses,
    RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCapacity)
{
    return BuildPathTraceCleanRtxdiDiMaterialFeatureRegistryRegistrations(
        BuildPathTraceCleanRtxdiDiMaterialFeatureRegistryContext(featurePasses),
        registrations,
        registrationCapacity);
}

void AddPathTraceCleanRtxdiDiMaterialFeatureLayoutBindings(nvrhi::BindingLayoutDesc& desc)
{
    RtPathTraceMaterialFeaturePassRegistration registrations[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_CAPACITY];
    const size_t registrationCount = BuildPathTraceCleanRtxdiDiMaterialFeatureRegistryLayoutRegistrations(
        registrations,
        sizeof(registrations) / sizeof(registrations[0]));
    AddPathTraceMaterialFeatureRegistrationListLayoutBindings(
        desc,
        registrations,
        Min(registrationCount, sizeof(registrations) / sizeof(registrations[0])));
}

void AddPathTraceCleanRtxdiDiMaterialFeatureBindings(
    nvrhi::BindingSetDesc& desc,
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    nvrhi::BufferHandle materialTableBuffer,
    nvrhi::BufferHandle materialFeatureBuffer,
    nvrhi::BufferHandle materialFeatureParameterBuffer,
    nvrhi::BufferHandle runtimeConstantsBuffer,
    const RtPathTraceFrameResources& frameResources)
{
    RtPathTraceMaterialFeaturePassRegistration registrations[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_CAPACITY];
    const size_t registrationCount = BuildPathTraceCleanRtxdiDiMaterialFeatureRegistrations(
        passes,
        registrations,
        sizeof(registrations) / sizeof(registrations[0]));
    AddPathTraceMaterialFeatureRegistrationListBindings(
        desc,
        {
            frameResources.primarySurfaceHistoryBuffers.current,
            materialTableBuffer,
            materialFeatureBuffer,
            materialFeatureParameterBuffer,
            runtimeConstantsBuffer
        },
        frameResources,
        registrations,
        Min(registrationCount, sizeof(registrations) / sizeof(registrations[0])));
}

bool PathTraceCleanRtxdiDiMaterialFeatureOutputsAvailable(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    const RtPathTraceFrameResources& frameResources)
{
    RtPathTraceMaterialFeaturePassRegistration registrations[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_CAPACITY];
    const size_t registrationCount = BuildPathTraceCleanRtxdiDiMaterialFeatureRegistrations(
        passes,
        registrations,
        sizeof(registrations) / sizeof(registrations[0]));
    return PathTraceMaterialFeatureRegistrationListOutputsAvailable(
        registrations,
        Min(registrationCount, sizeof(registrations) / sizeof(registrations[0])),
        frameResources);
}

bool PathTraceCleanRtxdiDiMaterialFeatureNeedsOutputColorSource(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes)
{
    RtPathTraceMaterialFeaturePassRegistration registrations[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_CAPACITY];
    const size_t registrationCount = BuildPathTraceCleanRtxdiDiMaterialFeatureRegistrations(
        passes,
        registrations,
        sizeof(registrations) / sizeof(registrations[0]));
    for (size_t i = 0; i < Min(registrationCount, sizeof(registrations) / sizeof(registrations[0])); ++i)
    {
        const RtPathTraceMaterialFeaturePassDesc& passDesc = registrations[i].passDesc;
        if (passDesc.enabled &&
            PathTraceMaterialFeaturePassReadsAnyInput(passDesc, RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR_SOURCE))
        {
            return true;
        }
    }
    return false;
}

bool EnsurePathTraceCleanRtxdiDiMaterialFeaturePassPipelines(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    const RtPathTraceCleanRtxdiDiPipelineContext& context)
{
    RtPathTraceMaterialFeaturePassRegistration registrations[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_CAPACITY];
    const size_t registrationCount = BuildPathTraceCleanRtxdiDiMaterialFeatureRegistrations(
        passes,
        registrations,
        sizeof(registrations) / sizeof(registrations[0]));
    const RtPathTraceCleanRtxdiDiMaterialFeaturePipelineEnsureContext ensureContext = { &passes, &context };
    return EnsurePathTraceMaterialFeatureRegistrationListPipelines(
        registrations,
        Min(registrationCount, sizeof(registrations) / sizeof(registrations[0])),
        BuildPathTraceCleanRtxdiDiMaterialFeaturePipelineContextCallback,
        &ensureContext,
        "clean-room RTXDI DI");
}

bool EnsurePathTraceCleanRtxdiDiMaterialFeatureLayoutPipelines(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    const RtPathTraceCleanRtxdiDiPipelineContext& context)
{
    RtPathTraceMaterialFeaturePassRegistration registrations[
        RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_CAPACITY];
    const size_t registrationCount =
        BuildPathTraceCleanRtxdiDiMaterialFeatureRegistryLayoutRegistrations(
            registrations,
            sizeof(registrations) / sizeof(registrations[0]));
    const RtPathTraceCleanRtxdiDiMaterialFeaturePipelineEnsureContext
        ensureContext = { &passes, &context };
    return EnsurePathTraceMaterialFeatureRegistrationListPipelines(
        registrations,
        Min(registrationCount, sizeof(registrations) / sizeof(registrations[0])),
        BuildPathTraceCleanRtxdiDiMaterialFeaturePipelineContextCallback,
        &ensureContext,
        "clean-room RTXDI DI layout probe");
}

void SetPathTraceCleanRtxdiDiMaterialFeatureOutputsUnorderedAccess(
    nvrhi::ICommandList* commandList,
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    const RtPathTraceFrameResources& frameResources)
{
    RtPathTraceMaterialFeatureRuntimePass runtimePasses[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_CAPACITY];
    const size_t passCount = BuildPathTraceCleanRtxdiDiMaterialFeatureRuntimePasses(
        passes,
        runtimePasses,
        nullptr,
        sizeof(runtimePasses) / sizeof(runtimePasses[0]));
    SetPathTraceMaterialFeatureRuntimePassOutputsState(
        commandList,
        runtimePasses,
        Min(passCount, sizeof(runtimePasses) / sizeof(runtimePasses[0])),
        frameResources,
        nvrhi::ResourceStates::UnorderedAccess);
}

void ClearPathTraceCleanRtxdiDiMaterialFeatureOutputs(
    nvrhi::ICommandList* commandList,
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    const RtPathTraceFrameResources& frameResources)
{
    RtPathTraceMaterialFeatureRuntimePass runtimePasses[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_CAPACITY];
    const size_t passCount = BuildPathTraceCleanRtxdiDiMaterialFeatureRuntimePasses(
        passes,
        runtimePasses,
        nullptr,
        sizeof(runtimePasses) / sizeof(runtimePasses[0]));
    ClearPathTraceMaterialFeatureRuntimePassPrimaryOutputs(
        commandList,
        runtimePasses,
        Min(passCount, sizeof(runtimePasses) / sizeof(runtimePasses[0])),
        frameResources,
        nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
}

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
    bool nsightGpuMarkers)
{
    RtPathTraceMaterialFeatureRuntimePass runtimePasses[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_CAPACITY];
    const size_t passCount = BuildPathTraceCleanRtxdiDiMaterialFeatureRuntimePasses(
        passes,
        runtimePasses,
        nullptr,
        sizeof(runtimePasses) / sizeof(runtimePasses[0]));
    DispatchPathTraceMaterialFeaturePassesWithRuntimeInfo(
        commandList,
        baseState,
        args,
        constantsBuffer,
        baseConstants,
        baseConstantsSize,
        runtimeConstantsBuffer,
        runtimePasses,
        Min(passCount, sizeof(runtimePasses) / sizeof(runtimePasses[0])),
        frameResources,
        nsightGpuMarkers);
}

void DispatchPathTraceCleanRtxdiDiTransmissionPsrPass(
    nvrhi::ICommandList* commandList,
    const nvrhi::rt::State& baseState,
    const nvrhi::rt::DispatchRaysArguments& args,
    nvrhi::BufferHandle constantsBuffer,
    const void* baseConstants,
    size_t baseConstantsSize,
    nvrhi::BufferHandle runtimeConstantsBuffer,
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    const RtPathTraceFrameResources& frameResources,
    bool nsightGpuMarkers)
{
    RtPathTraceMaterialFeatureRuntimePass runtimePasses[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_CAPACITY];
    const size_t passCount = BuildPathTraceCleanRtxdiDiMaterialFeatureRuntimePasses(
        passes,
        runtimePasses,
        nullptr,
        sizeof(runtimePasses) / sizeof(runtimePasses[0]));
    for (size_t i = 0; i < Min(passCount, sizeof(runtimePasses) / sizeof(runtimePasses[0])); ++i)
    {
        if (!runtimePasses[i].desc.featureId ||
            idStr::Cmp(runtimePasses[i].desc.featureId, "clean-rtxdi-di-transmission") != 0)
        {
            continue;
        }
        DispatchPathTraceMaterialFeaturePassWithRuntimeInfo(
            commandList,
            baseState,
            args,
            constantsBuffer,
            baseConstants,
            baseConstantsSize,
            runtimeConstantsBuffer,
            runtimePasses[i],
            frameResources,
            nsightGpuMarkers);
    }
}

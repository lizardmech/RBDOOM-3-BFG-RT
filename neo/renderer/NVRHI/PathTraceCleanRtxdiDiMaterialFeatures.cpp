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

struct RtPathTraceCleanRtxdiDiMaterialFeaturePipelineContext
{
    RtPathTraceMaterialFeatureShaderState* shaderState = nullptr;
    bool smokeTestInitialized = false;
    nvrhi::BindingLayoutHandle cleanRtxdiDiBindingLayout;
    nvrhi::BindingLayoutHandle textureBindlessLayout;
};

static RtPathTraceCleanRtxdiDiMaterialFeatureRegistryContext BuildPathTraceCleanRtxdiDiMaterialFeatureRegistryContext(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes)
{
    return {
        RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::CleanRouteRequested(passes),
        RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::CleanView(passes)
    };
}

static RtPathTraceCleanRtxdiDiMaterialFeaturePipelineContext BuildPathTraceCleanRtxdiDiMaterialFeaturePipelineContext(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const RtPathTraceCleanRtxdiDiPipelineContext& context)
{
    RtPathTraceCleanRtxdiDiMaterialFeatureState* featureState =
        RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::FeatureState(passes);
    return {
        featureState
            ? RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::ShaderStateForRegistration(*featureState, registration)
            : nullptr,
        context.smokeTestInitialized,
        context.cleanRtxdiDiBindingLayout,
        context.textureBindlessLayout
    };
}

static bool InitPathTraceCleanRtxdiDiMaterialFeaturePipeline(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const RtPathTraceCleanRtxdiDiMaterialFeaturePipelineContext& context)
{
    if (!context.shaderState)
    {
        return false;
    }

    RtPathTraceMaterialFeatureShaderState* materialFeatureShaderState = context.shaderState;
    if (materialFeatureShaderState->shaderTable)
    {
        return true;
    }

    if (!context.smokeTestInitialized || !context.textureBindlessLayout)
    {
        return false;
    }

    if (!PathTraceMaterialFeatureBindingMetadataCoversPass(registration))
    {
        common->Printf("PathTracePrimaryPass: %s material-feature registration is missing binding metadata\n", registration.shaderDesc.label);
        return false;
    }

    nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
    if (!device)
    {
        return false;
    }

    const RtPathTraceMaterialFeaturePipelineRequest pipelineRequest = BuildPathTraceMaterialFeaturePipelineRequest(
        registration.shaderDesc,
        context.shaderState,
        context.cleanRtxdiDiBindingLayout,
        deviceManager->GetGraphicsAPI());
    if (!pipelineRequest.shaderState)
    {
        return false;
    }

    if (!pipelineRequest.bindingLayout || pipelineRequest.shaderPath.empty())
    {
        return false;
    }

    RtPathTraceMaterialFeatureShaderState& shaderState = *pipelineRequest.shaderState;
    if (!shaderState.shaderLibrary &&
        !LoadPathTraceMaterialFeatureShaderLibrary(device, pipelineRequest.shaderPath.c_str(), pipelineRequest.shaderDesc.label, shaderState.shaderLibrary))
    {
        common->Printf("PathTracePrimaryPass: %s material-feature RT shader unavailable; matching material-feature passes will be disabled\n", pipelineRequest.shaderDesc.label);
        return false;
    }

    if (!CreatePathTraceMaterialFeatureRayTracingPipeline(
        device,
        shaderState.shaderLibrary,
        pipelineRequest.bindingLayout,
        context.textureBindlessLayout,
        pipelineRequest.shaderDesc,
        shaderState.pipeline,
        shaderState.shaderTable))
    {
        common->Printf("PathTracePrimaryPass: %s material-feature RT pipeline unavailable; matching material-feature passes will be disabled\n", pipelineRequest.shaderDesc.label);
        shaderState.pipeline = nullptr;
        shaderState.shaderTable = nullptr;
        return false;
    }

    common->Printf(
        "PathTracePrimaryPass: %s material-feature RT pipeline initialized; feature='%s' validation build='%s' runtime='%s' supported='%s' unsupported='%s' baseline='%s' resources='%s' abi='%s'\n",
        pipelineRequest.shaderDesc.label,
        registration.passDesc.featureId,
        registration.validation.buildProof,
        registration.validation.runtimeRoute,
        registration.validation.supportedMaterialTest,
        registration.validation.unsupportedMaterialTest,
        registration.validation.baselineRegressionCheck,
        registration.validation.resourceBindingProof,
        registration.validation.cpuShaderAbiProof);
    return true;
}

static bool EnsurePathTraceCleanRtxdiDiMaterialFeatureRuntimePassPipeline(
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const RtPathTraceCleanRtxdiDiMaterialFeaturePipelineContext& context)
{
    if (!pass.pipelineRequested)
    {
        return true;
    }
    if (!pass.shader)
    {
        return false;
    }
    if (!pass.shader->shaderTable)
    {
        InitPathTraceCleanRtxdiDiMaterialFeaturePipeline(registration, context);
    }
    return static_cast<bool>(pass.shader->shaderTable);
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
    for (size_t i = 0; i < registrationCount && i < sizeof(registrations) / sizeof(registrations[0]); ++i)
    {
        AddPathTraceMaterialFeatureRegistrationLayoutBindings(desc, registrations[i]);
    }
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
    for (size_t i = 0; i < registrationCount && i < sizeof(registrations) / sizeof(registrations[0]); ++i)
    {
        AddPathTraceMaterialFeatureRegistrationBindings(
            desc,
            {
                frameResources.primarySurfaceHistoryBuffers.current,
                materialTableBuffer,
                materialFeatureBuffer,
                materialFeatureParameterBuffer,
                runtimeConstantsBuffer
            },
            frameResources,
            registrations[i]);
    }
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
    for (size_t i = 0; i < registrationCount && i < sizeof(registrations) / sizeof(registrations[0]); ++i)
    {
        const RtPathTraceMaterialFeaturePassDesc& passDesc = registrations[i].passDesc;
        if (!PathTraceMaterialFeatureOutputsAvailable(passDesc, frameResources))
        {
            return false;
        }
    }
    return true;
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
    RtPathTraceCleanRtxdiDiMaterialFeatureState* featureState =
        RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::FeatureState(passes);

    for (size_t i = 0; i < registrationCount && i < sizeof(registrations) / sizeof(registrations[0]); ++i)
    {
        const RtPathTraceMaterialFeaturePassRegistration& registration = registrations[i];
        if (!registration.passDesc.enabled)
        {
            continue;
        }

        if (!ValidatePathTraceMaterialFeatureRegistration(registration, "clean-room RTXDI DI"))
        {
            return false;
        }

        RtPathTraceMaterialFeatureShaderState* shaderState = featureState
            ? RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::ShaderStateForRegistration(*featureState, registration)
            : nullptr;
        RtPathTraceMaterialFeatureRuntimePass pipelinePass =
            BuildPathTraceMaterialFeatureRuntimePass(registration, shaderState);
        if (!EnsurePathTraceCleanRtxdiDiMaterialFeatureRuntimePassPipeline(
            pipelinePass,
            registration,
            BuildPathTraceCleanRtxdiDiMaterialFeaturePipelineContext(passes, registration, context)))
        {
            return false;
        }
    }
    return true;
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

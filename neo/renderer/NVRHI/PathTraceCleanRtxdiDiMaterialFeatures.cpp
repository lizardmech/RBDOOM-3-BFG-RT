#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiMaterialFeatures.h"
#include "PathTraceCleanRtxdiDiMaterialFeatureRegistry.h"
#include "PathTraceMaterialFeatureBindings.h"
#include "PathTraceMaterialFeatureDispatch.h"
#include "PathTraceMaterialFeatureOutputs.h"
#include "PathTraceMaterialFeatureRuntime.h"

#include "../../sys/DeviceManager.h"

extern DeviceManager* deviceManager;

struct RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess
{
    static RtPathTraceMaterialFeatureShaderState* ShaderStateForRegistration(
        RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState,
        const RtPathTraceMaterialFeaturePassRegistration& registration);
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

static bool LoadPathTraceCleanRtxdiDiMaterialFeatureShaderLibrary(
    nvrhi::IDevice* device,
    const char* shaderPath,
    const char* label,
    nvrhi::ShaderLibraryHandle& shaderLibrary)
{
    shaderLibrary = nullptr;

    void* shaderData = nullptr;
    ID_TIME_T shaderTimestamp = 0;
    const int shaderSize = fileSystem->ReadFile(shaderPath, &shaderData, &shaderTimestamp);
    if (shaderSize <= 0 || !shaderData)
    {
        common->Printf("PathTracePrimaryPass: couldn't read %s material-feature RT shader %s\n", label, shaderPath);
        return false;
    }

    common->Printf("PathTracePrimaryPass: loaded %s material-feature RT shader %s (%d bytes, timestamp %u)\n",
        label, shaderPath, shaderSize, static_cast<unsigned int>(shaderTimestamp));

    shaderLibrary = device->createShaderLibrary(shaderData, shaderSize);
    Mem_Free(shaderData);

    if (!shaderLibrary)
    {
        common->Printf("PathTracePrimaryPass: failed to create %s material-feature RT shader library\n", label);
        return false;
    }

    return true;
}

static bool CreatePathTraceCleanRtxdiDiMaterialFeatureRayTracingPipeline(
    nvrhi::IDevice* device,
    nvrhi::ShaderLibraryHandle shaderLibrary,
    nvrhi::BindingLayoutHandle bindingLayout,
    nvrhi::BindingLayoutHandle textureBindlessLayout,
    const RtPathTraceMaterialFeatureShaderDesc& shaderDesc,
    nvrhi::rt::PipelineHandle& pipeline,
    nvrhi::rt::ShaderTableHandle& shaderTable)
{
    pipeline = nullptr;
    shaderTable = nullptr;

    const RtPathTraceMaterialFeatureRayTracingPipelineDesc& rtDesc = shaderDesc.rtPipeline;
    const char* label = shaderDesc.label;
    if (!shaderLibrary)
    {
        common->Printf("PathTracePrimaryPass: cannot create %s material-feature RT pipeline without a shader library\n", label);
        return false;
    }

    nvrhi::ShaderHandle rayGen = shaderLibrary->getShader(rtDesc.rayGenerationShader, nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle miss = shaderLibrary->getShader(rtDesc.missShader, nvrhi::ShaderType::Miss);
    nvrhi::ShaderHandle shadowMiss = shaderLibrary->getShader(rtDesc.shadowMissShader, nvrhi::ShaderType::Miss);
    nvrhi::ShaderHandle closestHit = shaderLibrary->getShader(rtDesc.closestHitShader, nvrhi::ShaderType::ClosestHit);
    nvrhi::ShaderHandle anyHit = shaderLibrary->getShader(rtDesc.anyHitShader, nvrhi::ShaderType::AnyHit);
    nvrhi::ShaderHandle shadowClosestHit = shaderLibrary->getShader(rtDesc.shadowClosestHitShader, nvrhi::ShaderType::ClosestHit);
    nvrhi::ShaderHandle shadowAnyHit = shaderLibrary->getShader(rtDesc.shadowAnyHitShader, nvrhi::ShaderType::AnyHit);

    if (!rayGen || !miss || !shadowMiss || !closestHit || !anyHit || !shadowClosestHit || !shadowAnyHit)
    {
        common->Printf("PathTracePrimaryPass: %s material-feature RT shader library is missing one or more required entry points\n", label);
        return false;
    }

    nvrhi::rt::PipelineDesc pipelineDesc;
    pipelineDesc.globalBindingLayouts = { bindingLayout, textureBindlessLayout };
    pipelineDesc.shaders = {
        { "", rayGen, nullptr },
        { "", miss, nullptr },
        { "", shadowMiss, nullptr }
    };
    pipelineDesc.hitGroups = {
        {
            rtDesc.hitGroupName,
            closestHit,
            anyHit,
            nullptr,
            nullptr,
            false
        },
        {
            rtDesc.shadowHitGroupName,
            shadowClosestHit,
            shadowAnyHit,
            nullptr,
            nullptr,
            false
        }
    };
    pipelineDesc.maxPayloadSize = rtDesc.maxPayloadSize;
    pipelineDesc.maxAttributeSize = rtDesc.maxAttributeSize;
    pipelineDesc.maxRecursionDepth = rtDesc.maxRecursionDepth;

    pipeline = device->createRayTracingPipeline(pipelineDesc);
    if (!pipeline)
    {
        common->Printf("PathTracePrimaryPass: failed to create %s material-feature RT pipeline\n", label);
        return false;
    }

    shaderTable = pipeline->createShaderTable();
    if (!shaderTable)
    {
        common->Printf("PathTracePrimaryPass: failed to create %s material-feature RT shader table\n", label);
        pipeline = nullptr;
        return false;
    }

    shaderTable->setRayGenerationShader(rtDesc.rayGenerationShader);
    shaderTable->addMissShader(rtDesc.missShader);
    shaderTable->addMissShader(rtDesc.shadowMissShader);
    shaderTable->addHitGroup(rtDesc.hitGroupName);
    shaderTable->addHitGroup(rtDesc.shadowHitGroupName);
    return true;
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
        !LoadPathTraceCleanRtxdiDiMaterialFeatureShaderLibrary(device, pipelineRequest.shaderPath.c_str(), pipelineRequest.shaderDesc.label, shaderState.shaderLibrary))
    {
        common->Printf("PathTracePrimaryPass: %s material-feature RT shader unavailable; matching material-feature passes will be disabled\n", pipelineRequest.shaderDesc.label);
        return false;
    }

    if (!CreatePathTraceCleanRtxdiDiMaterialFeatureRayTracingPipeline(
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
        "PathTracePrimaryPass: %s material-feature RT pipeline initialized; validation build='%s' runtime='%s' supported='%s' unsupported='%s' baseline='%s' resources='%s' abi='%s'\n",
        pipelineRequest.shaderDesc.label,
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
    if (!pass.ready)
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
    RtPathTraceCleanRtxdiDiMaterialFeatureState* featureState =
        RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::FeatureState(passes);

    size_t passCount = 0;
    for (size_t i = 0; i < registrationCount && i < sizeof(registrations) / sizeof(registrations[0]); ++i)
    {
        const RtPathTraceMaterialFeaturePassRegistration& registration = registrations[i];
        RtPathTraceMaterialFeatureShaderState* shaderState = featureState
            ? RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::ShaderStateForRegistration(*featureState, registration)
            : nullptr;
        RtPathTraceMaterialFeatureRuntimePass runtimePass =
            BuildPathTraceMaterialFeatureRuntimePass(registration, shaderState);
        if (!runtimePass.ready)
        {
            continue;
        }
        if (runtimePasses && passCount < passCapacity)
        {
            runtimePasses[passCount] = runtimePass;
        }
        if (registrationsOut && passCount < passCapacity)
        {
            registrationsOut[passCount] = registration;
        }
        ++passCount;
    }
    return passCount;
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
    RtPathTraceMaterialFeatureRuntimePass runtimePasses[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_CAPACITY];
    RtPathTraceMaterialFeaturePassRegistration registrations[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_CAPACITY];
    const size_t passCount = BuildPathTraceCleanRtxdiDiMaterialFeatureRuntimePasses(
        passes,
        runtimePasses,
        registrations,
        sizeof(runtimePasses) / sizeof(runtimePasses[0]));
    for (size_t i = 0; i < passCount && i < sizeof(runtimePasses) / sizeof(runtimePasses[0]); ++i)
    {
        if (!EnsurePathTraceCleanRtxdiDiMaterialFeatureRuntimePassPipeline(
            runtimePasses[i],
            registrations[i],
            BuildPathTraceCleanRtxdiDiMaterialFeaturePipelineContext(passes, registrations[i], context)))
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
    for (size_t i = 0; i < passCount && i < sizeof(runtimePasses) / sizeof(runtimePasses[0]); ++i)
    {
        SetPathTraceMaterialFeatureOutputsState(
            commandList,
            runtimePasses[i],
            frameResources,
            nvrhi::ResourceStates::UnorderedAccess);
    }
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
    uint32_t clearedPrimaryOutputs = RT_MATERIAL_FEATURE_RESOURCE_NONE;
    for (size_t i = 0; i < passCount && i < sizeof(runtimePasses) / sizeof(runtimePasses[0]); ++i)
    {
        const uint32_t primaryOutputResource = runtimePasses[i].desc.primaryOutputResource;
        if (primaryOutputResource == RT_MATERIAL_FEATURE_RESOURCE_NONE ||
            (clearedPrimaryOutputs & primaryOutputResource) != 0u)
        {
            continue;
        }
        ClearPathTraceMaterialFeaturePrimaryOutput(
            commandList,
            runtimePasses[i],
            frameResources,
            nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
        clearedPrimaryOutputs |= primaryOutputResource;
    }
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
    for (size_t i = 0; i < passCount && i < sizeof(runtimePasses) / sizeof(runtimePasses[0]); ++i)
    {
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

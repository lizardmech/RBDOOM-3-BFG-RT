#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiMaterialFeatures.h"
#include "PathTraceCleanRtxdiDiTransmissionFeature.h"
#include "PathTraceCVars.h"
#include "PathTraceMaterialFeatureBindings.h"
#include "PathTraceMaterialFeatureDispatch.h"
#include "PathTraceMaterialFeatureOutputs.h"
#include "PathTraceMaterialFeatureRuntime.h"

#include "../../sys/DeviceManager.h"

extern DeviceManager* deviceManager;

struct RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess
{
    static RtPathTraceMaterialFeatureShaderState* ShaderStateForPass(
        RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState,
        const RtPathTraceMaterialFeaturePassDesc& passDesc);
};

struct RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess
{
    static void Init(
        RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
        bool cleanRouteRequested,
        int cleanView,
        bool producerRequested,
        bool debugOutputRequested,
        RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState);
    static bool CleanRouteRequested(const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes);
    static int CleanView(const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes);
    static bool ProducerRequested(const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes);
    static bool DebugOutputRequested(const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes);
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
    bool producerRequested = false;
    bool debugOutputRequested = false;
    RtPathTraceCleanRtxdiDiMaterialFeatureState* featureState = nullptr;
};

struct RtPathTraceCleanRtxdiDiMaterialFeaturePipelineContext
{
    RtPathTraceMaterialFeatureShaderState* shaderState = nullptr;
    bool smokeTestInitialized = false;
    nvrhi::BindingLayoutHandle cleanRtxdiDiBindingLayout;
    nvrhi::BindingLayoutHandle textureBindlessLayout;
};

static constexpr size_t RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_COUNT = 2;

static RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiTransmissionFeatureRegistration(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes)
{
    return BuildPathTraceCleanRtxdiDiTransmissionFeatureRegistration(
        RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::CleanRouteRequested(passes),
        RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::CleanView(passes),
        RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::ProducerRequested(passes),
        RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::DebugOutputRequested(passes));
}

static RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiNoOpFeatureRegistration()
{
    RtPathTraceMaterialFeaturePassRegistration registration;
    registration.passDesc.debugLabel = "clean-rtxdi-di-noop-feature";
    registration.validation = {
        "host registry only",
        "disabled no-op registration",
        "not applicable",
        "not applicable",
        "clean RTXDI DI primary view 16 unchanged",
        "no resources or bindings",
        "no constants"
    };
    return registration;
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
            ? RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::ShaderStateForPass(*featureState, registration.passDesc)
            : nullptr,
        context.smokeTestInitialized,
        context.cleanRtxdiDiBindingLayout,
        context.textureBindlessLayout
    };
}

static RtPathTraceMaterialFeaturePassDesc BuildPathTraceCleanRtxdiDiMaterialFeatureLayoutPassDesc()
{
    RtPathTraceMaterialFeaturePassDesc layoutDesc;
    const RtPathTraceMaterialFeaturePassRegistration registrations[] = {
        BuildPathTraceCleanRtxdiDiTransmissionFeatureRegistration(true, 16, true, true),
        BuildPathTraceCleanRtxdiDiNoOpFeatureRegistration()
    };
    for (const RtPathTraceMaterialFeaturePassRegistration& registration : registrations)
    {
        layoutDesc.resourceInputs |= registration.passDesc.resourceInputs;
        layoutDesc.resourceOutputs |= registration.passDesc.resourceOutputs;
    }
    return layoutDesc;
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
        common->Printf("PathTracePrimaryPass: couldn't read %s RT smoke shader %s\n", label, shaderPath);
        return false;
    }

    common->Printf("PathTracePrimaryPass: loaded %s RT smoke shader %s (%d bytes, timestamp %u)\n",
        label, shaderPath, shaderSize, static_cast<unsigned int>(shaderTimestamp));

    shaderLibrary = device->createShaderLibrary(shaderData, shaderSize);
    Mem_Free(shaderData);

    if (!shaderLibrary)
    {
        common->Printf("PathTracePrimaryPass: failed to create %s RT smoke shader library\n", label);
        return false;
    }

    return true;
}

static bool CreatePathTraceCleanRtxdiDiMaterialFeatureRayTracingPipeline(
    nvrhi::IDevice* device,
    nvrhi::ShaderLibraryHandle shaderLibrary,
    nvrhi::BindingLayoutHandle bindingLayout,
    nvrhi::BindingLayoutHandle textureBindlessLayout,
    const char* label,
    nvrhi::rt::PipelineHandle& pipeline,
    nvrhi::rt::ShaderTableHandle& shaderTable)
{
    pipeline = nullptr;
    shaderTable = nullptr;

    if (!shaderLibrary)
    {
        common->Printf("PathTracePrimaryPass: cannot create %s RT smoke pipeline without a shader library\n", label);
        return false;
    }

    nvrhi::ShaderHandle rayGen = shaderLibrary->getShader("RayGen", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle miss = shaderLibrary->getShader("Miss", nvrhi::ShaderType::Miss);
    nvrhi::ShaderHandle shadowMiss = shaderLibrary->getShader("ShadowMiss", nvrhi::ShaderType::Miss);
    nvrhi::ShaderHandle closestHit = shaderLibrary->getShader("ClosestHit", nvrhi::ShaderType::ClosestHit);
    nvrhi::ShaderHandle anyHit = shaderLibrary->getShader("AnyHit", nvrhi::ShaderType::AnyHit);
    nvrhi::ShaderHandle shadowClosestHit = shaderLibrary->getShader("ShadowClosestHit", nvrhi::ShaderType::ClosestHit);
    nvrhi::ShaderHandle shadowAnyHit = shaderLibrary->getShader("ShadowAnyHit", nvrhi::ShaderType::AnyHit);

    if (!rayGen || !miss || !shadowMiss || !closestHit || !anyHit || !shadowClosestHit || !shadowAnyHit)
    {
        common->Printf("PathTracePrimaryPass: %s RT smoke shader library is missing one or more required entry points\n", label);
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
            "HitGroup",
            closestHit,
            anyHit,
            nullptr,
            nullptr,
            false
        },
        {
            "ShadowHitGroup",
            shadowClosestHit,
            shadowAnyHit,
            nullptr,
            nullptr,
            false
        }
    };
    pipelineDesc.maxPayloadSize = 64;
    pipelineDesc.maxAttributeSize = 8;
    pipelineDesc.maxRecursionDepth = 1;

    pipeline = device->createRayTracingPipeline(pipelineDesc);
    if (!pipeline)
    {
        common->Printf("PathTracePrimaryPass: failed to create %s RT smoke pipeline\n", label);
        return false;
    }

    shaderTable = pipeline->createShaderTable();
    if (!shaderTable)
    {
        common->Printf("PathTracePrimaryPass: failed to create %s RT smoke shader table\n", label);
        pipeline = nullptr;
        return false;
    }

    shaderTable->setRayGenerationShader("RayGen");
    shaderTable->addMissShader("Miss");
    shaderTable->addMissShader("ShadowMiss");
    shaderTable->addHitGroup("HitGroup");
    shaderTable->addHitGroup("ShadowHitGroup");
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

    if (!pipelineRequest.bindingLayout || !pipelineRequest.shaderPath)
    {
        return false;
    }

    RtPathTraceMaterialFeatureShaderState& shaderState = *pipelineRequest.shaderState;
    if (!shaderState.shaderLibrary &&
        !LoadPathTraceCleanRtxdiDiMaterialFeatureShaderLibrary(device, pipelineRequest.shaderPath, pipelineRequest.shaderDesc.label, shaderState.shaderLibrary))
    {
        common->Printf("PathTracePrimaryPass: %s RT smoke shader unavailable; matching material-feature passes will be disabled\n", pipelineRequest.shaderDesc.label);
        return false;
    }

    if (!CreatePathTraceCleanRtxdiDiMaterialFeatureRayTracingPipeline(
        device,
        shaderState.shaderLibrary,
        pipelineRequest.bindingLayout,
        context.textureBindlessLayout,
        pipelineRequest.shaderDesc.label,
        shaderState.pipeline,
        shaderState.shaderTable))
    {
        common->Printf("PathTracePrimaryPass: %s RT smoke pipeline unavailable; matching material-feature passes will be disabled\n", pipelineRequest.shaderDesc.label);
        shaderState.pipeline = nullptr;
        shaderState.shaderTable = nullptr;
        return false;
    }

    common->Printf(
        "PathTracePrimaryPass: %s RT smoke pipeline initialized; material-feature validation build='%s' runtime='%s' supported='%s' unsupported='%s' baseline='%s' resources='%s' abi='%s'\n",
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
    RtPathTraceMaterialFeaturePassRegistration registrations[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_COUNT];
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
            ? RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::ShaderStateForPass(*featureState, registration.passDesc)
            : nullptr;
        RtPathTraceMaterialFeatureRuntimePass runtimePass =
            BuildPathTraceMaterialFeatureRuntimePass(registration.passDesc, shaderState);
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
        r_pathTracingCleanRtxdiDiTransmissionProducer.GetInteger() != 0,
        r_pathTracingCleanRtxdiDiTransmissionDebugView.GetInteger() != 0,
        featureState);
    return passes;
}

RtPathTraceMaterialFeatureShaderState* RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::ShaderStateForPass(
    RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState,
    const RtPathTraceMaterialFeaturePassDesc& passDesc)
{
    return featureState.m_impl
        ? PathTraceMaterialFeatureShaderStateForPass(passDesc, featureState.m_impl->shaderTableState)
        : nullptr;
}

void RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::Init(
    RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    bool cleanRouteRequested,
    int cleanView,
    bool producerRequested,
    bool debugOutputRequested,
    RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState)
{
    if (!passes.m_impl)
    {
        passes.m_impl.reset(new RtPathTraceCleanRtxdiDiMaterialFeaturePasses::Impl());
    }
    passes.m_impl->cleanRouteRequested = cleanRouteRequested;
    passes.m_impl->cleanView = cleanView;
    passes.m_impl->producerRequested = producerRequested;
    passes.m_impl->debugOutputRequested = debugOutputRequested;
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

bool RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::ProducerRequested(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes)
{
    return passes.m_impl
        ? passes.m_impl->producerRequested
        : false;
}

bool RtPathTraceCleanRtxdiDiMaterialFeaturePassesAccess::DebugOutputRequested(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes)
{
    return passes.m_impl
        ? passes.m_impl->debugOutputRequested
        : false;
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
    static constexpr size_t registrationCount = RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_COUNT;
    if (registrations && registrationCapacity > 0)
    {
        registrations[0] = BuildPathTraceCleanRtxdiDiTransmissionFeatureRegistration(featurePasses);
        if (registrationCapacity > 1)
        {
            registrations[1] = BuildPathTraceCleanRtxdiDiNoOpFeatureRegistration();
        }
    }
    return registrationCount;
}

void AddPathTraceCleanRtxdiDiMaterialFeatureLayoutBindings(nvrhi::BindingLayoutDesc& desc)
{
    const RtPathTraceMaterialFeaturePassDesc featureDesc =
        BuildPathTraceCleanRtxdiDiMaterialFeatureLayoutPassDesc();
    AddPathTraceMaterialFeatureInputLayoutBindings(desc, featureDesc.resourceInputs);
    AddPathTraceMaterialFeatureOutputLayoutBindings(desc, featureDesc.resourceOutputs);
}

void AddPathTraceCleanRtxdiDiMaterialFeatureBindings(
    nvrhi::BindingSetDesc& desc,
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    nvrhi::BufferHandle materialFeatureBuffer,
    nvrhi::BufferHandle runtimeConstantsBuffer,
    const RtPathTraceFrameResources& frameResources)
{
    RtPathTraceMaterialFeaturePassDesc featureDesc;
    RtPathTraceMaterialFeaturePassRegistration registrations[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_COUNT];
    const size_t registrationCount = BuildPathTraceCleanRtxdiDiMaterialFeatureRegistrations(
        passes,
        registrations,
        sizeof(registrations) / sizeof(registrations[0]));
    for (size_t i = 0; i < registrationCount && i < sizeof(registrations) / sizeof(registrations[0]); ++i)
    {
        featureDesc.resourceInputs |= registrations[i].passDesc.resourceInputs;
        featureDesc.resourceOutputs |= registrations[i].passDesc.resourceOutputs;
    }
    AddPathTraceMaterialFeatureInputBindings(
        desc,
        {
            materialFeatureBuffer,
            runtimeConstantsBuffer
        },
        featureDesc.resourceInputs);
    AddPathTraceMaterialFeatureOutputBindings(desc, frameResources, featureDesc.resourceOutputs);
}

bool PathTraceCleanRtxdiDiMaterialFeatureOutputsAvailable(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses& passes,
    const RtPathTraceFrameResources& frameResources)
{
    RtPathTraceMaterialFeaturePassRegistration registrations[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_COUNT];
    const size_t registrationCount = BuildPathTraceCleanRtxdiDiMaterialFeatureRegistrations(
        passes,
        registrations,
        sizeof(registrations) / sizeof(registrations[0]));
    for (size_t i = 0; i < registrationCount && i < sizeof(registrations) / sizeof(registrations[0]); ++i)
    {
        const RtPathTraceMaterialFeaturePassDesc& passDesc = registrations[i].passDesc;
        if (!PathTraceMaterialFeaturePrimaryOutputAvailable(passDesc, frameResources) ||
            !PathTraceMaterialFeatureOutputAvailable(passDesc, frameResources, RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR))
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
    RtPathTraceMaterialFeatureRuntimePass runtimePasses[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_COUNT];
    RtPathTraceMaterialFeaturePassRegistration registrations[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_COUNT];
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
    RtPathTraceMaterialFeatureRuntimePass runtimePasses[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_COUNT];
    const size_t passCount = BuildPathTraceCleanRtxdiDiMaterialFeatureRuntimePasses(
        passes,
        runtimePasses,
        nullptr,
        sizeof(runtimePasses) / sizeof(runtimePasses[0]));
    for (size_t i = 0; i < passCount && i < sizeof(runtimePasses) / sizeof(runtimePasses[0]); ++i)
    {
        SetPathTraceMaterialFeaturePrimaryOutputState(
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
    RtPathTraceMaterialFeatureRuntimePass runtimePasses[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_COUNT];
    const size_t passCount = BuildPathTraceCleanRtxdiDiMaterialFeatureRuntimePasses(
        passes,
        runtimePasses,
        nullptr,
        sizeof(runtimePasses) / sizeof(runtimePasses[0]));
    for (size_t i = 0; i < passCount && i < sizeof(runtimePasses) / sizeof(runtimePasses[0]); ++i)
    {
        ClearPathTraceMaterialFeaturePrimaryOutput(
            commandList,
            runtimePasses[i],
            frameResources,
            nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
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
    RtPathTraceMaterialFeatureRuntimePass runtimePasses[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_COUNT];
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

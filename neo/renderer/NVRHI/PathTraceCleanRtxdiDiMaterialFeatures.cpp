#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiMaterialFeatures.h"
#include "PathTraceCVars.h"
#include "PathTraceMaterialFeatureBindings.h"
#include "PathTraceMaterialFeatureDispatch.h"
#include "PathTraceMaterialFeatureOutputs.h"
#include "PathTraceMaterialFeatureRuntime.h"

#include "../../sys/DeviceManager.h"

extern DeviceManager* deviceManager;

struct RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess
{
    static RtPathTraceMaterialFeatureShaderState* TransmissionShaderState(
        RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState);
    static const RtPathTraceMaterialFeatureShaderState* TransmissionShaderState(
        const RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState);
};

struct RtPathTraceCleanRtxdiDiTransmissionPassAccess
{
    static void Init(
        RtPathTraceCleanRtxdiDiTransmissionPass& pass,
        bool cleanRouteRequested,
        int cleanView,
        bool producerRequested,
        bool debugOutputRequested,
        RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState);
    static bool CleanRouteRequested(const RtPathTraceCleanRtxdiDiTransmissionPass& pass);
    static int CleanView(const RtPathTraceCleanRtxdiDiTransmissionPass& pass);
    static bool ProducerRequested(const RtPathTraceCleanRtxdiDiTransmissionPass& pass);
    static bool DebugOutputRequested(const RtPathTraceCleanRtxdiDiTransmissionPass& pass);
    static RtPathTraceCleanRtxdiDiMaterialFeatureState* FeatureState(
        const RtPathTraceCleanRtxdiDiTransmissionPass& pass);
};

struct RtPathTraceCleanRtxdiDiMaterialFeatureState::Impl
{
    RtPathTraceMaterialFeatureShaderState transmissionShaderState;
};

struct RtPathTraceCleanRtxdiDiTransmissionPass::Impl
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

static RtPathTraceMaterialFeaturePassDesc BuildPathTraceCleanRtxdiDiTransmissionFeaturePassDesc(
    bool cleanRouteRequested,
    int cleanView,
    bool producerRequested,
    bool debugOutputRequested)
{
    RtPathTraceMaterialFeaturePassDesc desc;
    desc.kind = RtPathTraceMaterialFeaturePassKind::TransmissionProducer;
    desc.shaderTable = RtPathTraceMaterialFeatureShaderTable::None;
    desc.materialCapsConsumed = RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION;
    desc.materialPassSupport = RT_PATH_TRACE_MATERIAL_PASS_TRANSMISSION_PRODUCER;
    desc.resourceInputs =
        RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_TABLE |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_SIDECAR |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_RUNTIME_CONSTANTS;
    desc.resourceOutputs = RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT;
    desc.primaryOutputResource = RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT;

    const bool cleanTransmissionRoute = cleanRouteRequested && cleanView == 16;
    const bool debugOutput = cleanTransmissionRoute && debugOutputRequested;
    if (debugOutput)
    {
        desc.resourceOutputs |= RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR;
    }
    desc.enabled = cleanTransmissionRoute && (producerRequested || debugOutputRequested);
    desc.debugLabel = debugOutput ? "clean-rtxdi-di-transmission-producer-debug" : "clean-rtxdi-di-transmission-producer";
    return desc;
}

static RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiTransmissionFeatureRegistration(
    bool cleanRouteRequested,
    int cleanView,
    bool producerRequested,
    bool debugOutputRequested)
{
    RtPathTraceMaterialFeaturePassRegistration registration;
    registration.passDesc = BuildPathTraceCleanRtxdiDiTransmissionFeaturePassDesc(
        cleanRouteRequested,
        cleanView,
        producerRequested,
        debugOutputRequested);
    registration.shaderDesc = {
        "clean-room RTXDI DI transmission producer",
        "renderprogs2/dxil/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_transmission_producer.rt.bin",
        "renderprogs2/spirv/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_transmission_producer.rt.bin"
    };
    registration.validation = {
        "cmake --build --preset win64-pt-dev-release",
        "r_pathTracingCleanRtxdiDiView 16; r_pathTracingCleanRtxdiDiTransmissionProducer 1; r_pathTracingCleanRtxdiDiTransmissionDebugView 1",
        "glass-like material writes cyan to transmission output",
        "opaque material writes dark unsupported sentinel",
        "clean RTXDI DI primary view 16 unchanged",
        "RtPathTraceMaterialFeatureOutputDesc transmission u87 PathTraceCleanRtxdiDiTransmissionOutput",
        "PathTraceMaterialFeatureRuntimeInfo packed in PathTraceMaterialFeatureRuntimeConstants b88"
    };
    return registration;
}

static RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiTransmissionFeatureRegistration(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass)
{
    return BuildPathTraceCleanRtxdiDiTransmissionFeatureRegistration(
        RtPathTraceCleanRtxdiDiTransmissionPassAccess::CleanRouteRequested(pass),
        RtPathTraceCleanRtxdiDiTransmissionPassAccess::CleanView(pass),
        RtPathTraceCleanRtxdiDiTransmissionPassAccess::ProducerRequested(pass),
        RtPathTraceCleanRtxdiDiTransmissionPassAccess::DebugOutputRequested(pass));
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

static RtPathTraceMaterialFeaturePassRegistration PathTraceCleanRtxdiDiMaterialFeatureRegistrationForKind(
    const RtPathTraceCleanRtxdiDiTransmissionPass& transmissionPass,
    RtPathTraceMaterialFeaturePassKind kind)
{
    RtPathTraceMaterialFeaturePassRegistration registrations[RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_COUNT];
    const size_t registrationCount = BuildPathTraceCleanRtxdiDiMaterialFeatureRegistrations(
        transmissionPass,
        registrations,
        sizeof(registrations) / sizeof(registrations[0]));
    for (size_t i = 0; i < registrationCount && i < sizeof(registrations) / sizeof(registrations[0]); ++i)
    {
        if (registrations[i].passDesc.kind == kind)
        {
            return registrations[i];
        }
    }
    return RtPathTraceMaterialFeaturePassRegistration();
}

static RtPathTraceCleanRtxdiDiMaterialFeaturePipelineContext BuildPathTraceCleanRtxdiDiMaterialFeaturePipelineContext(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceCleanRtxdiDiPipelineContext& context)
{
    RtPathTraceCleanRtxdiDiMaterialFeatureState* featureState =
        RtPathTraceCleanRtxdiDiTransmissionPassAccess::FeatureState(pass);
    return {
        featureState
            ? RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::TransmissionShaderState(*featureState)
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

RtPathTraceCleanRtxdiDiTransmissionPass BuildPathTraceCleanRtxdiDiTransmissionPass(
    bool cleanRouteRequested,
    int cleanView,
    RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState)
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

RtPathTraceMaterialFeatureShaderState* RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::TransmissionShaderState(
    RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState)
{
    return featureState.m_impl
        ? &featureState.m_impl->transmissionShaderState
        : nullptr;
}

const RtPathTraceMaterialFeatureShaderState* RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::TransmissionShaderState(
    const RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState)
{
    return featureState.m_impl
        ? &featureState.m_impl->transmissionShaderState
        : nullptr;
}

void RtPathTraceCleanRtxdiDiTransmissionPassAccess::Init(
    RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    bool cleanRouteRequested,
    int cleanView,
    bool producerRequested,
    bool debugOutputRequested,
    RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState)
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

RtPathTraceCleanRtxdiDiMaterialFeatureState* RtPathTraceCleanRtxdiDiTransmissionPassAccess::FeatureState(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass)
{
    return pass.m_impl
        ? pass.m_impl->featureState
        : nullptr;
}

size_t BuildPathTraceCleanRtxdiDiMaterialFeatureRegistrations(
    const RtPathTraceCleanRtxdiDiTransmissionPass& transmissionPass,
    RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCapacity)
{
    static constexpr size_t registrationCount = RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_COUNT;
    if (registrations && registrationCapacity > 0)
    {
        registrations[0] = BuildPathTraceCleanRtxdiDiTransmissionFeatureRegistration(transmissionPass);
        if (registrationCapacity > 1)
        {
            registrations[1] = BuildPathTraceCleanRtxdiDiNoOpFeatureRegistration();
        }
    }
    return registrationCount;
}

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass)
{
    const RtPathTraceCleanRtxdiDiMaterialFeatureState* featureState =
        RtPathTraceCleanRtxdiDiTransmissionPassAccess::FeatureState(pass);
    const RtPathTraceMaterialFeatureShaderState* shaderState = featureState
        ? RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess::TransmissionShaderState(*featureState)
        : nullptr;
    if (!shaderState)
    {
        return RtPathTraceMaterialFeatureRuntimePass();
    }

    const RtPathTraceMaterialFeaturePassRegistration registration =
        PathTraceCleanRtxdiDiMaterialFeatureRegistrationForKind(pass, RtPathTraceMaterialFeaturePassKind::TransmissionProducer);
    return BuildPathTraceMaterialFeatureRuntimePass(registration.passDesc, shaderState);
}

void AddPathTraceCleanRtxdiDiTransmissionOutputLayoutBindings(nvrhi::BindingLayoutDesc& desc)
{
    AddPathTraceMaterialFeatureOutputLayoutBindings(desc, RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT);
}

void AddPathTraceCleanRtxdiDiTransmissionOutputBindings(
    nvrhi::BindingSetDesc& desc,
    const RtPathTraceFrameResources& frameResources)
{
    AddPathTraceMaterialFeatureOutputBindings(desc, frameResources, RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT);
}

bool PathTraceCleanRtxdiDiTransmissionOutputAvailable(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources)
{
    const RtPathTraceMaterialFeatureRuntimePass featurePass =
        BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(pass);
    return !featurePass.ready ||
        (PathTraceMaterialFeaturePrimaryOutputAvailable(featurePass, frameResources) &&
            PathTraceMaterialFeatureOutputAvailable(featurePass.desc, frameResources, RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR));
}

bool EnsurePathTraceCleanRtxdiDiTransmissionPassPipeline(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceCleanRtxdiDiPipelineContext& context)
{
    const RtPathTraceMaterialFeatureRuntimePass featurePass =
        BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(pass);
    const RtPathTraceMaterialFeaturePassRegistration registration =
        PathTraceCleanRtxdiDiMaterialFeatureRegistrationForKind(pass, RtPathTraceMaterialFeaturePassKind::TransmissionProducer);
    return EnsurePathTraceCleanRtxdiDiMaterialFeatureRuntimePassPipeline(
        featurePass,
        registration,
        BuildPathTraceCleanRtxdiDiMaterialFeaturePipelineContext(pass, context));
}

void SetPathTraceCleanRtxdiDiTransmissionOutputUnorderedAccess(
    nvrhi::ICommandList* commandList,
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources)
{
    const RtPathTraceMaterialFeatureRuntimePass featurePass =
        BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(pass);
    SetPathTraceMaterialFeaturePrimaryOutputState(
        commandList,
        featurePass,
        frameResources,
        nvrhi::ResourceStates::UnorderedAccess);
}

void ClearPathTraceCleanRtxdiDiTransmissionOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources)
{
    const RtPathTraceMaterialFeatureRuntimePass featurePass =
        BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(pass);
    ClearPathTraceMaterialFeaturePrimaryOutput(
        commandList,
        featurePass,
        frameResources,
        nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
}

void DispatchPathTraceCleanRtxdiDiTransmissionFeaturePass(
    nvrhi::ICommandList* commandList,
    const nvrhi::rt::State& baseState,
    const nvrhi::rt::DispatchRaysArguments& args,
    nvrhi::BufferHandle constantsBuffer,
    const void* baseConstants,
    size_t baseConstantsSize,
    nvrhi::BufferHandle runtimeConstantsBuffer,
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceFrameResources& frameResources,
    bool nsightGpuMarkers)
{
    const RtPathTraceMaterialFeatureRuntimePass featurePass =
        BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(pass);
    DispatchPathTraceMaterialFeaturePassWithRuntimeInfo(
        commandList,
        baseState,
        args,
        constantsBuffer,
        baseConstants,
        baseConstantsSize,
        runtimeConstantsBuffer,
        featurePass,
        frameResources,
        nsightGpuMarkers);
}

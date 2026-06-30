#include "precompiled.h"
#pragma hdrstop

// Material feature ray-tracing pipeline lifetime.
//
// Feature descriptors and shader selection live outside the core smoke resource
// module; this file only bridges the resolved feature request into NVRHI.

#include "PathTraceCleanRtxdiDiMaterialFeatures.h"
#include "PathTracePrimaryPass.h"

#include "../../sys/DeviceManager.h"

extern DeviceManager* deviceManager;

struct RtPathTraceMaterialFeaturePipelineContext
{
    RtPathTraceMaterialFeatureShaderTableState* shaderTableState = nullptr;
    bool smokeTestInitialized = false;
    nvrhi::BindingLayoutHandle smokeBindingLayout;
    nvrhi::BindingLayoutHandle cleanRtxdiDiBindingLayout;
    nvrhi::BindingLayoutHandle textureBindlessLayout;
};

static RtPathTraceMaterialFeaturePipelineContext BuildPathTraceMaterialFeaturePipelineContext(
    const RtPathTraceCleanRtxdiDiMaterialFeaturePipelineResources& resources)
{
    return {
        resources.featureState
            ? &PathTraceCleanRtxdiDiMaterialFeatureShaderTableState(*resources.featureState)
            : nullptr,
        resources.smokeTestInitialized,
        resources.smokeBindingLayout,
        resources.cleanRtxdiDiBindingLayout,
        resources.textureBindlessLayout
    };
}

static bool LoadPathTraceMaterialFeatureShaderLibrary(nvrhi::IDevice* device, const char* shaderPath, const char* label, nvrhi::ShaderLibraryHandle& shaderLibrary)
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

static bool CreatePathTraceMaterialFeatureRayTracingPipeline(
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

static bool InitPathTraceMaterialFeaturePipeline(
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceMaterialFeaturePipelineContext& context)
{
    if (!context.shaderTableState)
    {
        return false;
    }

    RtPathTraceMaterialFeatureShaderState* materialFeatureShaderState = PathTraceMaterialFeatureShaderStateForPass(
        passDesc,
        *context.shaderTableState);
    if (!materialFeatureShaderState)
    {
        return false;
    }
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
        passDesc,
        *context.shaderTableState,
        context.smokeBindingLayout,
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
        !LoadPathTraceMaterialFeatureShaderLibrary(device, pipelineRequest.shaderPath, pipelineRequest.shaderDesc.label, shaderState.shaderLibrary))
    {
        common->Printf("PathTracePrimaryPass: %s RT smoke shader unavailable; matching material-feature passes will be disabled\n", pipelineRequest.shaderDesc.label);
        return false;
    }

    if (!CreatePathTraceMaterialFeatureRayTracingPipeline(
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

    common->Printf("PathTracePrimaryPass: %s RT smoke pipeline initialized\n", pipelineRequest.shaderDesc.label);
    return true;
}

static bool EnsurePathTraceMaterialFeatureRuntimePassPipeline(
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceMaterialFeaturePipelineContext& context)
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
        InitPathTraceMaterialFeaturePipeline(pass.desc, context);
    }
    return static_cast<bool>(pass.shader->shaderTable);
}

bool EnsurePathTraceCleanRtxdiDiTransmissionPassPipeline(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass,
    const RtPathTraceCleanRtxdiDiMaterialFeaturePipelineResources& resources)
{
    return EnsurePathTraceMaterialFeatureRuntimePassPipeline(
        PathTraceCleanRtxdiDiTransmissionMaterialFeaturePass(pass),
        BuildPathTraceMaterialFeaturePipelineContext(resources));
}

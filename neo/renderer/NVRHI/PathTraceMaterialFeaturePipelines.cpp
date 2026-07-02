#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialFeaturePipelines.h"

bool LoadPathTraceMaterialFeatureShaderLibrary(
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

bool CreatePathTraceMaterialFeatureRayTracingPipeline(
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

bool InitPathTraceMaterialFeaturePipeline(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const RtPathTraceMaterialFeaturePipelineContext& context)
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

    if (!context.runtimeInitialized || !context.textureBindlessLayout)
    {
        return false;
    }

    if (!PathTraceMaterialFeatureBindingMetadataCoversPass(registration))
    {
        common->Printf("PathTracePrimaryPass: %s material-feature registration is missing binding metadata\n", registration.shaderDesc.label);
        return false;
    }

    if (!context.device)
    {
        return false;
    }

    const RtPathTraceMaterialFeaturePipelineRequest pipelineRequest = BuildPathTraceMaterialFeaturePipelineRequest(
        registration.shaderDesc,
        context.shaderState,
        context.bindingLayout,
        context.graphicsApi);
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
        !LoadPathTraceMaterialFeatureShaderLibrary(context.device, pipelineRequest.shaderPath.c_str(), pipelineRequest.shaderDesc.label, shaderState.shaderLibrary))
    {
        common->Printf("PathTracePrimaryPass: %s material-feature RT shader unavailable; matching material-feature passes will be disabled\n", pipelineRequest.shaderDesc.label);
        return false;
    }

    if (!CreatePathTraceMaterialFeatureRayTracingPipeline(
        context.device,
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

bool EnsurePathTraceMaterialFeatureRuntimePassPipeline(
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const RtPathTraceMaterialFeaturePipelineContext& context)
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
        InitPathTraceMaterialFeaturePipeline(registration, context);
    }
    return static_cast<bool>(pass.shader->shaderTable);
}

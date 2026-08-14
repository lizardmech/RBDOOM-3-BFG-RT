#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialFeaturePipelines.h"

namespace {

void ReportPathTraceMaterialFeaturePipelineAdmissionFailure(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const char* reason)
{
    static idStr lastFailure;
    idStr failure;
    failure.Format(
        "%s:%s",
        registration.shaderDesc.label ? registration.shaderDesc.label : "unknown",
        reason ? reason : "unknown");
    if (lastFailure.Icmp(failure) == 0)
    {
        return;
    }
    lastFailure = failure;
    common->Printf(
        "PathTracePrimaryPass: material-feature pipeline admission failed feature='%s' shader='%s' reason=%s\n",
        registration.passDesc.featureId ? registration.passDesc.featureId : "unknown",
        registration.shaderDesc.label ? registration.shaderDesc.label : "unknown",
        reason ? reason : "unknown");
}

}

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
        },
        {
            "SkinnedHitGroup",
            closestHit,
            anyHit,
            nullptr,
            nullptr,
            false
        },
        {
            "SkinnedShadowHitGroup",
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
    shaderTable->addHitGroup("SkinnedHitGroup");
    shaderTable->addHitGroup("SkinnedShadowHitGroup");
    return true;
}

bool InitPathTraceMaterialFeaturePipeline(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const RtPathTraceMaterialFeaturePipelineContext& context)
{
    if (!context.shaderState)
    {
        ReportPathTraceMaterialFeaturePipelineAdmissionFailure(
            registration,
            "missing-shader-state");
        return false;
    }

    RtPathTraceMaterialFeatureShaderState* materialFeatureShaderState = context.shaderState;
    if (materialFeatureShaderState->shaderTable)
    {
        return true;
    }

    if (!context.runtimeInitialized)
    {
        ReportPathTraceMaterialFeaturePipelineAdmissionFailure(
            registration,
            "runtime-not-initialized");
        return false;
    }
    if (!context.textureBindlessLayout)
    {
        ReportPathTraceMaterialFeaturePipelineAdmissionFailure(
            registration,
            "missing-bindless-layout");
        return false;
    }

    if (!PathTraceMaterialFeatureBindingMetadataCoversPass(registration))
    {
        common->Printf("PathTracePrimaryPass: %s material-feature registration is missing binding metadata\n", registration.shaderDesc.label);
        ReportPathTraceMaterialFeaturePipelineAdmissionFailure(
            registration,
            "binding-metadata-does-not-cover-pass");
        return false;
    }

    if (!context.device)
    {
        ReportPathTraceMaterialFeaturePipelineAdmissionFailure(
            registration,
            "missing-device");
        return false;
    }

    const RtPathTraceMaterialFeaturePipelineRequest pipelineRequest = BuildPathTraceMaterialFeaturePipelineRequest(
        registration.shaderDesc,
        context.shaderState,
        context.bindingLayout,
        context.graphicsApi);
    if (!pipelineRequest.shaderState)
    {
        ReportPathTraceMaterialFeaturePipelineAdmissionFailure(
            registration,
            "invalid-pipeline-request");
        return false;
    }

    if (!pipelineRequest.bindingLayout)
    {
        ReportPathTraceMaterialFeaturePipelineAdmissionFailure(
            registration,
            "missing-global-binding-layout");
        return false;
    }
    if (pipelineRequest.shaderPath.empty())
    {
        ReportPathTraceMaterialFeaturePipelineAdmissionFailure(
            registration,
            "empty-shader-path");
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
        "PathTracePrimaryPass: %s material-feature RT pipeline initialized; feature='%s' params='%s' validation build='%s' runtime='%s' supported='%s' unsupported='%s' baseline='%s' resources='%s' abi='%s'\n",
        pipelineRequest.shaderDesc.label,
        registration.passDesc.featureId,
        registration.parameterLayout.layoutName ? registration.parameterLayout.layoutName : "none",
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
        ReportPathTraceMaterialFeaturePipelineAdmissionFailure(
            registration,
            "runtime-pass-missing-shader-state");
        return false;
    }
    if (!pass.shader->shaderTable)
    {
        if (!InitPathTraceMaterialFeaturePipeline(registration, context))
        {
            ReportPathTraceMaterialFeaturePipelineAdmissionFailure(
                registration,
                "pipeline-initialization-failed");
        }
    }
    return static_cast<bool>(pass.shader->shaderTable);
}

bool EnsurePathTraceMaterialFeatureRegistrationListPipelines(
    const RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCount,
    RtPathTraceMaterialFeaturePipelineContextCallback contextCallback,
    const void* userContext,
    const char* ownerLabel)
{
    if (!registrations)
    {
        return true;
    }

    for (size_t i = 0; i < registrationCount; ++i)
    {
        const RtPathTraceMaterialFeaturePassRegistration& registration = registrations[i];
        if (!registration.passDesc.enabled)
        {
            continue;
        }

        if (!ValidatePathTraceMaterialFeatureRegistration(registration, ownerLabel))
        {
            return false;
        }

        const RtPathTraceMaterialFeaturePipelineContext context = contextCallback
            ? contextCallback(registration, userContext)
            : RtPathTraceMaterialFeaturePipelineContext();
        const RtPathTraceMaterialFeatureRuntimePass pipelinePass =
            BuildPathTraceMaterialFeatureRuntimePass(registration, context.shaderState);
        if (!EnsurePathTraceMaterialFeatureRuntimePassPipeline(pipelinePass, registration, context))
        {
            return false;
        }
    }
    return true;
}

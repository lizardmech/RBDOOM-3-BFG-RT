#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialFeatureOutputs.h"
#include "PathTraceMaterialFeatureRuntime.h"

static_assert(sizeof(RtPathTraceMaterialFeatureRuntimeConstants) == 48, "Material feature runtime constants must match shader b88 three-float4 ABI");

static bool PathTraceMaterialFeatureStringIsSet(const char* value)
{
    return value && value[0] != '\0';
}

static bool PathTraceMaterialFeatureValidationProofIsSet(const char* value)
{
    return PathTraceMaterialFeatureStringIsSet(value) && idStr::Cmp(value, "none") != 0;
}

static bool PathTraceMaterialFeatureResourceMaskIsSingleResource(uint32_t resource)
{
    return resource != RT_MATERIAL_FEATURE_RESOURCE_NONE && (resource & (resource - 1u)) == 0u;
}

static bool PathTraceMaterialFeatureOutputBindingMetadataMatchesDeclaredOutputs(
    const RtPathTraceMaterialFeaturePassRegistration& registration)
{
    for (uint32_t resource = 1u; resource != 0u; resource <<= 1u)
    {
        if ((registration.passDesc.resourceOutputs & resource) == 0u)
        {
            continue;
        }

        const RtPathTraceMaterialFeatureOutputDesc* outputDesc =
            FindPathTraceMaterialFeatureOutputDesc(resource);
        if (!outputDesc)
        {
            return false;
        }

        bool foundBinding = false;
        for (size_t i = 0; registration.bindingMetadata && i < registration.bindingMetadataCount; ++i)
        {
            const RtPathTraceMaterialFeatureBindingDesc& binding = registration.bindingMetadata[i];
            if (binding.resource != resource)
            {
                continue;
            }

            foundBinding = true;
            if (binding.kind != RtPathTraceMaterialFeatureBindingKind::TextureUav ||
                binding.slot != outputDesc->uavSlot)
            {
                return false;
            }
        }

        if (!foundBinding)
        {
            return false;
        }
    }
    return true;
}

static bool PathTraceMaterialFeatureValidationFail(
    const char* ownerLabel,
    const char* featureLabel,
    const char* reason)
{
    common->Printf(
        "PathTracePrimaryPass: %s material-feature registration '%s' failed descriptor validation: %s\n",
        PathTraceMaterialFeatureStringIsSet(ownerLabel) ? ownerLabel : "unknown",
        PathTraceMaterialFeatureStringIsSet(featureLabel) ? featureLabel : "unknown",
        reason);
    return false;
}

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassDesc& desc,
    const RtPathTraceMaterialFeatureShaderState* shaderState)
{
    RtPathTraceMaterialFeatureRuntimePass pass;
    pass.desc = desc;
    pass.shader = shaderState;
    pass.pipelineRequested = pass.shader && pass.desc.enabled;
    pass.ready = pass.shader && PathTraceMaterialFeaturePassIsReady(pass.desc);
    return pass;
}

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const RtPathTraceMaterialFeatureShaderState* shaderState)
{
    RtPathTraceMaterialFeatureRuntimePass pass =
        BuildPathTraceMaterialFeatureRuntimePass(registration.passDesc, shaderState);
    pass.runtimeInfoCallback = registration.runtimeInfoCallback;
    return pass;
}

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount)
{
    const uint32_t shaderStateIndex = registration.shaderStateIndex;
    return BuildPathTraceMaterialFeatureRuntimePass(
        registration,
        shaderStates &&
                shaderStateIndex != RT_PATH_TRACE_MATERIAL_FEATURE_SHADER_STATE_INVALID &&
                shaderStateIndex < shaderStateCount
            ? &shaderStates[shaderStateIndex]
            : nullptr);
}

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const RtPathTraceMaterialFeatureShaderTableState& shaderTableState)
{
    return BuildPathTraceMaterialFeatureRuntimePass(registration, shaderTableState.shaders.data(), shaderTableState.shaders.size());
}

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassDesc& desc,
    const RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount)
{
    const size_t shaderTableIndex = static_cast<size_t>(desc.shaderTable);
    return BuildPathTraceMaterialFeatureRuntimePass(
        desc,
        shaderStates && shaderTableIndex < shaderStateCount
            ? &shaderStates[shaderTableIndex]
            : nullptr);
}

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassDesc& desc,
    const RtPathTraceMaterialFeatureShaderTableState& shaderTableState)
{
    return BuildPathTraceMaterialFeatureRuntimePass(desc, shaderTableState.shaders.data(), shaderTableState.shaders.size());
}

RtPathTraceMaterialFeatureShaderState* PathTraceMaterialFeatureShaderStateForPass(
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount)
{
    const size_t shaderTableIndex = static_cast<size_t>(passDesc.shaderTable);
    if (!shaderStates || shaderTableIndex >= shaderStateCount)
    {
        return nullptr;
    }

    return &shaderStates[shaderTableIndex];
}

RtPathTraceMaterialFeatureShaderState* PathTraceMaterialFeatureShaderStateForPass(
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    RtPathTraceMaterialFeatureShaderTableState& shaderTableState)
{
    return PathTraceMaterialFeatureShaderStateForPass(passDesc, shaderTableState.shaders.data(), shaderTableState.shaders.size());
}

RtPathTraceMaterialFeatureShaderState* PathTraceMaterialFeatureShaderStateForRegistration(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount)
{
    if (!shaderStates ||
        registration.shaderStateIndex == RT_PATH_TRACE_MATERIAL_FEATURE_SHADER_STATE_INVALID ||
        registration.shaderStateIndex >= shaderStateCount)
    {
        return nullptr;
    }

    return &shaderStates[registration.shaderStateIndex];
}

RtPathTraceMaterialFeatureShaderState* PathTraceMaterialFeatureShaderStateForRegistration(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    RtPathTraceMaterialFeatureShaderTableState& shaderTableState)
{
    return PathTraceMaterialFeatureShaderStateForRegistration(
        registration,
        shaderTableState.shaders.data(),
        shaderTableState.shaders.size());
}

std::string PathTraceMaterialFeatureShaderPathForGraphicsApi(
    const RtPathTraceMaterialFeatureShaderDesc& shaderDesc,
    nvrhi::GraphicsAPI graphicsApi)
{
    if (!shaderDesc.shaderBlobPath)
    {
        return std::string();
    }

    switch (graphicsApi)
    {
    case nvrhi::GraphicsAPI::D3D12:
        return std::string("renderprogs2/dxil/") + shaderDesc.shaderBlobPath;
    case nvrhi::GraphicsAPI::VULKAN:
        return std::string("renderprogs2/spirv/") + shaderDesc.shaderBlobPath;
    default:
        return std::string();
    }
}

RtPathTraceMaterialFeaturePipelineRequest BuildPathTraceMaterialFeaturePipelineRequest(
    const RtPathTraceMaterialFeatureShaderDesc& shaderDesc,
    RtPathTraceMaterialFeatureShaderState* shaderState,
    nvrhi::BindingLayoutHandle bindingLayout,
    nvrhi::GraphicsAPI graphicsApi)
{
    RtPathTraceMaterialFeaturePipelineRequest request;

    request.shaderState = shaderState;
    if (!request.shaderState)
    {
        return request;
    }

    request.shaderDesc = shaderDesc;
    if (!request.shaderDesc.shaderBlobPath)
    {
        request.shaderState = nullptr;
        return request;
    }

    request.bindingLayout = bindingLayout;
    request.shaderPath = PathTraceMaterialFeatureShaderPathForGraphicsApi(request.shaderDesc, graphicsApi);
    if (request.shaderPath.empty())
    {
        request.shaderState = nullptr;
    }
    return request;
}

bool ValidatePathTraceMaterialFeatureRegistration(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const char* ownerLabel)
{
    const RtPathTraceMaterialFeaturePassDesc& desc = registration.passDesc;
    const char* featureLabel = PathTraceMaterialFeatureStringIsSet(desc.featureId)
        ? desc.featureId
        : registration.shaderDesc.label;

    if (!desc.enabled)
    {
        return true;
    }

    if (desc.kind == RtPathTraceMaterialFeaturePassKind::Disabled)
    {
        return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "enabled pass uses Disabled kind");
    }

    if (!PathTraceMaterialFeatureStringIsSet(desc.featureId) || idStr::Cmp(desc.featureId, "disabled") == 0)
    {
        return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "missing feature id");
    }

    if (!PathTraceMaterialFeatureStringIsSet(desc.debugLabel) || idStr::Cmp(desc.debugLabel, "disabled") == 0)
    {
        return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "missing debug label");
    }

    if (registration.shaderStateIndex == RT_PATH_TRACE_MATERIAL_FEATURE_SHADER_STATE_INVALID ||
        registration.shaderStateIndex >= RT_PATH_TRACE_MATERIAL_FEATURE_SHADER_STATE_CAPACITY)
    {
        return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "invalid shader state index");
    }

    if (!PathTraceMaterialFeatureStringIsSet(registration.shaderDesc.label))
    {
        return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "missing shader label");
    }

    if (!PathTraceMaterialFeatureStringIsSet(registration.shaderDesc.shaderBlobPath))
    {
        return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "missing shader blob path");
    }

    if (desc.primaryOutputResource != RT_MATERIAL_FEATURE_RESOURCE_NONE)
    {
        if (!PathTraceMaterialFeatureResourceMaskIsSingleResource(desc.primaryOutputResource))
        {
            return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "primary output is not a single resource");
        }

        if ((desc.resourceOutputs & desc.primaryOutputResource) != desc.primaryOutputResource)
        {
            return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "primary output is not declared as an output resource");
        }
    }

    if (!PathTraceMaterialFeatureOutputResourcesDeclared(desc.resourceOutputs))
    {
        return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "output resource has no frame-resource mapping");
    }

    if (!PathTraceMaterialFeatureOutputBindingMetadataMatchesDeclaredOutputs(registration))
    {
        return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "output binding metadata does not match output declaration");
    }

    if (!PathTraceMaterialFeatureBindingMetadataCoversPass(registration))
    {
        return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "binding metadata does not cover declared inputs and outputs");
    }

    if (!PathTraceMaterialFeatureValidationProofIsSet(registration.validation.buildProof) ||
        !PathTraceMaterialFeatureValidationProofIsSet(registration.validation.runtimeRoute) ||
        !PathTraceMaterialFeatureValidationProofIsSet(registration.validation.resourceBindingProof) ||
        !PathTraceMaterialFeatureValidationProofIsSet(registration.validation.cpuShaderAbiProof))
    {
        return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "missing validation proof strings");
    }

    return true;
}

RtPathTraceMaterialFeatureRuntimeInfo BuildPathTraceMaterialFeatureRuntimeInfo(const RtPathTraceMaterialFeaturePassDesc& desc, bool passReady)
{
    RtPathTraceMaterialFeatureRuntimeInfo typedInfo;
    typedInfo.writesOutputColor = PathTraceMaterialFeaturePassWritesAnyOutput(desc, RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR) ? 1.0f : 0.0f;
    typedInfo.ready = passReady ? 1.0f : 0.0f;
    return typedInfo;
}

RtPathTraceMaterialFeatureRuntimeInfo BuildPathTraceMaterialFeatureRuntimeInfo(const RtPathTraceMaterialFeatureRuntimePass& pass)
{
    RtPathTraceMaterialFeatureRuntimeInfo typedInfo =
        BuildPathTraceMaterialFeatureRuntimeInfo(pass.desc, pass.ready);
    if (pass.runtimeInfoCallback)
    {
        pass.runtimeInfoCallback(typedInfo, pass.desc);
    }
    return typedInfo;
}

RtPathTraceMaterialFeatureRuntimeConstants BuildPathTraceMaterialFeatureRuntimeConstants(
    const RtPathTraceMaterialFeatureRuntimeInfo& typedInfo)
{
    RtPathTraceMaterialFeatureRuntimeConstants constants;
    constants.runtimeInfo[0] = typedInfo.writesOutputColor;
    constants.runtimeInfo[1] = typedInfo.ready;
    constants.runtimeInfo[2] = typedInfo.debugMode;
    constants.runtimeInfo[3] = typedInfo.frameIndex;
    for (size_t i = 0; i < 4; ++i)
    {
        constants.featureParams0[i] = typedInfo.featureParams0[i];
        constants.featureParams1[i] = typedInfo.featureParams1[i];
    }
    return constants;
}

RtPathTraceMaterialFeatureRuntimeConstants BuildPathTraceMaterialFeatureRuntimeConstants(
    const RtPathTraceMaterialFeaturePassDesc& desc,
    bool passReady)
{
    return BuildPathTraceMaterialFeatureRuntimeConstants(
        BuildPathTraceMaterialFeatureRuntimeInfo(desc, passReady));
}

RtPathTraceMaterialFeatureRuntimeConstants BuildPathTraceMaterialFeatureRuntimeConstants(
    const RtPathTraceMaterialFeatureRuntimePass& pass)
{
    return BuildPathTraceMaterialFeatureRuntimeConstants(BuildPathTraceMaterialFeatureRuntimeInfo(pass));
}

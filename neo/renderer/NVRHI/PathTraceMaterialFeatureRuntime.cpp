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

static const char* PathTraceMaterialFeatureResourceName(uint32_t resource)
{
    switch (resource)
    {
    case RT_MATERIAL_FEATURE_RESOURCE_TLAS: return "tlas";
    case RT_MATERIAL_FEATURE_RESOURCE_SCENE_GEOMETRY: return "scene-geometry";
    case RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_TABLE: return "material-table";
    case RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE: return "current-primary-surface";
    case RT_MATERIAL_FEATURE_RESOURCE_PREVIOUS_PRIMARY_SURFACE: return "previous-primary-surface";
    case RT_MATERIAL_FEATURE_RESOURCE_CURRENT_DIRECT_RESERVOIR: return "current-direct-reservoir";
    case RT_MATERIAL_FEATURE_RESOURCE_TEMPORAL_DIRECT_RESERVOIR: return "temporal-direct-reservoir";
    case RT_MATERIAL_FEATURE_RESOURCE_SPATIAL_DIRECT_RESERVOIR: return "spatial-direct-reservoir";
    case RT_MATERIAL_FEATURE_RESOURCE_GI_RESERVOIR: return "gi-reservoir";
    case RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR: return "output-color";
    case RT_MATERIAL_FEATURE_RESOURCE_MOTION_VECTORS: return "motion-vectors";
    case RT_MATERIAL_FEATURE_RESOURCE_RR_GUIDES: return "rr-guides";
    case RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT: return "transmission-output";
    case RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_SIDECAR: return "material-feature-sidecar";
    case RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_RUNTIME_CONSTANTS: return "material-feature-runtime-constants";
    case RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_PARAMETERS: return "material-feature-parameters";
    case RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR_SOURCE: return "output-color-source";
    case RT_MATERIAL_FEATURE_RESOURCE_RR_GUIDE_SPECULAR_ALBEDO: return "rr-guide-specular-albedo";
    case RT_MATERIAL_FEATURE_RESOURCE_RR_INPUT_COLOR: return "rr-input-color";
    case RT_MATERIAL_FEATURE_RESOURCE_GLASS_GUIDE_CANDIDATE0: return "glass-guide-candidate0";
    case RT_MATERIAL_FEATURE_RESOURCE_GLASS_GUIDE_CANDIDATE1: return "glass-guide-candidate1";
    case RT_MATERIAL_FEATURE_RESOURCE_GLASS_GUIDE_CANDIDATE2: return "glass-guide-candidate2";
    default: return "unknown";
    }
}

static void AppendPathTraceMaterialFeatureResourceMask(idStr& out, uint32_t resources)
{
    if (resources == RT_MATERIAL_FEATURE_RESOURCE_NONE)
    {
        out.Append("none");
        return;
    }

    for (uint32_t resource = 1u; resource != 0u; resource <<= 1u)
    {
        if ((resources & resource) == 0u)
        {
            continue;
        }
        if (out.Length() > 0)
        {
            out.Append("|");
        }
        out.Append(PathTraceMaterialFeatureResourceName(resource));
    }
}

static bool PathTraceMaterialFeatureShaderBlobPathIsPackagedRelative(const char* shaderBlobPath)
{
    if (!PathTraceMaterialFeatureStringIsSet(shaderBlobPath))
    {
        return false;
    }

    const size_t pathLength = strlen(shaderBlobPath);
    if (shaderBlobPath[0] == '/' ||
        shaderBlobPath[0] == '\\' ||
        (pathLength > 1 && shaderBlobPath[1] == ':'))
    {
        return false;
    }

    if (idStr::FindText(shaderBlobPath, "renderprogs2/", false) >= 0 ||
        idStr::FindText(shaderBlobPath, "renderprogs2\\", false) >= 0)
    {
        return false;
    }

    return pathLength > 4 && idStr::Icmp(shaderBlobPath + pathLength - 4, ".bin") == 0;
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

static bool PathTraceMaterialFeatureParameterLayoutIsValid(
    const RtPathTraceMaterialFeatureParameterLayoutDesc& layout)
{
    if (!PathTraceMaterialFeatureStringIsSet(layout.layoutName) ||
        !layout.lanes ||
        layout.laneCount == 0)
    {
        return false;
    }

    for (size_t i = 0; i < layout.laneCount; ++i)
    {
        const RtPathTraceMaterialFeatureParameterLaneDesc& lane = layout.lanes[i];
        if (!PathTraceMaterialFeatureStringIsSet(lane.name) ||
            lane.component >= RT_PATH_TRACE_MATERIAL_FEATURE_PARAMETER_LANE_COUNT)
        {
            return false;
        }
        if (lane.vector != RtPathTraceMaterialFeatureParameterVector::Params0 &&
            lane.vector != RtPathTraceMaterialFeatureParameterVector::Params1)
        {
            return false;
        }
    }

    return true;
}

static bool PathTraceMaterialFeatureRegistryContractIsSet(
    const RtPathTraceMaterialFeatureRegistryContractDesc& contract)
{
    return PathTraceMaterialFeatureStringIsSet(contract.featureId);
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

static bool PathTraceMaterialFeatureValidationFailResourceMask(
    const char* ownerLabel,
    const char* featureLabel,
    const char* reason,
    uint32_t resources)
{
    idStr resourceNames;
    AppendPathTraceMaterialFeatureResourceMask(resourceNames, resources);
    common->Printf(
        "PathTracePrimaryPass: %s material-feature registration '%s' failed descriptor validation: %s resources=0x%08x(%s)\n",
        PathTraceMaterialFeatureStringIsSet(ownerLabel) ? ownerLabel : "unknown",
        PathTraceMaterialFeatureStringIsSet(featureLabel) ? featureLabel : "unknown",
        reason,
        resources,
        resourceNames.c_str());
    return false;
}

static bool PathTraceMaterialFeatureRegistryContractMatchesRegistration(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    const char* ownerLabel,
    const char* featureLabel)
{
    const RtPathTraceMaterialFeaturePassDesc& desc = registration.passDesc;
    const RtPathTraceMaterialFeatureRegistryContractDesc& contract = registration.registryContract;
    if (!PathTraceMaterialFeatureRegistryContractIsSet(contract))
    {
        return true;
    }

    if (idStr::Cmp(desc.featureId, contract.featureId) != 0)
    {
        return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "feature id drifted from registry contract");
    }

    if (desc.kind != contract.kind)
    {
        return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "pass kind drifted from registry contract");
    }

    if (desc.materialCapsConsumed != contract.materialCapsConsumed)
    {
        return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "material caps drifted from registry contract");
    }

    if (desc.materialPassSupport != contract.materialPassSupport)
    {
        return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "material pass support drifted from registry contract");
    }

    if ((desc.resourceInputs & contract.requiredResourceInputs) != contract.requiredResourceInputs)
    {
        return PathTraceMaterialFeatureValidationFailResourceMask(
            ownerLabel,
            featureLabel,
            "required registry inputs are missing",
            contract.requiredResourceInputs & ~desc.resourceInputs);
    }

    const uint32_t undeclaredInputs = desc.resourceInputs & ~contract.allowedResourceInputs;
    if (undeclaredInputs != 0u)
    {
        return PathTraceMaterialFeatureValidationFailResourceMask(
            ownerLabel,
            featureLabel,
            "input resource is not declared by registry contract",
            undeclaredInputs);
    }

    const uint32_t undeclaredOutputs = desc.resourceOutputs & ~contract.allowedResourceOutputs;
    if (undeclaredOutputs != 0u)
    {
        return PathTraceMaterialFeatureValidationFailResourceMask(
            ownerLabel,
            featureLabel,
            "output resource is not declared by registry contract",
            undeclaredOutputs);
    }

    return true;
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

size_t BuildPathTraceReadyMaterialFeatureRuntimePasses(
    const RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCount,
    const RtPathTraceMaterialFeatureShaderTableState* shaderTableState,
    RtPathTraceMaterialFeatureRuntimePass* runtimePasses,
    RtPathTraceMaterialFeaturePassRegistration* registrationsOut,
    size_t passCapacity)
{
    size_t passCount = 0;
    for (size_t i = 0; registrations && i < registrationCount; ++i)
    {
        const RtPathTraceMaterialFeaturePassRegistration& registration = registrations[i];
        RtPathTraceMaterialFeatureRuntimePass runtimePass = shaderTableState
            ? BuildPathTraceMaterialFeatureRuntimePass(registration, *shaderTableState)
            : BuildPathTraceMaterialFeatureRuntimePass(registration, static_cast<const RtPathTraceMaterialFeatureShaderState*>(nullptr));
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

    if (!PathTraceMaterialFeatureRegistryContractMatchesRegistration(registration, ownerLabel, featureLabel))
    {
        return false;
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

    if (!PathTraceMaterialFeatureShaderBlobPathIsPackagedRelative(registration.shaderDesc.shaderBlobPath))
    {
        return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "shader blob path must be a packaged relative .bin path");
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

    const bool parameterLayoutRequired =
        PathTraceMaterialFeaturePassReadsAnyInput(desc, RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_PARAMETERS) ||
        registration.runtimeInfoCallback != nullptr;
    if (parameterLayoutRequired &&
        !PathTraceMaterialFeatureParameterLayoutIsValid(registration.parameterLayout))
    {
        return PathTraceMaterialFeatureValidationFail(ownerLabel, featureLabel, "missing or invalid parameter layout descriptor");
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
    constants.runtimeInfo[RT_PATH_TRACE_MATERIAL_FEATURE_RUNTIME_WRITES_OUTPUT_COLOR] = typedInfo.writesOutputColor;
    constants.runtimeInfo[RT_PATH_TRACE_MATERIAL_FEATURE_RUNTIME_READY] = typedInfo.ready;
    constants.runtimeInfo[RT_PATH_TRACE_MATERIAL_FEATURE_RUNTIME_DEBUG_MODE] = typedInfo.debugMode;
    constants.runtimeInfo[RT_PATH_TRACE_MATERIAL_FEATURE_RUNTIME_FRAME_INDEX] = typedInfo.frameIndex;
    for (size_t i = 0; i < RT_PATH_TRACE_MATERIAL_FEATURE_PARAMETER_LANE_COUNT; ++i)
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

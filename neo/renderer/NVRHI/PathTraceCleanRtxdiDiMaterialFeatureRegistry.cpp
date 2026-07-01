#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiMaterialFeatureRegistry.h"
#include "PathTraceCleanRtxdiDiGlassFeature.h"
#include "PathTraceCleanRtxdiDiTransmissionFeature.h"

namespace {

using RtPathTraceCleanRtxdiDiFeatureRuntimeRegistrationBuilder =
    RtPathTraceMaterialFeaturePassRegistration (*)(const RtPathTraceCleanRtxdiDiMaterialFeatureRegistryContext& context);
using RtPathTraceCleanRtxdiDiFeatureLayoutRegistrationBuilder =
    RtPathTraceMaterialFeaturePassRegistration (*)();

struct RtPathTraceCleanRtxdiDiMaterialFeatureRegistryEntry
{
    const char* debugName = "unknown";
    RtPathTraceCleanRtxdiDiFeatureRuntimeRegistrationBuilder buildRuntimeRegistration = nullptr;
    RtPathTraceCleanRtxdiDiFeatureLayoutRegistrationBuilder buildLayoutRegistration = nullptr;
};

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiTransmissionRuntimeRegistration(
    const RtPathTraceCleanRtxdiDiMaterialFeatureRegistryContext& context)
{
    return BuildPathTraceCleanRtxdiDiTransmissionFeatureRegistration(
        context.cleanRouteRequested,
        context.cleanView);
}

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiGlassRuntimeRegistration(
    const RtPathTraceCleanRtxdiDiMaterialFeatureRegistryContext& context)
{
    return BuildPathTraceCleanRtxdiDiGlassFeatureRegistration(
        context.cleanRouteRequested,
        context.cleanView);
}

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiNoOpFeatureRegistration()
{
    RtPathTraceMaterialFeaturePassRegistration registration;
    registration.passDesc.featureId = "clean-rtxdi-di-noop";
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

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiNoOpRuntimeRegistration(
    const RtPathTraceCleanRtxdiDiMaterialFeatureRegistryContext&)
{
    return BuildPathTraceCleanRtxdiDiNoOpFeatureRegistration();
}

static const RtPathTraceCleanRtxdiDiMaterialFeatureRegistryEntry kCleanRtxdiDiMaterialFeatureRegistry[] = {
    {
        "clean-rtxdi-di-transmission",
        BuildPathTraceCleanRtxdiDiTransmissionRuntimeRegistration,
        BuildPathTraceCleanRtxdiDiTransmissionFeatureLayoutRegistration
    },
    {
        "clean-rtxdi-di-glass",
        BuildPathTraceCleanRtxdiDiGlassRuntimeRegistration,
        BuildPathTraceCleanRtxdiDiGlassFeatureLayoutRegistration
    },
    {
        "clean-rtxdi-di-noop",
        BuildPathTraceCleanRtxdiDiNoOpRuntimeRegistration,
        BuildPathTraceCleanRtxdiDiNoOpFeatureRegistration
    }
};

static_assert(
    sizeof(kCleanRtxdiDiMaterialFeatureRegistry) / sizeof(kCleanRtxdiDiMaterialFeatureRegistry[0]) <=
        RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_CAPACITY,
    "Clean RTXDI DI material feature registry exceeds fixed traversal capacity");

void AssignPathTraceCleanRtxdiDiMaterialFeatureRegistryShaderStateIndex(
    RtPathTraceMaterialFeaturePassRegistration& registration,
    size_t registryIndex)
{
    registration.shaderStateIndex =
        registryIndex < RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_CAPACITY
            ? static_cast<uint32_t>(registryIndex)
            : RT_PATH_TRACE_MATERIAL_FEATURE_SHADER_STATE_INVALID;
}

void ResolvePathTraceCleanRtxdiDiSharedDebugOutput(
    RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCount,
    uint32_t outputResource)
{
    size_t ownerIndex = registrationCount;
    uint32_t ownerPriority = 0u;
    for (size_t i = 0; registrations && i < registrationCount; ++i)
    {
        const RtPathTraceMaterialFeaturePassDesc& passDesc = registrations[i].passDesc;
        if (!PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, outputResource))
        {
            continue;
        }
        if (ownerIndex == registrationCount || passDesc.sharedOutputPriority > ownerPriority)
        {
            ownerIndex = i;
            ownerPriority = passDesc.sharedOutputPriority;
        }
    }

    for (size_t i = 0; registrations && i < registrationCount; ++i)
    {
        if (i == ownerIndex)
        {
            continue;
        }

        RtPathTraceMaterialFeaturePassDesc& passDesc = registrations[i].passDesc;
        if (PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, outputResource))
        {
            passDesc.resourceOutputs &= ~outputResource;
            if ((passDesc.primaryOutputResource & outputResource) != 0u)
            {
                passDesc.primaryOutputResource = RT_MATERIAL_FEATURE_RESOURCE_NONE;
            }
        }
    }
}

}

size_t PathTraceCleanRtxdiDiMaterialFeatureRegistryCount()
{
    return sizeof(kCleanRtxdiDiMaterialFeatureRegistry) / sizeof(kCleanRtxdiDiMaterialFeatureRegistry[0]);
}

size_t BuildPathTraceCleanRtxdiDiMaterialFeatureRegistryRegistrations(
    const RtPathTraceCleanRtxdiDiMaterialFeatureRegistryContext& context,
    RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCapacity)
{
    const size_t registryCount = PathTraceCleanRtxdiDiMaterialFeatureRegistryCount();
    for (size_t i = 0; registrations && i < registryCount && i < registrationCapacity; ++i)
    {
        const RtPathTraceCleanRtxdiDiMaterialFeatureRegistryEntry& entry = kCleanRtxdiDiMaterialFeatureRegistry[i];
        if (entry.buildRuntimeRegistration)
        {
            registrations[i] = entry.buildRuntimeRegistration(context);
            AssignPathTraceCleanRtxdiDiMaterialFeatureRegistryShaderStateIndex(registrations[i], i);
        }
    }
    ResolvePathTraceCleanRtxdiDiSharedDebugOutput(
        registrations,
        registryCount < registrationCapacity ? registryCount : registrationCapacity,
        RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR);
    return registryCount;
}

size_t BuildPathTraceCleanRtxdiDiMaterialFeatureRegistryLayoutRegistrations(
    RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCapacity)
{
    const size_t registryCount = PathTraceCleanRtxdiDiMaterialFeatureRegistryCount();
    for (size_t i = 0; registrations && i < registryCount && i < registrationCapacity; ++i)
    {
        const RtPathTraceCleanRtxdiDiMaterialFeatureRegistryEntry& entry = kCleanRtxdiDiMaterialFeatureRegistry[i];
        if (entry.buildLayoutRegistration)
        {
            registrations[i] = entry.buildLayoutRegistration();
            AssignPathTraceCleanRtxdiDiMaterialFeatureRegistryShaderStateIndex(registrations[i], i);
        }
    }
    return registryCount;
}

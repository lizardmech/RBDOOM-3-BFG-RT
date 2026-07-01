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

void ResolvePathTraceCleanRtxdiDiSharedDebugOutputs(
    RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCount)
{
    uint32_t claimedDebugOutputs = RT_MATERIAL_FEATURE_RESOURCE_NONE;
    for (size_t i = 0; registrations && i < registrationCount; ++i)
    {
        RtPathTraceMaterialFeaturePassDesc& passDesc = registrations[i].passDesc;
        const uint32_t duplicateDebugOutputs =
            passDesc.resourceOutputs & claimedDebugOutputs & RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR;
        if (duplicateDebugOutputs != RT_MATERIAL_FEATURE_RESOURCE_NONE)
        {
            passDesc.resourceOutputs &= ~duplicateDebugOutputs;
            if ((passDesc.primaryOutputResource & duplicateDebugOutputs) != 0u)
            {
                passDesc.primaryOutputResource = RT_MATERIAL_FEATURE_RESOURCE_NONE;
            }
        }
        claimedDebugOutputs |= passDesc.resourceOutputs & RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR;
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
    ResolvePathTraceCleanRtxdiDiSharedDebugOutputs(
        registrations,
        registryCount < registrationCapacity ? registryCount : registrationCapacity);
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

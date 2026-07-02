#pragma once

// Generic host registry helpers for material-feature pass descriptors.
//
// A render path owns the concrete feature list, but the mechanics for building
// registrations, assigning shader-state slots, and arbitrating shared outputs
// are common across material-feature registries.

#include "PathTraceMaterialFeaturePasses.h"

#include <cstddef>

using RtPathTraceMaterialFeatureRuntimeRegistrationBuilder =
    RtPathTraceMaterialFeaturePassRegistration (*)(const void* context);
using RtPathTraceMaterialFeatureLayoutRegistrationBuilder =
    RtPathTraceMaterialFeaturePassRegistration (*)();

inline bool PathTraceMaterialFeatureRegistryStringIsUnsetOrNone(const char* value)
{
    return !value || value[0] == '\0' ||
        (value[0] == 'n' && value[1] == 'o' && value[2] == 'n' && value[3] == 'e' && value[4] == '\0');
}

inline bool PathTraceMaterialFeatureRegistryStringEquals(const char* value, const char* expected)
{
    if (!value || !expected)
    {
        return value == expected;
    }

    while (*value && *expected && *value == *expected)
    {
        ++value;
        ++expected;
    }
    return *value == '\0' && *expected == '\0';
}

inline bool PathTraceMaterialFeatureRegistryStringIsSet(const char* value)
{
    return !PathTraceMaterialFeatureRegistryStringIsUnsetOrNone(value);
}

inline bool PathTraceMaterialFeatureRegistryResourceMaskHasOnlyAllowed(uint32_t value, uint32_t allowed)
{
    return (value & ~allowed) == 0u;
}

struct RtPathTraceMaterialFeatureRegistryEntry
{
    const char* featureId = "unknown";
    RtPathTraceMaterialFeatureRuntimeRegistrationBuilder buildRuntimeRegistration = nullptr;
    RtPathTraceMaterialFeatureLayoutRegistrationBuilder buildLayoutRegistration = nullptr;
    RtPathTraceMaterialFeatureShaderDesc shaderDesc;
    RtPathTraceMaterialFeatureParameterLayoutDesc parameterLayout;
    RtPathTraceMaterialFeatureValidationDesc validation;
    RtPathTraceMaterialFeaturePassKind kind = RtPathTraceMaterialFeaturePassKind::Disabled;
    uint32_t materialCapsConsumed = 0;
    uint32_t materialPassSupport = 0;
    uint32_t requiredResourceInputs = RT_MATERIAL_FEATURE_RESOURCE_NONE;
    uint32_t allowedResourceInputs = RT_MATERIAL_FEATURE_RESOURCE_NONE;
    uint32_t allowedResourceOutputs = RT_MATERIAL_FEATURE_RESOURCE_NONE;
    uint32_t sharedOutputArbitrationResources = RT_MATERIAL_FEATURE_RESOURCE_NONE;
};

inline const char* ValidatePathTraceMaterialFeatureRegistryEntry(
    const RtPathTraceMaterialFeatureRegistryEntry& entry,
    bool requireRuntimeBuilder,
    bool requireLayoutBuilder)
{
    if (!PathTraceMaterialFeatureRegistryStringIsSet(entry.featureId) ||
        PathTraceMaterialFeatureRegistryStringEquals(entry.featureId, "unknown"))
    {
        return "feature id is not declared";
    }
    if (requireRuntimeBuilder && !entry.buildRuntimeRegistration)
    {
        return "runtime registration builder is missing";
    }
    if (requireLayoutBuilder && !entry.buildLayoutRegistration)
    {
        return "layout registration builder is missing";
    }

    if (entry.kind == RtPathTraceMaterialFeaturePassKind::Disabled)
    {
        return nullptr;
    }

    if (!PathTraceMaterialFeatureRegistryStringIsSet(entry.shaderDesc.label))
    {
        return "shader label is not declared";
    }
    if (!PathTraceMaterialFeatureRegistryStringIsSet(entry.shaderDesc.shaderBlobPath))
    {
        return "shader blob path is not declared";
    }
    if (entry.materialCapsConsumed == 0u)
    {
        return "material capability mask is empty";
    }
    if (entry.materialPassSupport == 0u)
    {
        return "material pass support mask is empty";
    }
    if (!PathTraceMaterialFeatureRegistryResourceMaskHasOnlyAllowed(
            entry.requiredResourceInputs,
            entry.allowedResourceInputs))
    {
        return "required inputs are not a subset of allowed inputs";
    }
    if (entry.allowedResourceOutputs == RT_MATERIAL_FEATURE_RESOURCE_NONE)
    {
        return "allowed output resource mask is empty";
    }
    if (!PathTraceMaterialFeatureRegistryResourceMaskHasOnlyAllowed(
            entry.sharedOutputArbitrationResources,
            entry.allowedResourceOutputs))
    {
        return "shared-output arbitration mask is not a subset of allowed outputs";
    }
    if (!PathTraceMaterialFeatureRegistryStringIsSet(entry.validation.buildProof) ||
        !PathTraceMaterialFeatureRegistryStringIsSet(entry.validation.runtimeRoute) ||
        !PathTraceMaterialFeatureRegistryStringIsSet(entry.validation.resourceBindingProof) ||
        !PathTraceMaterialFeatureRegistryStringIsSet(entry.validation.cpuShaderAbiProof))
    {
        return "validation proof strings are incomplete";
    }
    return nullptr;
}

inline size_t PathTraceMaterialFeatureRegistryWritableCount(size_t entryCount, size_t registrationCapacity)
{
    return entryCount < registrationCapacity ? entryCount : registrationCapacity;
}

inline void ApplyPathTraceMaterialFeatureRegistryEntry(
    const RtPathTraceMaterialFeatureRegistryEntry& entry,
    size_t registryIndex,
    RtPathTraceMaterialFeaturePassRegistration& registration)
{
    if ((!registration.passDesc.featureId || registration.passDesc.featureId[0] == '\0') && entry.featureId)
    {
        registration.passDesc.featureId = entry.featureId;
    }
    if ((!registration.passDesc.debugLabel || registration.passDesc.debugLabel[0] == '\0') && entry.featureId)
    {
        registration.passDesc.debugLabel = entry.featureId;
    }

    registration.shaderStateIndex =
        registryIndex < RT_PATH_TRACE_MATERIAL_FEATURE_SHADER_STATE_CAPACITY
            ? static_cast<uint32_t>(registryIndex)
            : RT_PATH_TRACE_MATERIAL_FEATURE_SHADER_STATE_INVALID;
    registration.registryContract = {
        entry.featureId,
        entry.kind,
        entry.materialCapsConsumed,
        entry.materialPassSupport,
        entry.requiredResourceInputs,
        entry.allowedResourceInputs,
        entry.allowedResourceOutputs
    };
    if (!registration.shaderDesc.shaderBlobPath && entry.shaderDesc.shaderBlobPath)
    {
        registration.shaderDesc = entry.shaderDesc;
    }
    if (!registration.parameterLayout.layoutName && entry.parameterLayout.layoutName)
    {
        registration.parameterLayout = entry.parameterLayout;
    }
    if (PathTraceMaterialFeatureRegistryStringIsUnsetOrNone(registration.validation.buildProof) &&
        !PathTraceMaterialFeatureRegistryStringIsUnsetOrNone(entry.validation.buildProof))
    {
        registration.validation = entry.validation;
    }
}

inline uint32_t BuildPathTraceMaterialFeatureRegistrySharedOutputMask(
    const RtPathTraceMaterialFeatureRegistryEntry* entries,
    size_t entryCount)
{
    uint32_t sharedOutputMask = RT_MATERIAL_FEATURE_RESOURCE_NONE;
    for (size_t i = 0; entries && i < entryCount; ++i)
    {
        sharedOutputMask |= entries[i].sharedOutputArbitrationResources;
    }
    return sharedOutputMask;
}

inline void ResolvePathTraceMaterialFeatureRegistrySharedOutputs(
    RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCount,
    const RtPathTraceMaterialFeatureRegistryEntry* entries,
    size_t entryCount)
{
    const uint32_t sharedOutputMask = BuildPathTraceMaterialFeatureRegistrySharedOutputMask(entries, entryCount);
    for (uint32_t resource = 1u; resource != 0u; resource <<= 1u)
    {
        if ((sharedOutputMask & resource) != 0u)
        {
            ResolvePathTraceMaterialFeatureSharedOutputOwner(registrations, registrationCount, resource);
        }
    }
}

inline size_t BuildPathTraceMaterialFeatureRegistryRuntimeRegistrations(
    const void* context,
    const RtPathTraceMaterialFeatureRegistryEntry* entries,
    size_t entryCount,
    RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCapacity)
{
    const size_t writableCount = PathTraceMaterialFeatureRegistryWritableCount(entryCount, registrationCapacity);
    for (size_t i = 0; entries && registrations && i < writableCount; ++i)
    {
        const RtPathTraceMaterialFeatureRegistryEntry& entry = entries[i];
        if (entry.buildRuntimeRegistration)
        {
            registrations[i] = entry.buildRuntimeRegistration(context);
            ApplyPathTraceMaterialFeatureRegistryEntry(entry, i, registrations[i]);
        }
    }
    ResolvePathTraceMaterialFeatureRegistrySharedOutputs(registrations, writableCount, entries, entryCount);
    return entryCount;
}

inline size_t BuildPathTraceMaterialFeatureRegistryLayoutRegistrations(
    const RtPathTraceMaterialFeatureRegistryEntry* entries,
    size_t entryCount,
    RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCapacity)
{
    const size_t writableCount = PathTraceMaterialFeatureRegistryWritableCount(entryCount, registrationCapacity);
    for (size_t i = 0; entries && registrations && i < writableCount; ++i)
    {
        const RtPathTraceMaterialFeatureRegistryEntry& entry = entries[i];
        if (entry.buildLayoutRegistration)
        {
            registrations[i] = entry.buildLayoutRegistration();
            ApplyPathTraceMaterialFeatureRegistryEntry(entry, i, registrations[i]);
        }
    }
    return entryCount;
}

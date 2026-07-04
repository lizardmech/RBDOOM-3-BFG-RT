#pragma once

// Shared clean RTXDI DI material-feature descriptor helpers.
// Concrete feature modules own runtime cvar decisions and feature-local outputs;
// registry entries own shader identity, invariant pass fields, and contracts.

#include "PathTraceMaterialFeatureBindings.h"
#include "PathTraceMaterialFeatureParameters.h"

#include <cstddef>

#define RT_CLEAN_RTXDI_DI_MATERIAL_FEATURE_RT_BLOB(stem) "builtin/pathtracing/cleanroom_rtxdi/" stem ".rt.bin"

enum class RtPathTraceCleanRtxdiDiObjectGlassBindingSet
{
    ComposedGlass,
    TransmissionProducer
};

inline RtPathTraceMaterialFeatureBindingDesc PathTraceCleanRtxdiDiCanonicalBinding(uint32_t resource)
{
    RtPathTraceMaterialFeatureBindingDesc binding;
    BuildPathTraceMaterialFeatureCanonicalBindingDesc(resource, binding);
    return binding;
}

inline RtPathTraceMaterialFeatureBindingDesc PathTraceCleanRtxdiDiCurrentPrimarySurfaceBinding()
{
    return PathTraceCleanRtxdiDiCanonicalBinding(RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE);
}

inline RtPathTraceMaterialFeatureBindingDesc PathTraceCleanRtxdiDiMaterialTableBinding()
{
    return PathTraceCleanRtxdiDiCanonicalBinding(RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_TABLE);
}

inline RtPathTraceMaterialFeatureBindingDesc PathTraceCleanRtxdiDiMaterialFeaturesBinding()
{
    return PathTraceCleanRtxdiDiCanonicalBinding(RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_SIDECAR);
}

inline RtPathTraceMaterialFeatureBindingDesc PathTraceCleanRtxdiDiMaterialFeatureParametersBinding()
{
    return PathTraceCleanRtxdiDiCanonicalBinding(RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_PARAMETERS);
}

inline RtPathTraceMaterialFeatureBindingDesc PathTraceCleanRtxdiDiMaterialFeatureRuntimeConstantsBinding()
{
    return PathTraceCleanRtxdiDiCanonicalBinding(RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_RUNTIME_CONSTANTS);
}

inline RtPathTraceMaterialFeatureBindingDesc PathTraceCleanRtxdiDiOutputColorSourceBinding()
{
    return PathTraceCleanRtxdiDiCanonicalBinding(RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR_SOURCE);
}

inline RtPathTraceMaterialFeatureBindingDesc PathTraceCleanRtxdiDiOutputColorBinding()
{
    return PathTraceCleanRtxdiDiCanonicalBinding(RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR);
}

inline RtPathTraceMaterialFeatureBindingDesc PathTraceCleanRtxdiDiTransmissionOutputBinding()
{
    return PathTraceCleanRtxdiDiCanonicalBinding(RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT);
}

inline RtPathTraceMaterialFeatureBindingDesc PathTraceCleanRtxdiDiRrInputColorBinding()
{
    return PathTraceCleanRtxdiDiCanonicalBinding(RT_MATERIAL_FEATURE_RESOURCE_RR_INPUT_COLOR);
}

inline bool PathTraceCleanRtxdiDiMaterialFeatureRouteEnabled(bool cleanRouteRequested, int cleanView)
{
    return cleanRouteRequested && cleanView == 16;
}

inline uint32_t PathTraceCleanRtxdiDiMaterialFeatureSurfaceInputs()
{
    return
        RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_TABLE |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_SIDECAR |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_PARAMETERS |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_RUNTIME_CONSTANTS;
}

inline uint32_t PathTraceCleanRtxdiDiComposedOutputResources()
{
    return
        RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR |
        RT_MATERIAL_FEATURE_RESOURCE_RR_INPUT_COLOR;
}

inline uint32_t PathTraceCleanRtxdiDiComposedInputResources()
{
    return
        PathTraceCleanRtxdiDiMaterialFeatureSurfaceInputs() |
        RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR_SOURCE;
}

inline uint32_t PathTraceCleanRtxdiDiTransmissionProducerOutputResources()
{
    return
        RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT |
        PathTraceCleanRtxdiDiComposedOutputResources();
}

inline void PathTraceCleanRtxdiDiEnableComposedOutput(
    RtPathTraceMaterialFeaturePassDesc& desc,
    bool primaryOutput)
{
    desc.resourceInputs |= RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR_SOURCE;
    desc.resourceOutputs |= PathTraceCleanRtxdiDiComposedOutputResources();
    if (primaryOutput)
    {
        desc.primaryOutputResource = RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR;
    }
}

inline const RtPathTraceMaterialFeatureBindingDesc* PathTraceCleanRtxdiDiObjectGlassBindingMetadata(
    RtPathTraceCleanRtxdiDiObjectGlassBindingSet bindingSet,
    size_t& bindingCount)
{
    static const RtPathTraceMaterialFeatureBindingDesc kComposedGlassBindings[] = {
        PathTraceCleanRtxdiDiCurrentPrimarySurfaceBinding(),
        PathTraceCleanRtxdiDiMaterialTableBinding(),
        PathTraceCleanRtxdiDiMaterialFeaturesBinding(),
        PathTraceCleanRtxdiDiMaterialFeatureParametersBinding(),
        PathTraceCleanRtxdiDiMaterialFeatureRuntimeConstantsBinding(),
        PathTraceCleanRtxdiDiOutputColorSourceBinding(),
        PathTraceCleanRtxdiDiRrInputColorBinding(),
        PathTraceCleanRtxdiDiOutputColorBinding()
    };
    static const RtPathTraceMaterialFeatureBindingDesc kTransmissionProducerBindings[] = {
        PathTraceCleanRtxdiDiCurrentPrimarySurfaceBinding(),
        PathTraceCleanRtxdiDiMaterialTableBinding(),
        PathTraceCleanRtxdiDiMaterialFeaturesBinding(),
        PathTraceCleanRtxdiDiMaterialFeatureParametersBinding(),
        PathTraceCleanRtxdiDiMaterialFeatureRuntimeConstantsBinding(),
        PathTraceCleanRtxdiDiOutputColorSourceBinding(),
        PathTraceCleanRtxdiDiTransmissionOutputBinding(),
        PathTraceCleanRtxdiDiRrInputColorBinding(),
        PathTraceCleanRtxdiDiOutputColorBinding()
    };

    if (bindingSet == RtPathTraceCleanRtxdiDiObjectGlassBindingSet::TransmissionProducer)
    {
        bindingCount = sizeof(kTransmissionProducerBindings) / sizeof(kTransmissionProducerBindings[0]);
        return kTransmissionProducerBindings;
    }

    bindingCount = sizeof(kComposedGlassBindings) / sizeof(kComposedGlassBindings[0]);
    return kComposedGlassBindings;
}

inline void AttachPathTraceCleanRtxdiDiObjectGlassBindingMetadata(
    RtPathTraceMaterialFeaturePassRegistration& registration,
    RtPathTraceCleanRtxdiDiObjectGlassBindingSet bindingSet)
{
    size_t bindingMetadataCount = 0;
    registration.bindingMetadata =
        PathTraceCleanRtxdiDiObjectGlassBindingMetadata(bindingSet, bindingMetadataCount);
    registration.bindingMetadataCount = bindingMetadataCount;
}

inline RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiObjectGlassRegistration(
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    RtPathTraceCleanRtxdiDiObjectGlassBindingSet bindingSet,
    RtPathTraceMaterialFeatureRuntimeInfoCallback runtimeInfoCallback)
{
    RtPathTraceMaterialFeaturePassRegistration registration;
    registration.passDesc = passDesc;
    AttachPathTraceCleanRtxdiDiObjectGlassBindingMetadata(registration, bindingSet);
    registration.runtimeInfoCallback = runtimeInfoCallback;
    return registration;
}

inline void FillPathTraceCleanRtxdiDiObjectGlassRuntimeInfo(
    RtPathTraceMaterialFeatureRuntimeInfo& runtimeInfo,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    int debugOutputMode)
{
    const bool debugOutputEnabled =
        debugOutputMode != 0 &&
        PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR);
    runtimeInfo.debugMode = debugOutputEnabled ? static_cast<float>(debugOutputMode) : 0.0f;
    CopyPathTraceMaterialFeatureParametersToRuntimeInfo(
        runtimeInfo,
        BuildPathTraceObjectGlassMaterialFeatureParameters());
}

#pragma once

// Shared clean RTXDI DI material-feature descriptor helpers.
// Concrete feature modules own runtime cvar decisions and feature-local outputs;
// registry entries own shader identity, invariant pass fields, and contracts.

#include "PathTraceMaterialFeatureBindings.h"
#include "PathTraceMaterialFeatureParameters.h"

#define RT_CLEAN_RTXDI_DI_MATERIAL_FEATURE_RT_BLOB(stem) "builtin/pathtracing/cleanroom_rtxdi/" stem ".rt.bin"

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

inline RtPathTraceMaterialFeatureBindingDesc PathTraceCleanRtxdiDiRrGuideSpecularAlbedoBinding()
{
    return PathTraceCleanRtxdiDiCanonicalBinding(RT_MATERIAL_FEATURE_RESOURCE_RR_GUIDE_SPECULAR_ALBEDO);
}

inline RtPathTraceMaterialFeatureBindingDesc PathTraceCleanRtxdiDiRrInputColorBinding()
{
    return PathTraceCleanRtxdiDiCanonicalBinding(RT_MATERIAL_FEATURE_RESOURCE_RR_INPUT_COLOR);
}

inline RtPathTraceMaterialFeatureBindingDesc PathTraceCleanRtxdiDiGlassGuideCandidate0Binding()
{
    return PathTraceCleanRtxdiDiCanonicalBinding(RT_MATERIAL_FEATURE_RESOURCE_GLASS_GUIDE_CANDIDATE0);
}

inline RtPathTraceMaterialFeatureBindingDesc PathTraceCleanRtxdiDiGlassGuideCandidate1Binding()
{
    return PathTraceCleanRtxdiDiCanonicalBinding(RT_MATERIAL_FEATURE_RESOURCE_GLASS_GUIDE_CANDIDATE1);
}

inline RtPathTraceMaterialFeatureBindingDesc PathTraceCleanRtxdiDiGlassGuideCandidate2Binding()
{
    return PathTraceCleanRtxdiDiCanonicalBinding(RT_MATERIAL_FEATURE_RESOURCE_GLASS_GUIDE_CANDIDATE2);
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

inline void FillPathTraceCleanRtxdiDiObjectGlassRuntimeInfo(
    RtPathTraceMaterialFeatureRuntimeInfo& runtimeInfo,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    bool debugOutputRequested)
{
    const bool debugOutputEnabled =
        debugOutputRequested &&
        PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR);
    runtimeInfo.debugMode = debugOutputEnabled ? 1.0f : 0.0f;
    CopyPathTraceMaterialFeatureParametersToRuntimeInfo(
        runtimeInfo,
        BuildPathTraceObjectGlassMaterialFeatureParameters());
}

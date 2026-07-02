#pragma once

// Shared clean RTXDI DI material-feature descriptor helpers.
// Concrete feature modules still own their outputs, shader identity, and cvars.

#include "PathTraceMaterialFeatureParameters.h"

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


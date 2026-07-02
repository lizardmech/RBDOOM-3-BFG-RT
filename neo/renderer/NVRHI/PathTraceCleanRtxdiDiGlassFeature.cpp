#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiGlassFeature.h"
#include "PathTraceCVars.h"
#include "PathTraceCleanRtxdiDiMaterialFeatureCommon.h"

static RtPathTraceMaterialFeaturePassDesc BuildPathTraceCleanRtxdiDiGlassFeaturePassDesc(
    bool cleanRouteRequested,
    int cleanView,
    bool shaderRequested,
    bool debugOutputRequested,
    bool guideCandidateOutputRequested)
{
    RtPathTraceMaterialFeaturePassDesc desc;

    const bool cleanGlassRoute = PathTraceCleanRtxdiDiMaterialFeatureRouteEnabled(cleanRouteRequested, cleanView);
    const bool outputRequested = cleanGlassRoute && (shaderRequested || debugOutputRequested);
    const bool debugOutput = cleanGlassRoute && debugOutputRequested;
    if (outputRequested)
    {
        desc.resourceInputs |= RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR_SOURCE;
        desc.resourceOutputs = RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR;
        desc.primaryOutputResource = RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR;
    }
    if (cleanGlassRoute && shaderRequested)
    {
        desc.resourceOutputs |=
            RT_MATERIAL_FEATURE_RESOURCE_RR_GUIDE_SPECULAR_ALBEDO |
            RT_MATERIAL_FEATURE_RESOURCE_RR_INPUT_COLOR;
    }
    if (cleanGlassRoute && guideCandidateOutputRequested)
    {
        desc.resourceOutputs |=
            RT_MATERIAL_FEATURE_RESOURCE_GLASS_GUIDE_CANDIDATE0 |
            RT_MATERIAL_FEATURE_RESOURCE_GLASS_GUIDE_CANDIDATE1 |
            RT_MATERIAL_FEATURE_RESOURCE_GLASS_GUIDE_CANDIDATE2;
    }
    desc.enabled = cleanGlassRoute && (shaderRequested || debugOutputRequested);
    desc.debugLabel = debugOutput ? "clean-rtxdi-di-glass-debug" : "clean-rtxdi-di-glass";
    return desc;
}

static const RtPathTraceMaterialFeatureBindingDesc kCleanRtxdiDiGlassBindings[] = {
    PathTraceCleanRtxdiDiCurrentPrimarySurfaceBinding(),
    PathTraceCleanRtxdiDiMaterialTableBinding(),
    PathTraceCleanRtxdiDiMaterialFeaturesBinding(),
    PathTraceCleanRtxdiDiMaterialFeatureParametersBinding(),
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeConstantsBinding(),
    PathTraceCleanRtxdiDiOutputColorSourceBinding(),
    PathTraceCleanRtxdiDiRrGuideSpecularAlbedoBinding(),
    PathTraceCleanRtxdiDiRrInputColorBinding(),
    PathTraceCleanRtxdiDiGlassGuideCandidate0Binding(),
    PathTraceCleanRtxdiDiGlassGuideCandidate1Binding(),
    PathTraceCleanRtxdiDiGlassGuideCandidate2Binding(),
    PathTraceCleanRtxdiDiOutputColorBinding()
};

static void FillPathTraceCleanRtxdiDiGlassRuntimeInfo(
    RtPathTraceMaterialFeatureRuntimeInfo& runtimeInfo,
    const RtPathTraceMaterialFeaturePassDesc& passDesc)
{
    const int guideDebugMode = idMath::ClampInt(0, 3, r_pathTracingCleanRtxdiDiGlassGuideDebugView.GetInteger());
    FillPathTraceCleanRtxdiDiObjectGlassRuntimeInfo(
        runtimeInfo,
        passDesc,
        r_pathTracingCleanRtxdiDiGlassDebugView.GetInteger() != 0);
    if (guideDebugMode > 0 &&
        PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR))
    {
        runtimeInfo.debugMode = static_cast<float>(1 + guideDebugMode);
    }
}

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiGlassFeatureRegistration(
    bool cleanRouteRequested,
    int cleanView)
{
    const bool shaderRequested = r_pathTracingCleanRtxdiDiGlassShader.GetInteger() != 0;
    const bool debugOutputRequested = r_pathTracingCleanRtxdiDiGlassDebugView.GetInteger() != 0;
    const bool guideDebugRequested = r_pathTracingCleanRtxdiDiGlassGuideDebugView.GetInteger() != 0;

    RtPathTraceMaterialFeaturePassRegistration registration;
    registration.passDesc = BuildPathTraceCleanRtxdiDiGlassFeaturePassDesc(
        cleanRouteRequested,
        cleanView,
        shaderRequested || guideDebugRequested,
        debugOutputRequested || guideDebugRequested,
        guideDebugRequested);
    registration.bindingMetadata = kCleanRtxdiDiGlassBindings;
    registration.bindingMetadataCount = sizeof(kCleanRtxdiDiGlassBindings) / sizeof(kCleanRtxdiDiGlassBindings[0]);
    registration.runtimeInfoCallback = FillPathTraceCleanRtxdiDiGlassRuntimeInfo;
    return registration;
}

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiGlassFeatureLayoutRegistration()
{
    RtPathTraceMaterialFeaturePassRegistration registration =
        BuildPathTraceCleanRtxdiDiGlassFeatureRegistration(true, 16);
    registration.passDesc = BuildPathTraceCleanRtxdiDiGlassFeaturePassDesc(
        true,
        16,
        true,
        true,
        true);
    return registration;
}

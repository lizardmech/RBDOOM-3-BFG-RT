#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiGlassFeature.h"
#include "PathTraceCVars.h"
#include "PathTraceCleanRtxdiDiMaterialFeatureCommon.h"

static RtPathTraceMaterialFeaturePassDesc BuildPathTraceCleanRtxdiDiGlassFeaturePassDesc(
    bool cleanRouteRequested,
    int cleanView,
    bool shaderRequested,
    bool debugOutputRequested)
{
    RtPathTraceMaterialFeaturePassDesc desc;

    const bool cleanGlassRoute = PathTraceCleanRtxdiDiMaterialFeatureRouteEnabled(cleanRouteRequested, cleanView);
    const bool outputRequested = cleanGlassRoute && (shaderRequested || debugOutputRequested);
    const bool debugOutput = cleanGlassRoute && debugOutputRequested;
    if (outputRequested)
    {
        PathTraceCleanRtxdiDiEnableComposedOutput(desc, true);
    }
    desc.enabled = cleanGlassRoute && (shaderRequested || debugOutputRequested);
    desc.debugLabel = debugOutput ? "clean-rtxdi-di-glass-debug" : "clean-rtxdi-di-glass";
    return desc;
}

static void FillPathTraceCleanRtxdiDiGlassRuntimeInfo(
    RtPathTraceMaterialFeatureRuntimeInfo& runtimeInfo,
    const RtPathTraceMaterialFeaturePassDesc& passDesc)
{
    FillPathTraceCleanRtxdiDiObjectGlassRuntimeInfo(
        runtimeInfo,
        passDesc,
        r_pathTracingCleanRtxdiDiGlassDebugView.GetInteger() != 0);
}

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiGlassFeatureRegistration(
    bool cleanRouteRequested,
    int cleanView)
{
    const bool shaderRequested = r_pathTracingCleanRtxdiDiGlassShader.GetInteger() != 0;
    const bool debugOutputRequested = r_pathTracingCleanRtxdiDiGlassDebugView.GetInteger() != 0;

    return BuildPathTraceCleanRtxdiDiObjectGlassRegistration(
        BuildPathTraceCleanRtxdiDiGlassFeaturePassDesc(
            cleanRouteRequested,
            cleanView,
            shaderRequested,
            debugOutputRequested),
        RtPathTraceCleanRtxdiDiObjectGlassBindingSet::ComposedGlass,
        FillPathTraceCleanRtxdiDiGlassRuntimeInfo);
}

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiGlassFeatureLayoutRegistration()
{
    return BuildPathTraceCleanRtxdiDiObjectGlassRegistration(
        BuildPathTraceCleanRtxdiDiGlassFeaturePassDesc(
            true,
            16,
            true,
            true),
        RtPathTraceCleanRtxdiDiObjectGlassBindingSet::ComposedGlass,
        FillPathTraceCleanRtxdiDiGlassRuntimeInfo);
}

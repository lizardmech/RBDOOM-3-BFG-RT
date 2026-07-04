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
    bool transmissionComposeRequested)
{
    RtPathTraceMaterialFeaturePassDesc desc;

    const bool cleanGlassRoute = PathTraceCleanRtxdiDiMaterialFeatureRouteEnabled(cleanRouteRequested, cleanView);
    // The older screen-space glass beauty pass has been removed from normal
    // output. Normal compose now only applies the PSR transmission sidecar.
    const bool beautyOutput = false;
    const bool sidecarComposeOutput = cleanGlassRoute && transmissionComposeRequested;
    const bool outputRequested = cleanGlassRoute && (beautyOutput || debugOutputRequested || sidecarComposeOutput);
    const bool debugOutput = cleanGlassRoute && debugOutputRequested;
    if (outputRequested)
    {
        PathTraceCleanRtxdiDiEnableComposedOutput(desc, true);
        desc.resourceInputs |= RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_SIDECAR;
    }
    desc.enabled = cleanGlassRoute && (beautyOutput || debugOutputRequested || sidecarComposeOutput);
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
        r_pathTracingCleanRtxdiDiGlassDebugView.GetInteger(),
        idMath::ClampFloat(0.0f, 8.0f, r_pathTracingCleanRtxdiDiGlassReflectionBoost.GetFloat()),
        idMath::ClampFloat(0.0f, 1.0f, r_pathTracingCleanRtxdiDiGlassTransmissionFloor.GetFloat()));
}

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiGlassFeatureRegistration(
    bool cleanRouteRequested,
    int cleanView)
{
    const bool shaderRequested = r_pathTracingCleanRtxdiDiGlassShader.GetInteger() != 0;
    const bool debugOutputRequested = r_pathTracingCleanRtxdiDiGlassDebugView.GetInteger() != 0;
    const bool transmissionComposeRequested =
        r_pathTracingCleanRtxdiDiTransmissionProducer.GetInteger() != 0 &&
        r_pathTracingCleanRtxdiDiTransmissionCompose.GetInteger() != 0;

    return BuildPathTraceCleanRtxdiDiObjectGlassRegistration(
        BuildPathTraceCleanRtxdiDiGlassFeaturePassDesc(
            cleanRouteRequested,
            cleanView,
            shaderRequested,
            debugOutputRequested,
            transmissionComposeRequested),
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
            true,
            false),
        RtPathTraceCleanRtxdiDiObjectGlassBindingSet::ComposedGlass,
        FillPathTraceCleanRtxdiDiGlassRuntimeInfo);
}

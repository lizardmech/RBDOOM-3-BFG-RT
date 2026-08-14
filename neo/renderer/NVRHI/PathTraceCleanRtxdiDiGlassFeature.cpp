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
    // Shader-only output is an opaque overlay test path. Normal transparent
    // compose still comes from the PSR transmission sidecar.
    const bool beautyOutput = cleanGlassRoute && shaderRequested;
    const bool sidecarComposeOutput = cleanGlassRoute && transmissionComposeRequested;
    const bool outputRequested = cleanGlassRoute && (beautyOutput || debugOutputRequested || sidecarComposeOutput);
    const bool debugOutput = cleanGlassRoute && debugOutputRequested;
    if (outputRequested)
    {
        PathTraceCleanRtxdiDiEnableComposedOutput(desc, true);
        desc.resourceInputs |=
            RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_SIDECAR |
            RT_MATERIAL_FEATURE_RESOURCE_REFLECTION_SIDECAR |
            RT_MATERIAL_FEATURE_RESOURCE_GLASS_DISTORTION_SIDECAR;
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
    const bool transmissionProducerEnabled =
        r_pathTracingCleanRtxdiDiTransmissionProducer.GetInteger() != 0;
    const bool transmissionComposeEnabled =
        transmissionProducerEnabled &&
        r_pathTracingCleanRtxdiDiTransmissionCompose.GetInteger() != 0;
    runtimeInfo.frameIndex = transmissionComposeEnabled ? 2.0f : (transmissionProducerEnabled ? 1.0f : 0.0f);
}

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiGlassFeatureRegistration(
    bool cleanRouteRequested,
    int cleanView)
{
    const bool transmissionDebugRequested =
        r_pathTracingCleanRtxdiDiTransmissionDebugView.GetInteger() != 0;
    const bool producerOwnedOutputRequested = transmissionDebugRequested;
    const bool shaderRequested =
        !producerOwnedOutputRequested &&
        r_pathTracingCleanRtxdiDiGlassShader.GetInteger() != 0;
    const bool debugOutputRequested =
        !producerOwnedOutputRequested &&
        r_pathTracingCleanRtxdiDiGlassDebugView.GetInteger() != 0;
    const bool transmissionComposeRequested =
        !producerOwnedOutputRequested &&
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

#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiTransmissionFeature.h"
#include "PathTraceCVars.h"
#include "PathTraceCleanRtxdiDiMaterialFeatureCommon.h"

static RtPathTraceMaterialFeaturePassDesc BuildPathTraceCleanRtxdiDiTransmissionFeaturePassDesc(
    bool cleanRouteRequested,
    int cleanView,
    bool producerRequested,
    bool debugOutputRequested,
    bool transmissionComposeRequested)
{
    RtPathTraceMaterialFeaturePassDesc desc;
    desc.resourceOutputs =
        RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT |
        RT_MATERIAL_FEATURE_RESOURCE_REFLECTION_OUTPUT |
        RT_MATERIAL_FEATURE_RESOURCE_GLASS_DISTORTION_OUTPUT |
        RT_MATERIAL_FEATURE_RESOURCE_RR_GUIDE_SPECULAR_ALBEDO;
    desc.primaryOutputResource = RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT;

    const bool cleanTransmissionRoute = PathTraceCleanRtxdiDiMaterialFeatureRouteEnabled(cleanRouteRequested, cleanView);
    const bool debugOutput = cleanTransmissionRoute && debugOutputRequested;
    const bool composedOutput = cleanTransmissionRoute && (debugOutputRequested || transmissionComposeRequested);
    if (composedOutput)
    {
        desc.resourceInputs |= RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR_SOURCE;
        desc.resourceOutputs |= RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR;
        PathTraceCleanRtxdiDiEnableComposedOutput(desc, false);
    }
    desc.enabled = cleanTransmissionRoute && (producerRequested || debugOutputRequested || transmissionComposeRequested);
    desc.debugLabel = debugOutput ? "clean-rtxdi-di-transmission-producer-debug" : "clean-rtxdi-di-transmission-producer";
    return desc;
}

static void FillPathTraceCleanRtxdiDiTransmissionRuntimeInfo(
    RtPathTraceMaterialFeatureRuntimeInfo& runtimeInfo,
    const RtPathTraceMaterialFeaturePassDesc& passDesc)
{
    FillPathTraceCleanRtxdiDiObjectGlassRuntimeInfo(
        runtimeInfo,
        passDesc,
        r_pathTracingCleanRtxdiDiTransmissionDebugView.GetInteger(),
        idMath::ClampFloat(0.0f, 8.0f, r_pathTracingCleanRtxdiDiGlassReflectionBoost.GetFloat()),
        idMath::ClampFloat(0.0f, 1.0f, r_pathTracingCleanRtxdiDiGlassTransmissionFloor.GetFloat()));
}

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiTransmissionFeatureRegistration(
    bool cleanRouteRequested,
    int cleanView)
{
    const bool producerRequested = r_pathTracingCleanRtxdiDiTransmissionProducer.GetInteger() != 0;
    const bool debugOutputRequested = r_pathTracingCleanRtxdiDiTransmissionDebugView.GetInteger() != 0;
    const bool transmissionComposeRequested =
        producerRequested &&
        r_pathTracingCleanRtxdiDiTransmissionCompose.GetInteger() != 0;

    return BuildPathTraceCleanRtxdiDiObjectGlassRegistration(
        BuildPathTraceCleanRtxdiDiTransmissionFeaturePassDesc(
            cleanRouteRequested,
            cleanView,
            producerRequested,
            debugOutputRequested,
            transmissionComposeRequested),
        RtPathTraceCleanRtxdiDiObjectGlassBindingSet::TransmissionProducer,
        FillPathTraceCleanRtxdiDiTransmissionRuntimeInfo);
}

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiTransmissionFeatureLayoutRegistration()
{
    return BuildPathTraceCleanRtxdiDiObjectGlassRegistration(
        BuildPathTraceCleanRtxdiDiTransmissionFeaturePassDesc(
            true,
            16,
            true,
            true,
            true),
        RtPathTraceCleanRtxdiDiObjectGlassBindingSet::TransmissionProducer,
        FillPathTraceCleanRtxdiDiTransmissionRuntimeInfo);
}

#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiTransmissionFeature.h"
#include "PathTraceCVars.h"
#include "PathTraceCleanRtxdiDiMaterialFeatureCommon.h"

static RtPathTraceMaterialFeaturePassDesc BuildPathTraceCleanRtxdiDiTransmissionFeaturePassDesc(
    bool cleanRouteRequested,
    int cleanView,
    bool producerRequested,
    bool composeOutputRequested,
    bool debugOutputRequested)
{
    RtPathTraceMaterialFeaturePassDesc desc;
    desc.resourceOutputs = RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT;
    desc.primaryOutputResource = RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT;

    const bool cleanTransmissionRoute = PathTraceCleanRtxdiDiMaterialFeatureRouteEnabled(cleanRouteRequested, cleanView);
    const bool debugOutput = cleanTransmissionRoute && debugOutputRequested;
    const bool composeOutput = cleanTransmissionRoute && producerRequested && composeOutputRequested;
    if (debugOutput || composeOutput)
    {
        desc.resourceInputs |= RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR_SOURCE;
        desc.resourceOutputs |= RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR;
    }
    if (composeOutput)
    {
        desc.resourceOutputs |= RT_MATERIAL_FEATURE_RESOURCE_RR_INPUT_COLOR;
    }
    desc.enabled = cleanTransmissionRoute && (producerRequested || debugOutputRequested);
    desc.debugLabel = debugOutput ? "clean-rtxdi-di-transmission-producer-debug" : "clean-rtxdi-di-transmission-producer";
    return desc;
}

static const RtPathTraceMaterialFeatureBindingDesc kCleanRtxdiDiTransmissionBindings[] = {
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

static void FillPathTraceCleanRtxdiDiTransmissionRuntimeInfo(
    RtPathTraceMaterialFeatureRuntimeInfo& runtimeInfo,
    const RtPathTraceMaterialFeaturePassDesc& passDesc)
{
    FillPathTraceCleanRtxdiDiObjectGlassRuntimeInfo(
        runtimeInfo,
        passDesc,
        r_pathTracingCleanRtxdiDiTransmissionDebugView.GetInteger() != 0);
}

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiTransmissionFeatureRegistration(
    bool cleanRouteRequested,
    int cleanView)
{
    const bool producerRequested = r_pathTracingCleanRtxdiDiTransmissionProducer.GetInteger() != 0;
    const bool composeOutputRequested = r_pathTracingCleanRtxdiDiTransmissionCompose.GetInteger() != 0;
    const bool debugOutputRequested = r_pathTracingCleanRtxdiDiTransmissionDebugView.GetInteger() != 0;

    RtPathTraceMaterialFeaturePassRegistration registration;
    registration.passDesc = BuildPathTraceCleanRtxdiDiTransmissionFeaturePassDesc(
        cleanRouteRequested,
        cleanView,
        producerRequested,
        composeOutputRequested,
        debugOutputRequested);
    registration.bindingMetadata = kCleanRtxdiDiTransmissionBindings;
    registration.bindingMetadataCount = sizeof(kCleanRtxdiDiTransmissionBindings) / sizeof(kCleanRtxdiDiTransmissionBindings[0]);
    registration.runtimeInfoCallback = FillPathTraceCleanRtxdiDiTransmissionRuntimeInfo;
    return registration;
}

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiTransmissionFeatureLayoutRegistration()
{
    RtPathTraceMaterialFeaturePassRegistration registration =
        BuildPathTraceCleanRtxdiDiTransmissionFeatureRegistration(true, 16);
    registration.passDesc = BuildPathTraceCleanRtxdiDiTransmissionFeaturePassDesc(
        true,
        16,
        true,
        true,
        true);
    return registration;
}

#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiTransmissionFeature.h"
#include "PathTraceCVars.h"
#include "PathTraceCleanRtxdiDiMaterialFeatureCommon.h"

static RtPathTraceMaterialFeatureShaderDesc PathTraceCleanRtxdiDiTransmissionProducerShaderDesc()
{
    RtPathTraceMaterialFeatureShaderDesc desc;
    desc.label = "clean-room RTXDI DI transmission producer";
    desc.shaderBlobPath = "builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_transmission_producer.rt.bin";
    return desc;
}

static RtPathTraceMaterialFeaturePassDesc BuildPathTraceCleanRtxdiDiTransmissionFeaturePassDesc(
    bool cleanRouteRequested,
    int cleanView,
    bool producerRequested,
    bool composeOutputRequested,
    bool debugOutputRequested)
{
    RtPathTraceMaterialFeaturePassDesc desc =
        BuildPathTraceCleanRtxdiDiTransmissionProducerFeaturePassDesc("clean-rtxdi-di-transmission", 100u);
    desc.resourceOutputs = RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT;
    desc.primaryOutputResource = RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT;

    const bool cleanTransmissionRoute = PathTraceCleanRtxdiDiMaterialFeatureRouteEnabled(cleanRouteRequested, cleanView);
    const bool debugOutput = cleanTransmissionRoute && debugOutputRequested;
    const bool composeOutput = cleanTransmissionRoute && producerRequested && composeOutputRequested;
    if (debugOutput || composeOutput)
    {
        desc.resourceOutputs |= RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR;
    }
    desc.enabled = cleanTransmissionRoute && (producerRequested || debugOutputRequested);
    desc.debugLabel = debugOutput ? "clean-rtxdi-di-transmission-producer-debug" : "clean-rtxdi-di-transmission-producer";
    return desc;
}

static const RtPathTraceMaterialFeatureBindingDesc kCleanRtxdiDiTransmissionBindings[] = {
    RT_CLEAN_RTXDI_DI_BINDING_CURRENT_PRIMARY_SURFACE,
    RT_CLEAN_RTXDI_DI_BINDING_MATERIAL_TABLE,
    RT_CLEAN_RTXDI_DI_BINDING_MATERIAL_FEATURES,
    RT_CLEAN_RTXDI_DI_BINDING_MATERIAL_FEATURE_PARAMETERS,
    RT_CLEAN_RTXDI_DI_BINDING_MATERIAL_FEATURE_RUNTIME_CONSTANTS,
    RT_CLEAN_RTXDI_DI_BINDING_TRANSMISSION_OUTPUT,
    RT_CLEAN_RTXDI_DI_BINDING_OUTPUT_COLOR
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
    registration.shaderDesc = PathTraceCleanRtxdiDiTransmissionProducerShaderDesc();
    registration.bindingMetadata = kCleanRtxdiDiTransmissionBindings;
    registration.bindingMetadataCount = sizeof(kCleanRtxdiDiTransmissionBindings) / sizeof(kCleanRtxdiDiTransmissionBindings[0]);
    registration.runtimeInfoCallback = FillPathTraceCleanRtxdiDiTransmissionRuntimeInfo;
    registration.validation = {
        "cmake --build --preset win64-pt-dev-release",
        "r_pathTracingCleanRtxdiDiView 16; r_pathTracingCleanRtxdiDiTransmissionProducer 1; r_pathTracingCleanRtxdiDiTransmissionCompose 1; r_pathTracingCleanRtxdiDiTransmissionDebugView 1",
        "glass-like material writes thin-glass attenuation rgb plus contribution weight to transmission output",
        "opaque material writes neutral zero-weight transmission payload and dark debug sentinel",
        "clean RTXDI DI primary view 16 unchanged unless transmission compose or debug view is enabled",
        "RtPathTraceMaterialFeatureOutputDesc transmission u87 PathTraceCleanRtxdiDiTransmissionOutput plus optional output-color u1",
        "PathTraceMaterialFeatureRuntimeInfo plus PathTraceMaterialFeatureParameters t81 with b88 defaults/controls"
    };
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

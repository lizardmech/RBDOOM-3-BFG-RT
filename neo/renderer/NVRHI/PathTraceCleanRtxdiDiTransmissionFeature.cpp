#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiTransmissionFeature.h"
#include "PathTraceCVars.h"

static RtPathTraceMaterialFeaturePassDesc BuildPathTraceCleanRtxdiDiTransmissionFeaturePassDesc(
    bool cleanRouteRequested,
    int cleanView,
    bool producerRequested,
    bool composeOutputRequested,
    bool debugOutputRequested)
{
    RtPathTraceMaterialFeaturePassDesc desc;
    desc.kind = RtPathTraceMaterialFeaturePassKind::TransmissionProducer;
    desc.featureId = "clean-rtxdi-di-transmission";
    desc.materialCapsConsumed = RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION;
    desc.materialPassSupport = RT_PATH_TRACE_MATERIAL_PASS_TRANSMISSION_PRODUCER;
    desc.resourceInputs =
        RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_TABLE |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_SIDECAR |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_RUNTIME_CONSTANTS;
    desc.resourceOutputs = RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT;
    desc.primaryOutputResource = RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT;
    desc.sharedOutputPriority = 100u;

    const bool cleanTransmissionRoute = cleanRouteRequested && cleanView == 16;
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
    {
        RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE,
        30u,
        RtPathTraceMaterialFeatureBindingKind::StructuredBufferUav,
        "PrimarySurfaceHistoryCurrent"
    },
    {
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_TABLE,
        13u,
        RtPathTraceMaterialFeatureBindingKind::StructuredBufferSrv,
        "PathTraceMaterialTable"
    },
    {
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_SIDECAR,
        80u,
        RtPathTraceMaterialFeatureBindingKind::StructuredBufferSrv,
        "PathTraceMaterialFeatures"
    },
    {
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_RUNTIME_CONSTANTS,
        88u,
        RtPathTraceMaterialFeatureBindingKind::ConstantBuffer,
        "PathTraceMaterialFeatureRuntimeConstants"
    },
    {
        RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT,
        87u,
        RtPathTraceMaterialFeatureBindingKind::TextureUav,
        "PathTraceCleanRtxdiDiTransmissionOutput"
    },
    {
        RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR,
        1u,
        RtPathTraceMaterialFeatureBindingKind::TextureUav,
        "output-color"
    }
};

static void FillPathTraceCleanRtxdiDiTransmissionRuntimeInfo(
    RtPathTraceMaterialFeatureRuntimeInfo& runtimeInfo,
    const RtPathTraceMaterialFeaturePassDesc& passDesc)
{
    const bool debugOutputEnabled =
        r_pathTracingCleanRtxdiDiTransmissionDebugView.GetInteger() != 0 &&
        PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR);
    runtimeInfo.debugMode = debugOutputEnabled ? 1.0f : 0.0f;
    runtimeInfo.featureParams0[0] = 0.82f;
    runtimeInfo.featureParams0[1] = 0.93f;
    runtimeInfo.featureParams0[2] = 1.0f;
    runtimeInfo.featureParams0[3] = 0.08f;
    runtimeInfo.featureParams1[0] = 1.5f;
    runtimeInfo.featureParams1[1] = 1.0f;
    runtimeInfo.featureParams1[2] = 1.5f;
    runtimeInfo.featureParams1[3] = 0.02f;
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
    registration.shaderDesc = {
        "clean-room RTXDI DI transmission producer",
        "builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_transmission_producer.rt.bin"
    };
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
        "PathTraceMaterialFeatureRuntimeInfo plus pathtrace_clean_rtxdi_di_glass_params.hlsli material params with b88 defaults/controls"
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

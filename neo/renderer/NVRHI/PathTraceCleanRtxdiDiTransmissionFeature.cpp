#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiTransmissionFeature.h"
#include "PathTraceCVars.h"

static RtPathTraceMaterialFeaturePassDesc BuildPathTraceCleanRtxdiDiTransmissionFeaturePassDesc(
    bool cleanRouteRequested,
    int cleanView,
    bool producerRequested,
    bool debugOutputRequested)
{
    RtPathTraceMaterialFeaturePassDesc desc;
    desc.kind = RtPathTraceMaterialFeaturePassKind::TransmissionProducer;
    desc.materialCapsConsumed = RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION;
    desc.materialPassSupport = RT_PATH_TRACE_MATERIAL_PASS_TRANSMISSION_PRODUCER;
    desc.resourceInputs =
        RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_TABLE |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_SIDECAR |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_RUNTIME_CONSTANTS;
    desc.resourceOutputs = RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT;
    desc.primaryOutputResource = RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT;

    const bool cleanTransmissionRoute = cleanRouteRequested && cleanView == 16;
    const bool debugOutput = cleanTransmissionRoute && debugOutputRequested;
    if (debugOutput)
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
    const RtPathTraceMaterialFeaturePassDesc&)
{
    runtimeInfo.debugMode = r_pathTracingCleanRtxdiDiTransmissionDebugView.GetInteger() != 0 ? 1.0f : 0.0f;
}

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiTransmissionFeatureRegistration(
    bool cleanRouteRequested,
    int cleanView)
{
    const bool producerRequested = r_pathTracingCleanRtxdiDiTransmissionProducer.GetInteger() != 0;
    const bool debugOutputRequested = r_pathTracingCleanRtxdiDiTransmissionDebugView.GetInteger() != 0;

    RtPathTraceMaterialFeaturePassRegistration registration;
    registration.passDesc = BuildPathTraceCleanRtxdiDiTransmissionFeaturePassDesc(
        cleanRouteRequested,
        cleanView,
        producerRequested,
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
        "r_pathTracingCleanRtxdiDiView 16; r_pathTracingCleanRtxdiDiTransmissionProducer 1; r_pathTracingCleanRtxdiDiTransmissionDebugView 1",
        "glass-like material writes cyan to transmission output",
        "opaque material writes dark unsupported sentinel",
        "clean RTXDI DI primary view 16 unchanged",
        "RtPathTraceMaterialFeatureOutputDesc transmission u87 PathTraceCleanRtxdiDiTransmissionOutput",
        "PathTraceMaterialFeatureRuntimeInfo packed in PathTraceMaterialFeatureRuntimeConstants b88"
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
        true);
    return registration;
}

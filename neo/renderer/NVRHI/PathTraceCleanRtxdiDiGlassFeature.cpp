#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiGlassFeature.h"
#include "PathTraceCVars.h"

static RtPathTraceMaterialFeaturePassDesc BuildPathTraceCleanRtxdiDiGlassFeaturePassDesc(
    bool cleanRouteRequested,
    int cleanView,
    bool shaderRequested,
    bool debugOutputRequested)
{
    RtPathTraceMaterialFeaturePassDesc desc;
    desc.kind = RtPathTraceMaterialFeaturePassKind::TransmissionProducer;
    desc.featureId = "clean-rtxdi-di-glass";
    desc.materialCapsConsumed = RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION;
    desc.materialPassSupport = RT_PATH_TRACE_MATERIAL_PASS_TRANSMISSION_PRODUCER;
    desc.resourceInputs =
        RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_TABLE |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_SIDECAR |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_RUNTIME_CONSTANTS;
    desc.sharedOutputPriority = 50u;

    const bool cleanGlassRoute = cleanRouteRequested && cleanView == 16;
    const bool debugOutput = cleanGlassRoute && debugOutputRequested;
    if (debugOutput)
    {
        desc.resourceOutputs = RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR;
        desc.primaryOutputResource = RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR;
    }
    desc.enabled = cleanGlassRoute && (shaderRequested || debugOutputRequested);
    desc.debugLabel = debugOutput ? "clean-rtxdi-di-glass-debug" : "clean-rtxdi-di-glass";
    return desc;
}

static const RtPathTraceMaterialFeatureBindingDesc kCleanRtxdiDiGlassBindings[] = {
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
        RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR,
        1u,
        RtPathTraceMaterialFeatureBindingKind::TextureUav,
        "output-color"
    }
};

static void FillPathTraceCleanRtxdiDiGlassRuntimeInfo(
    RtPathTraceMaterialFeatureRuntimeInfo& runtimeInfo,
    const RtPathTraceMaterialFeaturePassDesc& passDesc)
{
    const bool debugOutputEnabled =
        r_pathTracingCleanRtxdiDiGlassDebugView.GetInteger() != 0 &&
        PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR);
    runtimeInfo.debugMode = debugOutputEnabled ? 1.0f : 0.0f;
    runtimeInfo.featureParams0[0] = 0.82f;
    runtimeInfo.featureParams0[1] = 0.93f;
    runtimeInfo.featureParams0[2] = 1.0f;
    runtimeInfo.featureParams0[3] = 0.08f;
    runtimeInfo.featureParams1[0] = 1.5f;
    runtimeInfo.featureParams1[1] = 1.0f;
}

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiGlassFeatureRegistration(
    bool cleanRouteRequested,
    int cleanView)
{
    const bool shaderRequested = r_pathTracingCleanRtxdiDiGlassShader.GetInteger() != 0;
    const bool debugOutputRequested = r_pathTracingCleanRtxdiDiGlassDebugView.GetInteger() != 0;

    RtPathTraceMaterialFeaturePassRegistration registration;
    registration.passDesc = BuildPathTraceCleanRtxdiDiGlassFeaturePassDesc(
        cleanRouteRequested,
        cleanView,
        shaderRequested,
        debugOutputRequested);
    registration.shaderDesc = {
        "clean-room RTXDI DI glass proof",
        "builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_glass.rt.bin"
    };
    registration.bindingMetadata = kCleanRtxdiDiGlassBindings;
    registration.bindingMetadataCount = sizeof(kCleanRtxdiDiGlassBindings) / sizeof(kCleanRtxdiDiGlassBindings[0]);
    registration.runtimeInfoCallback = FillPathTraceCleanRtxdiDiGlassRuntimeInfo;
    registration.validation = {
        "cmake --build --preset win64-pt-dev-release",
        "r_pathTracingCleanRtxdiDiView 16; r_pathTracingCleanRtxdiDiGlassShader 1; r_pathTracingCleanRtxdiDiGlassDebugView 1",
        "glass material writes thin-glass attenuation debug color through the material-feature ABI",
        "opaque material writes dark unsupported debug color",
        "clean RTXDI DI primary view 16 unchanged unless glass debug view is enabled",
        "RtPathTraceMaterialFeatureBindingDesc output-color u1 PathTraceCleanRtxdiDiGlassDebug",
        "PathTraceMaterialFeatureRecord t80 plus pathtrace_clean_rtxdi_di_glass_params.hlsli material params with b88 defaults"
    };
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
        true);
    return registration;
}

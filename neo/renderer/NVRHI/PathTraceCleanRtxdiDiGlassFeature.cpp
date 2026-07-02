#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiGlassFeature.h"
#include "PathTraceCVars.h"
#include "PathTraceCleanRtxdiDiMaterialFeatureCommon.h"

static RtPathTraceMaterialFeatureShaderDesc PathTraceCleanRtxdiDiGlassShaderDesc()
{
    RtPathTraceMaterialFeatureShaderDesc desc;
    desc.label = "clean-room RTXDI DI glass";
    desc.shaderBlobPath = "builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_glass.rt.bin";
    return desc;
}

static RtPathTraceMaterialFeaturePassDesc BuildPathTraceCleanRtxdiDiGlassFeaturePassDesc(
    bool cleanRouteRequested,
    int cleanView,
    bool shaderRequested,
    bool debugOutputRequested)
{
    RtPathTraceMaterialFeaturePassDesc desc =
        BuildPathTraceCleanRtxdiDiTransmissionProducerFeaturePassDesc("clean-rtxdi-di-glass", 50u);

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
    desc.enabled = cleanGlassRoute && (shaderRequested || debugOutputRequested);
    desc.debugLabel = debugOutput ? "clean-rtxdi-di-glass-debug" : "clean-rtxdi-di-glass";
    return desc;
}

static const RtPathTraceMaterialFeatureBindingDesc kCleanRtxdiDiGlassBindings[] = {
    RT_CLEAN_RTXDI_DI_BINDING_CURRENT_PRIMARY_SURFACE,
    RT_CLEAN_RTXDI_DI_BINDING_MATERIAL_TABLE,
    RT_CLEAN_RTXDI_DI_BINDING_MATERIAL_FEATURES,
    RT_CLEAN_RTXDI_DI_BINDING_MATERIAL_FEATURE_PARAMETERS,
    RT_CLEAN_RTXDI_DI_BINDING_MATERIAL_FEATURE_RUNTIME_CONSTANTS,
    RT_CLEAN_RTXDI_DI_BINDING_OUTPUT_COLOR_SOURCE,
    RT_CLEAN_RTXDI_DI_BINDING_RR_GUIDE_SPECULAR_ALBEDO,
    RT_CLEAN_RTXDI_DI_BINDING_RR_INPUT_COLOR,
    RT_CLEAN_RTXDI_DI_BINDING_OUTPUT_COLOR
};

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

    RtPathTraceMaterialFeaturePassRegistration registration;
    registration.passDesc = BuildPathTraceCleanRtxdiDiGlassFeaturePassDesc(
        cleanRouteRequested,
        cleanView,
        shaderRequested,
        debugOutputRequested);
    registration.shaderDesc = PathTraceCleanRtxdiDiGlassShaderDesc();
    registration.bindingMetadata = kCleanRtxdiDiGlassBindings;
    registration.bindingMetadataCount = sizeof(kCleanRtxdiDiGlassBindings) / sizeof(kCleanRtxdiDiGlassBindings[0]);
    registration.runtimeInfoCallback = FillPathTraceCleanRtxdiDiGlassRuntimeInfo;
    registration.validation = {
        "cmake --build --preset win64-pt-dev-release",
        "r_pathTracingCleanRtxdiDiView 16; r_pathTracingCleanRtxdiDiGlassShader 1; optional r_pathTracingCleanRtxdiDiGlassDebugView 1",
        "glass material writes thin-glass attenuation/reflectance through the material-feature ABI",
        "opaque material writes dark unsupported debug color",
        "clean RTXDI DI primary view 16 unchanged unless the glass shader owns output-color",
        "RtPathTraceMaterialFeatureBindingDesc output-color-source t89, rr-guide-specular-albedo u53, rr-input-color u54, output-color u1",
        "PathTraceMaterialFeatureRecord t80 plus PathTraceMaterialFeatureParameters t81 with b88 defaults"
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

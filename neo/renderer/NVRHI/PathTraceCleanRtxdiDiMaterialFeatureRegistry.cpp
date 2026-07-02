#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiMaterialFeatureRegistry.h"
#include "PathTraceCleanRtxdiDiGlassFeature.h"
#include "PathTraceCleanRtxdiDiMaterialFeatureCommon.h"
#include "PathTraceCleanRtxdiDiTransmissionFeature.h"
#include "PathTraceMaterialFeatureRegistry.h"

namespace {

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiTransmissionRuntimeRegistration(
    const void* contextPtr)
{
    RtPathTraceCleanRtxdiDiMaterialFeatureRegistryContext context;
    if (contextPtr)
    {
        context = *static_cast<const RtPathTraceCleanRtxdiDiMaterialFeatureRegistryContext*>(contextPtr);
    }
    return BuildPathTraceCleanRtxdiDiTransmissionFeatureRegistration(
        context.cleanRouteRequested,
        context.cleanView);
}

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiGlassRuntimeRegistration(
    const void* contextPtr)
{
    RtPathTraceCleanRtxdiDiMaterialFeatureRegistryContext context;
    if (contextPtr)
    {
        context = *static_cast<const RtPathTraceCleanRtxdiDiMaterialFeatureRegistryContext*>(contextPtr);
    }
    return BuildPathTraceCleanRtxdiDiGlassFeatureRegistration(
        context.cleanRouteRequested,
        context.cleanView);
}

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiNoOpFeatureRegistration()
{
    RtPathTraceMaterialFeaturePassRegistration registration;
    registration.passDesc.featureId = "clean-rtxdi-di-noop";
    registration.passDesc.debugLabel = "clean-rtxdi-di-noop-feature";
    registration.validation = {
        "host registry only",
        "disabled no-op registration",
        "not applicable",
        "not applicable",
        "clean RTXDI DI primary view 16 unchanged",
        "no resources or bindings",
        "no constants"
    };
    return registration;
}

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiNoOpRuntimeRegistration(
    const void*)
{
    return BuildPathTraceCleanRtxdiDiNoOpFeatureRegistration();
}

static const RtPathTraceMaterialFeatureRegistryEntry kCleanRtxdiDiMaterialFeatureRegistry[] = {
    {
        "clean-rtxdi-di-transmission",
        BuildPathTraceCleanRtxdiDiTransmissionRuntimeRegistration,
        BuildPathTraceCleanRtxdiDiTransmissionFeatureLayoutRegistration,
        {
            "clean-room RTXDI DI transmission producer",
            RT_CLEAN_RTXDI_DI_MATERIAL_FEATURE_RT_BLOB("pathtrace_clean_rtxdi_di_transmission_producer")
        },
        PathTraceObjectGlassMaterialFeatureParameterLayout(),
        {
            "cmake --build --preset win64-pt-dev-release",
            "r_pathTracingCleanRtxdiDiView 16; r_pathTracingCleanRtxdiDiTransmissionProducer 1; r_pathTracingCleanRtxdiDiTransmissionCompose 1; r_pathTracingCleanRtxdiDiTransmissionDebugView 1",
            "glass-like material writes thin-glass attenuation rgb plus contribution weight to transmission output",
            "opaque material writes neutral zero-weight transmission payload and dark debug sentinel",
            "clean RTXDI DI primary view 16 unchanged unless transmission compose or debug view is enabled",
            "RtPathTraceMaterialFeatureOutputDesc transmission u87 plus output-color-source t89, optional rr-input-color u54 and output-color u1",
            "PathTraceMaterialFeatureRuntimeInfo plus PathTraceMaterialFeatureParameters t81 with b88 defaults/controls"
        },
        RtPathTraceMaterialFeaturePassKind::TransmissionProducer,
        RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION,
        RT_PATH_TRACE_MATERIAL_PASS_TRANSMISSION_PRODUCER,
        PathTraceCleanRtxdiDiMaterialFeatureSurfaceInputs(),
        PathTraceCleanRtxdiDiMaterialFeatureSurfaceInputs() |
            RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR_SOURCE,
        RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT |
            RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR |
            RT_MATERIAL_FEATURE_RESOURCE_RR_INPUT_COLOR,
        RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR
    },
    {
        "clean-rtxdi-di-glass",
        BuildPathTraceCleanRtxdiDiGlassRuntimeRegistration,
        BuildPathTraceCleanRtxdiDiGlassFeatureLayoutRegistration,
        {
            "clean-room RTXDI DI glass",
            RT_CLEAN_RTXDI_DI_MATERIAL_FEATURE_RT_BLOB("pathtrace_clean_rtxdi_di_glass")
        },
        PathTraceObjectGlassMaterialFeatureParameterLayout(),
        {
            "cmake --build --preset win64-pt-dev-release",
            "r_pathTracingCleanRtxdiDiView 16; r_pathTracingCleanRtxdiDiGlassShader 1; optional r_pathTracingCleanRtxdiDiGlassDebugView 1 or r_pathTracingCleanRtxdiDiGlassGuideDebugView 1..3",
            "glass material writes thin-glass attenuation/reflectance through the material-feature ABI",
            "opaque material writes dark unsupported debug color",
            "clean RTXDI DI primary view 16 unchanged unless the glass shader owns output-color",
            "RtPathTraceMaterialFeatureBindingDesc output-color-source t89, rr-guide-specular-albedo u53, rr-input-color u54, isolated glass guide candidates u90/u91/u92, output-color u1",
            "PathTraceMaterialFeatureRecord t80 plus PathTraceMaterialFeatureParameters t81 with b88 defaults"
        },
        RtPathTraceMaterialFeaturePassKind::TransmissionProducer,
        RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION,
        RT_PATH_TRACE_MATERIAL_PASS_TRANSMISSION_PRODUCER,
        PathTraceCleanRtxdiDiMaterialFeatureSurfaceInputs(),
        PathTraceCleanRtxdiDiMaterialFeatureSurfaceInputs() |
            RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR_SOURCE,
        RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR |
            RT_MATERIAL_FEATURE_RESOURCE_RR_GUIDE_SPECULAR_ALBEDO |
            RT_MATERIAL_FEATURE_RESOURCE_RR_INPUT_COLOR |
            RT_MATERIAL_FEATURE_RESOURCE_GLASS_GUIDE_CANDIDATE0 |
            RT_MATERIAL_FEATURE_RESOURCE_GLASS_GUIDE_CANDIDATE1 |
            RT_MATERIAL_FEATURE_RESOURCE_GLASS_GUIDE_CANDIDATE2,
        RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR
    },
    {
        "clean-rtxdi-di-noop",
        BuildPathTraceCleanRtxdiDiNoOpRuntimeRegistration,
        BuildPathTraceCleanRtxdiDiNoOpFeatureRegistration,
        {},
        {},
        {},
        RtPathTraceMaterialFeaturePassKind::Disabled,
        0u,
        0u,
        RT_MATERIAL_FEATURE_RESOURCE_NONE,
        RT_MATERIAL_FEATURE_RESOURCE_NONE,
        RT_MATERIAL_FEATURE_RESOURCE_NONE,
        RT_MATERIAL_FEATURE_RESOURCE_NONE
    }
};

static_assert(
    sizeof(kCleanRtxdiDiMaterialFeatureRegistry) / sizeof(kCleanRtxdiDiMaterialFeatureRegistry[0]) <=
        RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_CAPACITY,
    "Clean RTXDI DI material feature registry exceeds fixed traversal capacity");

}

size_t PathTraceCleanRtxdiDiMaterialFeatureRegistryCount()
{
    return sizeof(kCleanRtxdiDiMaterialFeatureRegistry) / sizeof(kCleanRtxdiDiMaterialFeatureRegistry[0]);
}

size_t BuildPathTraceCleanRtxdiDiMaterialFeatureRegistryRegistrations(
    const RtPathTraceCleanRtxdiDiMaterialFeatureRegistryContext& context,
    RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCapacity)
{
    return BuildPathTraceMaterialFeatureRegistryRuntimeRegistrations(
        &context,
        kCleanRtxdiDiMaterialFeatureRegistry,
        PathTraceCleanRtxdiDiMaterialFeatureRegistryCount(),
        registrations,
        registrationCapacity);
}

size_t BuildPathTraceCleanRtxdiDiMaterialFeatureRegistryLayoutRegistrations(
    RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCapacity)
{
    return BuildPathTraceMaterialFeatureRegistryLayoutRegistrations(
        kCleanRtxdiDiMaterialFeatureRegistry,
        PathTraceCleanRtxdiDiMaterialFeatureRegistryCount(),
        registrations,
        registrationCapacity);
}

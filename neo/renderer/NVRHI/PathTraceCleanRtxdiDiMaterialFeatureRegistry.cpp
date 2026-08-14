#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCleanRtxdiDiMaterialFeatureRegistry.h"
#include "PathTraceCleanRtxdiDiGlassFeature.h"
#include "PathTraceCleanRtxdiDiMaterialFeatureCommon.h"
#include "PathTraceCleanRtxdiDiTransmissionFeature.h"
#include "PathTraceMaterialFeatureRegistry.h"

namespace {

using RtPathTraceCleanRtxdiDiRouteFeatureBuilder =
    RtPathTraceMaterialFeaturePassRegistration (*)(bool cleanRouteRequested, int cleanView);

// PathTraceCleanRtxdiPayload with the trace-hit adapter enabled contains five
// base words, eleven hit/transport words, four liquid counters, and six
// four-word liquid-candidate arrays. The transmission path declares and traces
// this payload directly, independent of the retired compact bucket libraries.
static constexpr uint32_t RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_PAYLOAD_DWORDS =
    5u + 11u + 4u + (6u * 4u);
static constexpr uint32_t RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_PAYLOAD_BYTES =
    RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_PAYLOAD_DWORDS * sizeof(uint32_t);
static_assert(
    RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_PAYLOAD_BYTES == 176u,
    "Clean RTXDI DI material-feature payload ABI must cover four liquid candidates");

RtPathTraceMaterialFeatureShaderDesc BuildPathTraceCleanRtxdiDiMaterialFeatureShaderDesc(
    const char* label,
    const char* shaderBlobPath)
{
    RtPathTraceMaterialFeatureShaderDesc desc;
    desc.label = label;
    desc.shaderBlobPath = shaderBlobPath;
    desc.rtPipeline.maxPayloadSize =
        RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_PAYLOAD_BYTES;
    return desc;
}

template<RtPathTraceCleanRtxdiDiRouteFeatureBuilder BuildFeatureRegistration>
RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiRouteRuntimeRegistration(
    const void* contextPtr)
{
    RtPathTraceCleanRtxdiDiMaterialFeatureRegistryContext context;
    if (contextPtr)
    {
        context = *static_cast<const RtPathTraceCleanRtxdiDiMaterialFeatureRegistryContext*>(contextPtr);
    }
    return BuildFeatureRegistration(
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
        BuildPathTraceCleanRtxdiDiRouteRuntimeRegistration<BuildPathTraceCleanRtxdiDiTransmissionFeatureRegistration>,
        BuildPathTraceCleanRtxdiDiTransmissionFeatureLayoutRegistration,
        BuildPathTraceCleanRtxdiDiMaterialFeatureShaderDesc(
            "clean-room RTXDI DI transmission producer",
            RT_CLEAN_RTXDI_DI_MATERIAL_FEATURE_RT_BLOB("pathtrace_clean_rtxdi_di_transmission_producer")),
        PathTraceObjectGlassMaterialFeatureParameterLayout(),
        {
            "cmake --build --preset win64-pt-dev-release",
            "r_pathTracingCleanRtxdiDiView 16; r_pathTracingCleanRtxdiDiTransmissionProducer 1; optional r_pathTracingCleanRtxdiDiTransmissionDebugView 1",
            "glass-like material writes thin-glass attenuation rgb plus contribution weight to transmission output and optional reflected radiance/cosmetic distortion to sidecars",
            "opaque material writes neutral zero-weight transmission payload and dark debug sentinel",
            "clean RTXDI DI primary view 16 unchanged unless transmission debug view is enabled",
            "RtPathTraceMaterialFeatureOutputDesc transmission u95, reflection sidecar u96, cosmetic distortion sidecar u91, rr-guide-specular-albedo u53 plus optional debug output-color-source t89, output-color u1, rr-input-color u54",
            "PathTraceMaterialFeatureRuntimeInfo plus PathTraceMaterialFeatureParameters t81 with b88 defaults/controls"
        },
        RtPathTraceMaterialFeaturePassKind::TransmissionProducer,
        RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION,
        RT_PATH_TRACE_MATERIAL_PASS_TRANSMISSION_PRODUCER,
        // Transmission owns PSR replacement and sidecar generation. Glass owns
        // final output-color composition from the readable sidecar.
        75u,
        PathTraceCleanRtxdiDiMaterialFeatureSurfaceInputs(),
        PathTraceCleanRtxdiDiComposedInputResources(),
        PathTraceCleanRtxdiDiTransmissionProducerOutputResources(),
        RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR
    },
    {
        "clean-rtxdi-di-glass",
        BuildPathTraceCleanRtxdiDiRouteRuntimeRegistration<BuildPathTraceCleanRtxdiDiGlassFeatureRegistration>,
        BuildPathTraceCleanRtxdiDiGlassFeatureLayoutRegistration,
        BuildPathTraceCleanRtxdiDiMaterialFeatureShaderDesc(
            "clean-room RTXDI DI glass",
            RT_CLEAN_RTXDI_DI_MATERIAL_FEATURE_RT_BLOB("pathtrace_clean_rtxdi_di_glass")),
        PathTraceObjectGlassMaterialFeatureParameterLayout(),
        {
            "cmake --build --preset win64-pt-dev-release",
            "r_pathTracingCleanRtxdiDiView 16; r_pathTracingCleanRtxdiDiGlassShader 1; optional r_pathTracingCleanRtxdiDiGlassDebugView 1",
            "glass material writes thin-glass attenuation/reflectance through the material-feature ABI",
            "opaque material writes dark unsupported debug color",
            "clean RTXDI DI primary view 16 unchanged unless the glass shader owns output-color",
            "RtPathTraceMaterialFeatureBindingDesc transmission-sidecar t87, output-color-source t89, reflection-sidecar t90, distortion-sidecar t92, output-color u1, rr-input-color u54 for DLSSRR color presentation",
            "PathTraceMaterialFeatureRecord t80 plus PathTraceMaterialFeatureParameters t81 with b88 defaults"
        },
        RtPathTraceMaterialFeaturePassKind::TransmissionProducer,
        RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION,
        RT_PATH_TRACE_MATERIAL_PASS_TRANSMISSION_PRODUCER,
        50u,
        PathTraceCleanRtxdiDiMaterialFeatureSurfaceInputs(),
        PathTraceCleanRtxdiDiGlassComposeInputResources(),
        PathTraceCleanRtxdiDiComposedOutputResources(),
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

bool ValidatePathTraceCleanRtxdiDiMaterialFeatureRegistryEntries(
    bool requireRuntimeBuilder,
    bool requireLayoutBuilder)
{
    const size_t entryCount = sizeof(kCleanRtxdiDiMaterialFeatureRegistry) / sizeof(kCleanRtxdiDiMaterialFeatureRegistry[0]);
    for (size_t i = 0; i < entryCount; ++i)
    {
        const RtPathTraceMaterialFeatureRegistryEntry& entry = kCleanRtxdiDiMaterialFeatureRegistry[i];
        const char* failureReason = ValidatePathTraceMaterialFeatureRegistryEntry(
            entry,
            requireRuntimeBuilder,
            requireLayoutBuilder);
        if (failureReason)
        {
            common->Printf(
                "PathTracePrimaryPass: clean-room RTXDI DI material-feature registry entry '%s' failed validation: %s\n",
                entry.featureId ? entry.featureId : "unknown",
                failureReason);
            return false;
        }
    }
    return true;
}

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
    if (!ValidatePathTraceCleanRtxdiDiMaterialFeatureRegistryEntries(true, true))
    {
        return 0;
    }

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
    if (!ValidatePathTraceCleanRtxdiDiMaterialFeatureRegistryEntries(true, true))
    {
        return 0;
    }

    return BuildPathTraceMaterialFeatureRegistryLayoutRegistrations(
        kCleanRtxdiDiMaterialFeatureRegistry,
        PathTraceCleanRtxdiDiMaterialFeatureRegistryCount(),
        registrations,
        registrationCapacity);
}

#pragma once

// Shared clean RTXDI DI material-feature descriptor helpers.
// Concrete feature modules still own their outputs, shader identity, and cvars.

#include "PathTraceMaterialFeatureParameters.h"

static const RtPathTraceMaterialFeatureBindingDesc RT_CLEAN_RTXDI_DI_BINDING_CURRENT_PRIMARY_SURFACE = {
    RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE,
    30u,
    RtPathTraceMaterialFeatureBindingKind::StructuredBufferUav,
    "PrimarySurfaceHistoryCurrent"
};

static const RtPathTraceMaterialFeatureBindingDesc RT_CLEAN_RTXDI_DI_BINDING_MATERIAL_TABLE = {
    RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_TABLE,
    13u,
    RtPathTraceMaterialFeatureBindingKind::StructuredBufferSrv,
    "PathTraceMaterialTable"
};

static const RtPathTraceMaterialFeatureBindingDesc RT_CLEAN_RTXDI_DI_BINDING_MATERIAL_FEATURES = {
    RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_SIDECAR,
    80u,
    RtPathTraceMaterialFeatureBindingKind::StructuredBufferSrv,
    "PathTraceMaterialFeatures"
};

static const RtPathTraceMaterialFeatureBindingDesc RT_CLEAN_RTXDI_DI_BINDING_MATERIAL_FEATURE_PARAMETERS = {
    RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_PARAMETERS,
    81u,
    RtPathTraceMaterialFeatureBindingKind::StructuredBufferSrv,
    "PathTraceMaterialFeatureParameters"
};

static const RtPathTraceMaterialFeatureBindingDesc RT_CLEAN_RTXDI_DI_BINDING_MATERIAL_FEATURE_RUNTIME_CONSTANTS = {
    RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_RUNTIME_CONSTANTS,
    88u,
    RtPathTraceMaterialFeatureBindingKind::ConstantBuffer,
    "PathTraceMaterialFeatureRuntimeConstants"
};

static const RtPathTraceMaterialFeatureBindingDesc RT_CLEAN_RTXDI_DI_BINDING_OUTPUT_COLOR = {
    RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR,
    1u,
    RtPathTraceMaterialFeatureBindingKind::TextureUav,
    "output-color"
};

static const RtPathTraceMaterialFeatureBindingDesc RT_CLEAN_RTXDI_DI_BINDING_OUTPUT_COLOR_SOURCE = {
    RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR_SOURCE,
    89u,
    RtPathTraceMaterialFeatureBindingKind::TextureSrv,
    "output-color-source"
};

static const RtPathTraceMaterialFeatureBindingDesc RT_CLEAN_RTXDI_DI_BINDING_TRANSMISSION_OUTPUT = {
    RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT,
    87u,
    RtPathTraceMaterialFeatureBindingKind::TextureUav,
    "PathTraceCleanRtxdiDiTransmissionOutput"
};

static const RtPathTraceMaterialFeatureBindingDesc RT_CLEAN_RTXDI_DI_BINDING_RR_GUIDE_SPECULAR_ALBEDO = {
    RT_MATERIAL_FEATURE_RESOURCE_RR_GUIDE_SPECULAR_ALBEDO,
    53u,
    RtPathTraceMaterialFeatureBindingKind::TextureUav,
    "rr-guide-specular-albedo"
};

static const RtPathTraceMaterialFeatureBindingDesc RT_CLEAN_RTXDI_DI_BINDING_RR_INPUT_COLOR = {
    RT_MATERIAL_FEATURE_RESOURCE_RR_INPUT_COLOR,
    54u,
    RtPathTraceMaterialFeatureBindingKind::TextureUav,
    "rr-input-color"
};

static const RtPathTraceMaterialFeatureBindingDesc RT_CLEAN_RTXDI_DI_BINDING_GLASS_GUIDE_CANDIDATE0 = {
    RT_MATERIAL_FEATURE_RESOURCE_GLASS_GUIDE_CANDIDATE0,
    90u,
    RtPathTraceMaterialFeatureBindingKind::TextureUav,
    "glass-guide-candidate0"
};

static const RtPathTraceMaterialFeatureBindingDesc RT_CLEAN_RTXDI_DI_BINDING_GLASS_GUIDE_CANDIDATE1 = {
    RT_MATERIAL_FEATURE_RESOURCE_GLASS_GUIDE_CANDIDATE1,
    91u,
    RtPathTraceMaterialFeatureBindingKind::TextureUav,
    "glass-guide-candidate1"
};

static const RtPathTraceMaterialFeatureBindingDesc RT_CLEAN_RTXDI_DI_BINDING_GLASS_GUIDE_CANDIDATE2 = {
    RT_MATERIAL_FEATURE_RESOURCE_GLASS_GUIDE_CANDIDATE2,
    92u,
    RtPathTraceMaterialFeatureBindingKind::TextureUav,
    "glass-guide-candidate2"
};

inline bool PathTraceCleanRtxdiDiMaterialFeatureRouteEnabled(bool cleanRouteRequested, int cleanView)
{
    return cleanRouteRequested && cleanView == 16;
}

inline uint32_t PathTraceCleanRtxdiDiMaterialFeatureSurfaceInputs()
{
    return
        RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_TABLE |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_SIDECAR |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_PARAMETERS |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_RUNTIME_CONSTANTS;
}

inline RtPathTraceMaterialFeaturePassDesc BuildPathTraceCleanRtxdiDiTransmissionProducerFeaturePassDesc(
    const char* featureId,
    uint32_t sharedOutputPriority)
{
    RtPathTraceMaterialFeaturePassDesc desc;
    desc.kind = RtPathTraceMaterialFeaturePassKind::TransmissionProducer;
    desc.featureId = featureId;
    desc.materialCapsConsumed = RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION;
    desc.materialPassSupport = RT_PATH_TRACE_MATERIAL_PASS_TRANSMISSION_PRODUCER;
    desc.resourceInputs = PathTraceCleanRtxdiDiMaterialFeatureSurfaceInputs();
    desc.sharedOutputPriority = sharedOutputPriority;
    return desc;
}

inline void FillPathTraceCleanRtxdiDiObjectGlassRuntimeInfo(
    RtPathTraceMaterialFeatureRuntimeInfo& runtimeInfo,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    bool debugOutputRequested)
{
    const bool debugOutputEnabled =
        debugOutputRequested &&
        PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR);
    runtimeInfo.debugMode = debugOutputEnabled ? 1.0f : 0.0f;
    CopyPathTraceMaterialFeatureParametersToRuntimeInfo(
        runtimeInfo,
        BuildPathTraceObjectGlassMaterialFeatureParameters());
}

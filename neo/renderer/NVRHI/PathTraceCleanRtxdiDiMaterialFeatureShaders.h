#pragma once

// Host-side shader identities for clean RTXDI DI material-feature passes.
// Keep these descriptors aligned with
// PATH_TRACING_CLEAN_RTXDI_DI_MATERIAL_FEATURE_RT_SHADERS in neo/shaders/CMakeLists.txt.

#include "PathTraceMaterialFeaturePasses.h"

enum class RtPathTraceCleanRtxdiDiMaterialFeatureShaderId : uint8_t
{
    TransmissionProducer = 0,
    Glass,
    Count
};

RtPathTraceMaterialFeatureShaderDesc PathTraceCleanRtxdiDiMaterialFeatureShaderDesc(
    RtPathTraceCleanRtxdiDiMaterialFeatureShaderId shaderId);

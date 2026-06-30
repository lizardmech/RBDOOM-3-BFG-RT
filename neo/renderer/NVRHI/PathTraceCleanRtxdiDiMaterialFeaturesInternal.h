#pragma once

// Internal bridge from the clean RTXDI DI facade to generic material-feature
// mechanics. Render-level code should include PathTraceCleanRtxdiDiMaterialFeatures.h.

#include "PathTraceCleanRtxdiDiMaterialFeatures.h"
#include "PathTraceMaterialFeatureRuntime.h"

struct RtPathTraceCleanRtxdiDiMaterialFeatureStateAccess
{
    static RtPathTraceMaterialFeatureShaderTableState* ShaderTableState(
        RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState);
    static const RtPathTraceMaterialFeatureShaderTableState* ShaderTableState(
        const RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState);
};

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass);

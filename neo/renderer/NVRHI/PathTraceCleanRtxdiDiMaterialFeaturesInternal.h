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

struct RtPathTraceCleanRtxdiDiTransmissionPassAccess
{
    static void Init(
        RtPathTraceCleanRtxdiDiTransmissionPass& pass,
        bool cleanRouteRequested,
        int cleanView,
        bool producerRequested,
        bool debugOutputRequested,
        const RtPathTraceCleanRtxdiDiMaterialFeatureState& featureState);
    static bool CleanRouteRequested(const RtPathTraceCleanRtxdiDiTransmissionPass& pass);
    static int CleanView(const RtPathTraceCleanRtxdiDiTransmissionPass& pass);
    static bool ProducerRequested(const RtPathTraceCleanRtxdiDiTransmissionPass& pass);
    static bool DebugOutputRequested(const RtPathTraceCleanRtxdiDiTransmissionPass& pass);
    static const RtPathTraceCleanRtxdiDiMaterialFeatureState* FeatureState(
        const RtPathTraceCleanRtxdiDiTransmissionPass& pass);
};

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass);
RtPathTraceMaterialFeatureShaderDesc PathTraceCleanRtxdiDiTransmissionShaderDesc(
    const RtPathTraceCleanRtxdiDiTransmissionPass& pass);

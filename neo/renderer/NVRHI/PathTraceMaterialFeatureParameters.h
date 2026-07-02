#pragma once

// CPU-side defaults for shader-owned material feature parameter records.
// The dynamic material table stores and uploads these rows, but shader feature
// policy belongs here so new material shaders do not edit the table builder.

#include "PathTraceMaterialFeaturePasses.h"
#include "PathTraceMaterialUniverse.h"
#include "PathTracePrimarySurface.h"

enum RtPathTraceObjectGlassMaterialFeatureParam0Lane : uint8_t
{
    RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_R = 0,
    RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_G = 1,
    RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_B = 2,
    RT_PATH_TRACE_OBJECT_GLASS_PARAM0_THICKNESS = 3
};

enum RtPathTraceObjectGlassMaterialFeatureParam1Lane : uint8_t
{
    RT_PATH_TRACE_OBJECT_GLASS_PARAM1_IOR = 0,
    RT_PATH_TRACE_OBJECT_GLASS_PARAM1_STRENGTH = 1,
    RT_PATH_TRACE_OBJECT_GLASS_PARAM1_REFLECTION_BOOST = 2,
    RT_PATH_TRACE_OBJECT_GLASS_PARAM1_TRANSMISSION_FLOOR = 3
};

RtPathTraceMaterialFeatureParameterRecord BuildPathTraceObjectGlassMaterialFeatureParameters();
RtPathTraceMaterialFeatureParameterRecord BuildPathTracePortalWindowMaterialFeatureParameters();
RtPathTraceMaterialFeatureParameterRecord BuildSmokeMaterialFeatureParameterRecord(const RtSmokeMaterialUniverseFacts& facts);
void CopyPathTraceMaterialFeatureParametersToRuntimeInfo(
    RtPathTraceMaterialFeatureRuntimeInfo& runtimeInfo,
    const RtPathTraceMaterialFeatureParameterRecord& params);

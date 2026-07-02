#pragma once

// CPU-side defaults for shader-owned material feature parameter records.
// The dynamic material table stores and uploads these rows, but shader feature
// policy belongs here so new material shaders do not edit the table builder.

#include "PathTraceMaterialFeaturePasses.h"
#include "PathTraceMaterialUniverse.h"
#include "PathTracePrimarySurface.h"

RtPathTraceMaterialFeatureParameterRecord BuildPathTraceObjectGlassMaterialFeatureParameters();
RtPathTraceMaterialFeatureParameterRecord BuildPathTracePortalWindowMaterialFeatureParameters();
RtPathTraceMaterialFeatureParameterRecord BuildSmokeMaterialFeatureParameterRecord(const RtSmokeMaterialUniverseFacts& facts);
void CopyPathTraceMaterialFeatureParametersToRuntimeInfo(
    RtPathTraceMaterialFeatureRuntimeInfo& runtimeInfo,
    const RtPathTraceMaterialFeatureParameterRecord& params);

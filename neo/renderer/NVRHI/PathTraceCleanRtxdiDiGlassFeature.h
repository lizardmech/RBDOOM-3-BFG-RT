#pragma once

// Clean RTXDI DI glass material-feature descriptor.
//
// This module owns the concrete glass shader proof contract; the clean DI
// material-feature facade owns only registry and runtime mechanics.

#include "PathTraceMaterialFeaturePasses.h"

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiGlassFeatureRegistration(
    bool cleanRouteRequested,
    int cleanView);
RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiGlassFeatureLayoutRegistration();

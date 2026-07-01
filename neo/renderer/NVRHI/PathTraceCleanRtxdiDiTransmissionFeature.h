#pragma once

// Clean RTXDI DI transmission material-feature descriptor.
//
// This module owns the concrete shader pass contract; the clean DI material
// feature facade owns only registry and runtime mechanics.

#include "PathTraceMaterialFeaturePasses.h"

RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiTransmissionFeatureRegistration(
    bool cleanRouteRequested,
    int cleanView);
RtPathTraceMaterialFeaturePassRegistration BuildPathTraceCleanRtxdiDiTransmissionFeatureLayoutRegistration();

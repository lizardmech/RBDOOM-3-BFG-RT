#pragma once

// Host registry for clean RTXDI DI material-feature passes.
//
// Concrete shader modules register here; the clean DI facade iterates the
// resulting descriptors without knowing which shader feature produced them.

#include "PathTraceMaterialFeaturePasses.h"

#include <cstddef>

struct RtPathTraceCleanRtxdiDiMaterialFeatureRegistryContext
{
    bool cleanRouteRequested = false;
    int cleanView = 0;
};

static constexpr size_t RT_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_REGISTRATION_CAPACITY =
    static_cast<size_t>(RtPathTraceMaterialFeatureShaderTable::Count);

size_t PathTraceCleanRtxdiDiMaterialFeatureRegistryCount();
size_t BuildPathTraceCleanRtxdiDiMaterialFeatureRegistryRegistrations(
    const RtPathTraceCleanRtxdiDiMaterialFeatureRegistryContext& context,
    RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCapacity);
size_t BuildPathTraceCleanRtxdiDiMaterialFeatureRegistryLayoutRegistrations(
    RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCapacity);

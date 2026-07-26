#pragma once

// Pure GEO-09 audit contract for the canonical skinned consumer funnel.
//
// This does not create geometry, lights, or shader-visible state. It verifies
// that the accepted CPU route, GPU upload, TLAS plan, legacy mapped-triangle
// metadata, material table, and current/previous vertex domains describe the
// same skinned surfaces before later GEO-09 slices promote more consumers.

#include "PathTraceSkinnedHitRoute.h"

#include <cstdint>
#include <vector>

struct PtSkinnedConsumerAuditInput
{
    const PtSkinnedHitRouteBuild* cpuBuild = nullptr;
    const PtSkinnedHitRouteGpuUpload* gpuUpload = nullptr;
    const PtSkinnedTlasRoutePlan* tlasPlan = nullptr;
    bool requireTlasPlan = true;

    const std::vector<std::uint32_t>* legacyTriangleClasses = nullptr;
    const std::vector<std::uint32_t>* legacyTriangleMaterialIds = nullptr;
    const std::vector<std::uint32_t>* legacyTriangleMaterialIndexes = nullptr;
    const std::vector<std::uint32_t>* materialTableIds = nullptr;

    std::uint64_t currentOutputVertexCount = 0;
    std::uint64_t previousPositionCount = 0;
};

struct PtSkinnedConsumerAuditStats
{
    std::uint64_t routes = 0;
    std::uint64_t triangles = 0;
    std::uint64_t mappedLegacyTriangles = 0;
    std::uint64_t sourceOnlyTriangles = 0;
    std::uint64_t motionReadyRoutes = 0;
    std::uint64_t motionMissingRoutes = 0;

    std::uint64_t missingInput = 0;
    std::uint64_t routeCountMismatch = 0;
    std::uint64_t routeUploadMismatch = 0;
    std::uint64_t tlasRouteMismatch = 0;
    std::uint64_t triangleRangeMismatch = 0;
    std::uint64_t triangleUploadMismatch = 0;
    std::uint64_t legacyMetadataMismatch = 0;
    std::uint64_t materialTableMismatch = 0;
    std::uint64_t currentRangeMismatch = 0;
    std::uint64_t previousRangeMismatch = 0;
    std::uint64_t primitiveIdentityInvalid = 0;
    std::uint64_t emissiveIdentityInvalid = 0;
    std::uint64_t emissiveIdentityCollision = 0;

    bool Accepted() const;
};

PtSkinnedConsumerAuditStats PtAuditSkinnedConsumerContract(
    const PtSkinnedConsumerAuditInput& input);

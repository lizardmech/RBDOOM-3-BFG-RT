#ifndef PATH_TRACE_SKINNED_HIT_ROUTE_HLSLI
#define PATH_TRACE_SKINNED_HIT_ROUTE_HLSLI

// GEO-08 source-local hit-route ABI. The table is bound and populated before
// any per-instance skinned BLAS enters a TLAS. Record zero carries the exact
// route and metadata counts; an empty table is represented by a zero-count
// sentinel.

static const uint PT_SKINNED_HIT_ROUTE_INVALID_INDEX = 0xffffffffu;
static const uint PT_SKINNED_HIT_ROUTE_HAS_PREVIOUS = 1u << 0;
static const uint PT_SKINNED_HIT_ROUTE_HAS_SOURCE_ONLY_PRIMITIVES = 1u << 1;

struct PathTraceSkinnedHitRouteGpuRecord
{
    uint shaderInstanceId;
    uint sourceIndexOffset;
    uint outputVertexOffset;
    uint previousPositionOffset;
    uint triangleMetadataOffset;
    uint vertexCount;
    uint indexCount;
    uint triangleCount;
    uint flags;
    uint instanceHashLo;
    uint instanceHashHi;
    uint sourceChecksumLo;
    uint sourceChecksumHi;
    uint sourceGpuIndexGenerationLo;
    uint sourceGpuIndexGenerationHi;
    uint outputStorageGenerationLo;
    uint outputStorageGenerationHi;
    uint routeCount;
    uint triangleMetadataCount;
    uint padding0;
};

struct PathTraceSkinnedHitRouteGpuTriangle
{
    uint sourcePrimitiveIndex;
    uint legacyPrimitiveIndex;
    uint materialId;
    uint materialIndex;
    uint triangleClassAndFlags;
    uint canonicalPrimitiveHashLo;
    uint canonicalPrimitiveHashHi;
    uint emissiveIdentityHashLo;
    uint emissiveIdentityHashHi;
};

StructuredBuffer<PathTraceSkinnedHitRouteGpuRecord>
    SmokeSkinnedHitRouteRecords : register(t18);
StructuredBuffer<PathTraceSkinnedHitRouteGpuTriangle>
    SmokeSkinnedHitRouteTriangles : register(t19);

uint PathTraceSkinnedHitRouteCount()
{
    return SmokeSkinnedHitRouteRecords[0].routeCount;
}

uint PathTraceSkinnedHitRouteFirstInstanceId()
{
    return SmokeSkinnedHitRouteRecords[0].shaderInstanceId;
}

bool PathTraceIsSkinnedHitRouteInstance(uint instanceId)
{
    const uint routeCount = PathTraceSkinnedHitRouteCount();
    const uint firstInstanceId =
        PathTraceSkinnedHitRouteFirstInstanceId();
    return routeCount != 0u &&
        instanceId >= firstInstanceId &&
        instanceId - firstInstanceId < routeCount;
}

bool PathTraceLoadSkinnedHitRoute(
    uint instanceId,
    out PathTraceSkinnedHitRouteGpuRecord route)
{
    route = (PathTraceSkinnedHitRouteGpuRecord)0;
    if (!PathTraceIsSkinnedHitRouteInstance(instanceId))
    {
        return false;
    }
    const uint routeIndex =
        instanceId - PathTraceSkinnedHitRouteFirstInstanceId();
    route = SmokeSkinnedHitRouteRecords[routeIndex];
    return route.shaderInstanceId == instanceId &&
        route.routeCount == PathTraceSkinnedHitRouteCount() &&
        route.triangleMetadataCount ==
            SmokeSkinnedHitRouteRecords[0].triangleMetadataCount;
}

bool PathTraceLoadSkinnedHitRouteTriangle(
    PathTraceSkinnedHitRouteGpuRecord route,
    uint primitiveIndex,
    out PathTraceSkinnedHitRouteGpuTriangle routeTriangle)
{
    routeTriangle = (PathTraceSkinnedHitRouteGpuTriangle)0;
    if (primitiveIndex >= route.triangleCount)
    {
        return false;
    }
    const uint metadataIndex =
        route.triangleMetadataOffset + primitiveIndex;
    if (metadataIndex >= route.triangleMetadataCount)
    {
        return false;
    }
    routeTriangle = SmokeSkinnedHitRouteTriangles[metadataIndex];
    return routeTriangle.sourcePrimitiveIndex == primitiveIndex;
}

#endif

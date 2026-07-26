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
    // Table-wide previous-position count in the reserved ABI word.
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

struct PathTraceSkinnedPreviousPosition
{
    float4 previousPosition;
};

StructuredBuffer<PathTraceSkinnedHitRouteGpuRecord>
    SmokeSkinnedHitRouteRecords : register(t18);
StructuredBuffer<PathTraceSkinnedHitRouteGpuTriangle>
    SmokeSkinnedHitRouteTriangles : register(t19);
StructuredBuffer<uint>
    SmokeSkinnedSourceIndices : register(t28);
StructuredBuffer<PathTraceSkinnedPreviousPosition>
    SmokeSkinnedPreviousPositions : register(t32);

uint PathTraceSkinnedHitRouteCount()
{
    return SmokeSkinnedHitRouteRecords[0].routeCount;
}

uint PathTraceSkinnedHitRouteFirstInstanceId()
{
    return SmokeSkinnedHitRouteRecords[0].shaderInstanceId;
}

#ifndef RB_PT_RIGID_HIT_ROUTE_INSTANCE_COUNT
#define RB_PT_RIGID_HIT_ROUTE_INSTANCE_COUNT() \
    ((uint)max(ToyPathInfo.w, 0.0))
#endif

#define PathTraceIsRigidHitRouteInstance(instanceId) \
    ((instanceId) >= 2u && \
        (instanceId) - 2u < RB_PT_RIGID_HIT_ROUTE_INSTANCE_COUNT())

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

#if !defined(RB_PT_SKINNED_HIT_ROUTE_LIGHTWEIGHT)
static bool PathTraceLoadSkinnedHitRouteIndices(
    PathTraceSkinnedHitRouteGpuRecord route,
    uint primitiveIndex,
    out uint i0,
    out uint i1,
    out uint i2)
{
    i0 = 0u;
    i1 = 0u;
    i2 = 0u;
    if (primitiveIndex >= route.triangleCount ||
        primitiveIndex * 3u + 2u >= route.indexCount)
    {
        return false;
    }
    const uint indexOffset =
        route.sourceIndexOffset + primitiveIndex * 3u;
    i0 = SmokeSkinnedSourceIndices[indexOffset + 0u];
    i1 = SmokeSkinnedSourceIndices[indexOffset + 1u];
    i2 = SmokeSkinnedSourceIndices[indexOffset + 2u];
    return i0 < route.vertexCount &&
        i1 < route.vertexCount &&
        i2 < route.vertexCount;
}

static bool PathTraceLoadSkinnedHitRouteTriangleData(
    uint instanceId,
    uint primitiveIndex,
    out PathTraceSkinnedHitRouteGpuRecord route,
    out PathTraceSkinnedHitRouteGpuTriangle routeTriangle,
    out uint outputVertexIndex0,
    out uint outputVertexIndex1,
    out uint outputVertexIndex2)
{
    route = (PathTraceSkinnedHitRouteGpuRecord)0;
    routeTriangle = (PathTraceSkinnedHitRouteGpuTriangle)0;
    outputVertexIndex0 = 0u;
    outputVertexIndex1 = 0u;
    outputVertexIndex2 = 0u;
    uint localIndex0;
    uint localIndex1;
    uint localIndex2;
    if (!PathTraceLoadSkinnedHitRoute(instanceId, route) ||
        !PathTraceLoadSkinnedHitRouteTriangle(
            route,
            primitiveIndex,
            routeTriangle) ||
        !PathTraceLoadSkinnedHitRouteIndices(
            route,
            primitiveIndex,
            localIndex0,
            localIndex1,
            localIndex2))
    {
        return false;
    }
    outputVertexIndex0 = route.outputVertexOffset + localIndex0;
    outputVertexIndex1 = route.outputVertexOffset + localIndex1;
    outputVertexIndex2 = route.outputVertexOffset + localIndex2;
    return outputVertexIndex0 >= route.outputVertexOffset &&
        outputVertexIndex1 >= route.outputVertexOffset &&
        outputVertexIndex2 >= route.outputVertexOffset;
}

static bool PathTraceLoadSkinnedHitRoutePreviousTriangleData(
    uint instanceId,
    uint primitiveIndex,
    out PathTraceSkinnedHitRouteGpuRecord route,
    out PathTraceSkinnedHitRouteGpuTriangle routeTriangle,
    out uint currentVertexIndex0,
    out uint currentVertexIndex1,
    out uint currentVertexIndex2,
    out uint previousVertexIndex0,
    out uint previousVertexIndex1,
    out uint previousVertexIndex2)
{
    previousVertexIndex0 = 0u;
    previousVertexIndex1 = 0u;
    previousVertexIndex2 = 0u;
    if (!PathTraceLoadSkinnedHitRouteTriangleData(
            instanceId,
            primitiveIndex,
            route,
            routeTriangle,
            currentVertexIndex0,
            currentVertexIndex1,
            currentVertexIndex2) ||
        (route.flags & PT_SKINNED_HIT_ROUTE_HAS_PREVIOUS) == 0u ||
        route.previousPositionOffset ==
            PT_SKINNED_HIT_ROUTE_INVALID_INDEX)
    {
        return false;
    }

    const uint localIndex0 =
        currentVertexIndex0 - route.outputVertexOffset;
    const uint localIndex1 =
        currentVertexIndex1 - route.outputVertexOffset;
    const uint localIndex2 =
        currentVertexIndex2 - route.outputVertexOffset;
    if (localIndex0 >= route.vertexCount ||
        localIndex1 >= route.vertexCount ||
        localIndex2 >= route.vertexCount ||
        route.previousPositionOffset >
            PT_SKINNED_HIT_ROUTE_INVALID_INDEX - localIndex0 ||
        route.previousPositionOffset >
            PT_SKINNED_HIT_ROUTE_INVALID_INDEX - localIndex1 ||
        route.previousPositionOffset >
            PT_SKINNED_HIT_ROUTE_INVALID_INDEX - localIndex2)
    {
        return false;
    }

    previousVertexIndex0 =
        route.previousPositionOffset + localIndex0;
    previousVertexIndex1 =
        route.previousPositionOffset + localIndex1;
    previousVertexIndex2 =
        route.previousPositionOffset + localIndex2;
    const uint previousPositionCount =
        SmokeSkinnedHitRouteRecords[0].padding0;
    return previousVertexIndex0 < previousPositionCount &&
        previousVertexIndex1 < previousPositionCount &&
        previousVertexIndex2 < previousPositionCount;
}
#endif

#endif

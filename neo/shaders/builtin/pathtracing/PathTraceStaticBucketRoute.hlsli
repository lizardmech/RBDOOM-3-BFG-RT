#ifndef PATH_TRACE_STATIC_BUCKET_ROUTE_HLSLI
#define PATH_TRACE_STATIC_BUCKET_ROUTE_HLSLI

// GEO-10 resident-pool route. Bucket BLASes use the existing static
// t3/t4/t5/t9/t11 pool and contribution-0/1 hit records. The upper bit of the
// 24-bit InstanceID identifies a bucket; the lower 23 bits encode its first
// resident surface record. GeometryIndex selects the surface and
// PrimitiveIndex selects a surface-local triangle. No bucket-specific
// descriptor or route-record read is required in the hit path.
#ifndef RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS
#define RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS 0
#endif

static const uint PATH_TRACE_STATIC_BUCKET_INSTANCE_ID_BASE =
    0x00800000u;
static const uint PATH_TRACE_STATIC_BUCKET_SURFACE_OFFSET_MASK =
    0x007fffffu;
static const uint PATH_TRACE_SHADER_INSTANCE_ID_MASK =
    0x00ffffffu;
static const uint PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_VALID = 1u;
static const uint PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_SOURCE_TRIANGLE_SHIFT =
    1u;
static const uint PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_VALID_MASK =
    (1u << PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_SOURCE_TRIANGLE_SHIFT) - 1u;
static const uint PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_WORDS = 4u;

#define SmokeStaticBucketVertices SmokeStaticVertices
#define SmokeStaticBucketIndices SmokeStaticIndices
#define SmokeStaticBucketTriangleClasses SmokeStaticTriangleClasses
#define SmokeStaticBucketTriangleMaterials SmokeStaticTriangleMaterials
#define SmokeStaticBucketTriangleMaterialIndexes SmokeStaticTriangleMaterialIndexes

// Compatibility shape for existing consumers. The arithmetic route only
// populates fields that those consumers read; it is not a GPU buffer ABI.
struct PathTraceStaticBucketRouteRecord
{
    uint instanceId;
    uint vertexOffset;
    uint indexOffset;
    uint triangleOffset;
    uint vertexCount;
    uint indexCount;
    uint triangleCount;
    uint surfaceCount;
    uint generationLo;
    uint generationHi;
    uint bucketKeyLo;
    uint bucketKeyHi;
};

struct PathTraceStaticGeometryAddress
{
    uint surfaceRecordIndex;
    uint triangleIndex;
    uint sourceTriangleIndex;
    uint indexOffset;
    uint triangleCount;
    uint3 vertexIndexes;
};

bool PathTraceStaticBucketInstanceInPublishedRange(
    uint instanceId,
    uint4 routeInfo,
    out uint surfaceRecordBase)
{
    surfaceRecordBase = 0u;
#if !RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS
    return false;
#else
    if (routeInfo.y == 0u ||
        (instanceId & ~PATH_TRACE_SHADER_INSTANCE_ID_MASK) != 0u ||
        (instanceId &
            PATH_TRACE_STATIC_BUCKET_INSTANCE_ID_BASE) == 0u)
    {
        return false;
    }

    surfaceRecordBase =
        instanceId &
        PATH_TRACE_STATIC_BUCKET_SURFACE_OFFSET_MASK;
    return true;
#endif
}

bool PathTraceIsStaticBucketRouteInstance(
    uint instanceId,
    uint4 routeInfo)
{
    uint surfaceRecordBase = 0u;
    return PathTraceStaticBucketInstanceInPublishedRange(
        instanceId,
        routeInfo,
        surfaceRecordBase);
}

bool PathTraceTryResolveStaticBucketGeometryAddress(
    uint instanceId,
    uint geometryIndex,
    uint primitiveIndex,
    uint4 routeInfo,
    uint staticVertexCount,
    uint staticIndexCount,
    uint staticTriangleCount,
    out PathTraceStaticGeometryAddress address)
{
    address = (PathTraceStaticGeometryAddress)0;
#if !RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS
    return false;
#else
    uint surfaceRecordBase = 0u;
    if (!PathTraceStaticBucketInstanceInPublishedRange(
            instanceId,
            routeInfo,
            surfaceRecordBase) ||
        geometryIndex >
            PATH_TRACE_STATIC_BUCKET_SURFACE_OFFSET_MASK -
                surfaceRecordBase)
    {
        return false;
    }

    const uint surfaceRecordIndex =
        surfaceRecordBase + geometryIndex;
    if (surfaceRecordIndex >= routeInfo.y ||
        surfaceRecordIndex >
            (0xffffffffu - staticTriangleCount) /
                PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_WORDS)
    {
        return false;
    }

    const uint surfaceRecordWord =
        staticTriangleCount +
        surfaceRecordIndex *
            PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_WORDS;
    const uint indexOffset =
        SmokeStaticTriangleClasses[surfaceRecordWord + 0u];
    const uint triangleOffset =
        SmokeStaticTriangleClasses[surfaceRecordWord + 1u];
    const uint triangleCount =
        SmokeStaticTriangleClasses[surfaceRecordWord + 2u];
    const uint flags =
        SmokeStaticTriangleClasses[surfaceRecordWord + 3u];
    if ((flags &
            PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_VALID_MASK) !=
            PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_VALID ||
        primitiveIndex >= triangleCount ||
        triangleOffset > staticTriangleCount ||
        primitiveIndex >
            staticTriangleCount - triangleOffset ||
        indexOffset > staticIndexCount ||
        primitiveIndex >
            (staticIndexCount - indexOffset) / 3u)
    {
        return false;
    }

    const uint triangleIndex =
        triangleOffset + primitiveIndex;
    const uint sourceTriangleOffset =
        flags >>
        PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_SOURCE_TRIANGLE_SHIFT;
    if (primitiveIndex >
        0xffffffffu - sourceTriangleOffset)
    {
        return false;
    }
    const uint sourceTriangleIndex =
        sourceTriangleOffset + primitiveIndex;
    const uint packedIndexOffset =
        indexOffset + primitiveIndex * 3u;
    if (triangleIndex >= staticTriangleCount ||
        packedIndexOffset > staticIndexCount ||
        staticIndexCount - packedIndexOffset < 3u)
    {
        return false;
    }

    const uint3 vertexIndexes = uint3(
        SmokeStaticIndices[packedIndexOffset + 0u],
        SmokeStaticIndices[packedIndexOffset + 1u],
        SmokeStaticIndices[packedIndexOffset + 2u]);
    if (any(vertexIndexes >= staticVertexCount))
    {
        return false;
    }

    address.surfaceRecordIndex = surfaceRecordIndex;
    address.triangleIndex = triangleIndex;
    address.sourceTriangleIndex = sourceTriangleIndex;
    address.indexOffset = packedIndexOffset;
    address.triangleCount = triangleCount;
    address.vertexIndexes = vertexIndexes;
    return true;
#endif
}

#if RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS
uint PathTraceStaticBucketCanonicalSurfaceInstanceId(
    PathTraceStaticGeometryAddress address)
{
    return PATH_TRACE_STATIC_BUCKET_INSTANCE_ID_BASE |
        address.surfaceRecordIndex;
}

// Replays a canonical surface/source tuple after the hit stage has discarded
// GeometryIndex. The canonical InstanceID names exactly one surface record, so
// GeometryIndex is zero and the record's authored source offset recovers the
// surface-local PrimitiveIndex. This is also the address stored by static
// emissive records.
bool PathTraceTryResolveCanonicalStaticBucketSourceTriangle(
    uint canonicalInstanceId,
    uint sourceTriangleIndex,
    uint4 routeInfo,
    uint staticVertexCount,
    uint staticIndexCount,
    uint staticTriangleCount,
    out PathTraceStaticGeometryAddress address)
{
    address = (PathTraceStaticGeometryAddress)0;
    uint surfaceRecordIndex = 0u;
    if (!PathTraceStaticBucketInstanceInPublishedRange(
            canonicalInstanceId,
            routeInfo,
            surfaceRecordIndex) ||
        surfaceRecordIndex >= routeInfo.y ||
        surfaceRecordIndex >
            (0xffffffffu - staticTriangleCount) /
                PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_WORDS)
    {
        return false;
    }

    const uint surfaceRecordWord =
        staticTriangleCount +
        surfaceRecordIndex *
            PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_WORDS;
    const uint flags =
        SmokeStaticTriangleClasses[surfaceRecordWord + 3u];
    if ((flags &
            PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_VALID_MASK) !=
            PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_VALID)
    {
        return false;
    }

    const uint sourceTriangleOffset =
        flags >>
        PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_SOURCE_TRIANGLE_SHIFT;
    if (sourceTriangleIndex < sourceTriangleOffset)
    {
        return false;
    }

    return PathTraceTryResolveStaticBucketGeometryAddress(
            canonicalInstanceId,
            0u,
            sourceTriangleIndex - sourceTriangleOffset,
            routeInfo,
            staticVertexCount,
            staticIndexCount,
            staticTriangleCount,
            address) &&
        address.surfaceRecordIndex == surfaceRecordIndex &&
        address.sourceTriangleIndex == sourceTriangleIndex;
}
#endif

// Transitional compatibility wrapper for consumers that have not yet moved
// GeometryIndex into their hit-entry address. It intentionally fails closed;
// the old triangle-base interpretation is not valid for multi-geometry BLASes.
bool PathTraceTryLoadStaticBucketTriangleRoute(
    uint instanceId,
    uint primitiveIndex,
    uint4 routeInfo,
    out PathTraceStaticBucketRouteRecord route,
    out uint packedTriangleIndex,
    out uint3 packedVertexIndexes)
{
    route = (PathTraceStaticBucketRouteRecord)0;
    packedTriangleIndex = 0u;
    packedVertexIndexes = uint3(0u, 0u, 0u);
    return false;
}

#endif

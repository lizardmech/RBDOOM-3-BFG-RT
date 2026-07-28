#ifndef PATH_TRACE_STATIC_BUCKET_ROUTE_HLSLI
#define PATH_TRACE_STATIC_BUCKET_ROUTE_HLSLI

// GEO-10 resident-pool route. Every bucket BLAS contains one geometry over a
// contiguous range of the existing static t3/t4/t5/t9/t11 pool. The upper bit
// of the 24-bit InstanceID identifies a bucket and the lower 23 bits encode
// its packed triangle base. PrimitiveIndex is bucket-local, so one addition
// recovers the established static-pool triangle address. GeometryIndex must be
// zero and no descriptor, route-record, or surface-record read is required.
#ifndef RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS
#define RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS 0
#endif

static const uint PATH_TRACE_STATIC_BUCKET_INSTANCE_ID_BASE =
    0x00800000u;
static const uint PATH_TRACE_STATIC_BUCKET_TRIANGLE_OFFSET_MASK =
    0x007fffffu;
static const uint PATH_TRACE_SHADER_INSTANCE_ID_MASK =
    0x00ffffffu;

#define SmokeStaticBucketVertices SmokeStaticVertices
#define SmokeStaticBucketIndices SmokeStaticIndices
#define SmokeStaticBucketTriangleClasses SmokeStaticTriangleClasses
#define SmokeStaticBucketTriangleMaterials SmokeStaticTriangleMaterials
#define SmokeStaticBucketTriangleMaterialIndexes SmokeStaticTriangleMaterialIndexes

// Compatibility shape for consumers that still use the shared routed-static
// helper signature. It is not a GPU buffer ABI.
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
    uint triangleBase;
    uint triangleIndex;
    uint sourceTriangleIndex;
    uint indexOffset;
    uint triangleCount;
    uint3 vertexIndexes;
};

bool PathTraceStaticBucketInstanceInPublishedRange(
    uint instanceId,
    uint4 routeInfo,
    out uint triangleBase)
{
    triangleBase = 0u;
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

    triangleBase =
        instanceId &
        PATH_TRACE_STATIC_BUCKET_TRIANGLE_OFFSET_MASK;
    return triangleBase < routeInfo.y;
#endif
}

bool PathTraceIsStaticBucketRouteInstance(
    uint instanceId,
    uint4 routeInfo)
{
    uint triangleBase = 0u;
    return PathTraceStaticBucketInstanceInPublishedRange(
        instanceId,
        routeInfo,
        triangleBase);
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
    uint triangleBase = 0u;
    if (!PathTraceStaticBucketInstanceInPublishedRange(
            instanceId,
            routeInfo,
            triangleBase) ||
        geometryIndex != 0u ||
        triangleBase >= staticTriangleCount ||
        primitiveIndex >= staticTriangleCount - triangleBase)
    {
        return false;
    }

    const uint triangleIndex = triangleBase + primitiveIndex;
    if (triangleIndex > (0xffffffffu - 2u) / 3u)
    {
        return false;
    }
    const uint packedIndexOffset = triangleIndex * 3u;
    if (packedIndexOffset >= staticIndexCount ||
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

    address.triangleBase = triangleBase;
    address.triangleIndex = triangleIndex;
    // The stable replay identity is the hardware bucket-local primitive. CPU
    // audit code retains the separate monolithic-source mapping.
    address.sourceTriangleIndex = primitiveIndex;
    address.indexOffset = packedIndexOffset;
    address.triangleCount = staticTriangleCount - triangleBase;
    address.vertexIndexes = vertexIndexes;
    return true;
#endif
}

#if RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS
uint PathTraceStaticBucketCanonicalSurfaceInstanceId(
    PathTraceStaticGeometryAddress address)
{
    return PATH_TRACE_STATIC_BUCKET_INSTANCE_ID_BASE |
        address.triangleBase;
}

// Replay uses the same bucket InstanceID plus bucket-local PrimitiveIndex that
// the hit stage received. The legacy name is retained temporarily so consumer
// edits stay mechanical while the surface-record route is removed.
bool PathTraceTryResolveCanonicalStaticBucketSourceTriangle(
    uint canonicalInstanceId,
    uint sourceTriangleIndex,
    uint4 routeInfo,
    uint staticVertexCount,
    uint staticIndexCount,
    uint staticTriangleCount,
    out PathTraceStaticGeometryAddress address)
{
    return PathTraceTryResolveStaticBucketGeometryAddress(
        canonicalInstanceId,
        0u,
        sourceTriangleIndex,
        routeInfo,
        staticVertexCount,
        staticIndexCount,
        staticTriangleCount,
        address);
}
#endif

// Transitional compatibility wrapper for consumers compiled without bucket
// support. A supported entry point supplies its explicit-count wrapper.
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

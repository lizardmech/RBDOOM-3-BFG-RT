#ifndef PATH_TRACE_STATIC_BUCKET_ROUTE_HLSLI
#define PATH_TRACE_STATIC_BUCKET_ROUTE_HLSLI

// GEO-10 resident-pool route. Every bucket BLAS contains fixed-size geometry
// chunks over a contiguous range of the existing static t3/t4/t5/t9/t11 pool. The upper bit
// of the 24-bit InstanceID identifies a bucket and the lower 23 bits encode
// its packed triangle base. GeometryIndex and PrimitiveIndex recover the
// bucket-local primitive without another descriptor or route-record read.
#ifndef RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS
#define RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS 0
#endif

static const uint PATH_TRACE_STATIC_BUCKET_INSTANCE_ID_BASE =
    0x00800000u;
static const uint PATH_TRACE_STATIC_BUCKET_TRIANGLE_OFFSET_MASK =
    0x007fffffu;
static const uint PATH_TRACE_SHADER_INSTANCE_ID_MASK =
    0x00ffffffu;
static const uint PATH_TRACE_BLAS_GEOMETRY_TRIANGLE_CHUNK = 256u;

#if RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS || \
    defined(PATH_TRACE_ENABLE_MONOLITHIC_CHUNK_HELPERS)
bool PathTraceTryCanonicalizeBlasPrimitive(
    uint geometryIndex,
    uint primitiveIndex,
    out uint canonicalPrimitiveIndex)
{
    canonicalPrimitiveIndex =
        geometryIndex * PATH_TRACE_BLAS_GEOMETRY_TRIANGLE_CHUNK +
        primitiveIndex;
    // The CPU builder is the authority for the 256-triangle geometry ceiling.
    // GeometryIndex and PrimitiveIndex are hardware-produced values from that
    // descriptor array, so revalidating them in every hit shader only expands
    // the already size-constrained all-views sentinel.
    return true;
}

bool PathTraceTryCanonicalizeSmokeHardwarePrimitive(
    uint instanceId,
    uint geometryIndex,
    uint primitiveIndex,
    out uint canonicalPrimitiveIndex)
{
    const bool chunkedRoute =
        instanceId <= 1u ||
        ((instanceId & ~PATH_TRACE_SHADER_INSTANCE_ID_MASK) == 0u &&
            (instanceId & PATH_TRACE_STATIC_BUCKET_INSTANCE_ID_BASE) != 0u);
    if (chunkedRoute)
    {
        canonicalPrimitiveIndex =
            geometryIndex * PATH_TRACE_BLAS_GEOMETRY_TRIANGLE_CHUNK +
            primitiveIndex;
        return true;
    }
    canonicalPrimitiveIndex = primitiveIndex;
    return true;
}
#endif

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
    uint canonicalPrimitiveIndex = 0u;
    if (!PathTraceStaticBucketInstanceInPublishedRange(
            instanceId,
            routeInfo,
            triangleBase) ||
        !PathTraceTryCanonicalizeBlasPrimitive(
            geometryIndex,
            primitiveIndex,
            canonicalPrimitiveIndex) ||
        triangleBase >= staticTriangleCount ||
        canonicalPrimitiveIndex >=
            staticTriangleCount - triangleBase)
    {
        return false;
    }

    const uint triangleIndex =
        triangleBase + canonicalPrimitiveIndex;
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
    address.sourceTriangleIndex = canonicalPrimitiveIndex;
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
        sourceTriangleIndex /
            PATH_TRACE_BLAS_GEOMETRY_TRIANGLE_CHUNK,
        sourceTriangleIndex %
            PATH_TRACE_BLAS_GEOMETRY_TRIANGLE_CHUNK,
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

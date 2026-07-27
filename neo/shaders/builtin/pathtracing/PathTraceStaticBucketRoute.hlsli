#ifndef PATH_TRACE_STATIC_BUCKET_ROUTE_HLSLI
#define PATH_TRACE_STATIC_BUCKET_ROUTE_HLSLI

// GEO-10 resident-pool route. Bucket BLASes use the existing static
// t3/t4/t5/t9/t11 pool and contribution-0/1 hit records. The upper bit of the
// 24-bit InstanceID identifies a bucket; the lower 23 bits encode its stable
// global triangle base. No bucket-specific descriptor or route-record read is
// required in the hit path.
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

bool PathTraceStaticBucketInstanceInPublishedRange(
    uint instanceId,
    uint4 routeInfo,
    out uint triangleBase)
{
    triangleBase = 0u;
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
    return true;
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

    uint triangleBase = 0u;
    if (!PathTraceStaticBucketInstanceInPublishedRange(
            instanceId,
            routeInfo,
            triangleBase) ||
        primitiveIndex >=
            PATH_TRACE_STATIC_BUCKET_INSTANCE_ID_BASE -
                triangleBase)
    {
        return false;
    }

    packedTriangleIndex =
        triangleBase + primitiveIndex;
    const uint packedIndexOffset =
        packedTriangleIndex * 3u;
    packedVertexIndexes = uint3(
        SmokeStaticIndices[packedIndexOffset + 0u],
        SmokeStaticIndices[packedIndexOffset + 1u],
        SmokeStaticIndices[packedIndexOffset + 2u]);

    route.instanceId = instanceId;
    route.vertexOffset = 0u;
    route.indexOffset = triangleBase * 3u;
    route.triangleOffset = triangleBase;
    route.vertexCount = 0xffffffffu;
    route.triangleCount =
        PATH_TRACE_STATIC_BUCKET_INSTANCE_ID_BASE -
        triangleBase;
    route.indexCount = route.triangleCount * 3u;
    return true;
}

#endif

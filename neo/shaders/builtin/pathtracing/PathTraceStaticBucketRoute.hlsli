#ifndef PATH_TRACE_STATIC_BUCKET_ROUTE_HLSLI
#define PATH_TRACE_STATIC_BUCKET_ROUTE_HLSLI

// GEO-10 checked static-bucket route ABI. The route remains shadow-only until
// the TLAS and every shared-TLAS hit consumer switch under the live rollback
// gate. Separate pipelines may override the six register macros before include.
//
// The first integrated consumer set caused repeatable Vulkan device-loss/TDR
// failures before a fresh geometry log could open. Keep the source contract in
// place for offline validation, but make the route compile-time unreachable so
// DXC can remove its buffer accesses from every production RT library.
#ifndef RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS
#define RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS 0
#endif
#ifndef RB_PT_STATIC_BUCKET_ROUTES_REGISTER
#define RB_PT_STATIC_BUCKET_ROUTES_REGISTER t83
#endif
#ifndef RB_PT_STATIC_BUCKET_VERTICES_REGISTER
#define RB_PT_STATIC_BUCKET_VERTICES_REGISTER t84
#endif
#ifndef RB_PT_STATIC_BUCKET_INDICES_REGISTER
#define RB_PT_STATIC_BUCKET_INDICES_REGISTER t85
#endif
#ifndef RB_PT_STATIC_BUCKET_CLASSES_REGISTER
#define RB_PT_STATIC_BUCKET_CLASSES_REGISTER t86
#endif
#ifndef RB_PT_STATIC_BUCKET_MATERIALS_REGISTER
#define RB_PT_STATIC_BUCKET_MATERIALS_REGISTER t87
#endif
#ifndef RB_PT_STATIC_BUCKET_MATERIAL_INDEXES_REGISTER
#define RB_PT_STATIC_BUCKET_MATERIAL_INDEXES_REGISTER t88
#endif

static const uint PATH_TRACE_STATIC_BUCKET_INSTANCE_ID_BASE =
    0x00800000u;
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

StructuredBuffer<PathTraceStaticBucketRouteRecord>
    SmokeStaticBucketRoutes :
        register(RB_PT_STATIC_BUCKET_ROUTES_REGISTER);
StructuredBuffer<PathTraceSmokeVertex>
    SmokeStaticBucketVertices :
        register(RB_PT_STATIC_BUCKET_VERTICES_REGISTER);
StructuredBuffer<uint>
    SmokeStaticBucketIndices :
        register(RB_PT_STATIC_BUCKET_INDICES_REGISTER);
StructuredBuffer<uint>
    SmokeStaticBucketTriangleClasses :
        register(RB_PT_STATIC_BUCKET_CLASSES_REGISTER);
StructuredBuffer<uint>
    SmokeStaticBucketTriangleMaterials :
        register(RB_PT_STATIC_BUCKET_MATERIALS_REGISTER);
StructuredBuffer<uint>
    SmokeStaticBucketTriangleMaterialIndexes :
        register(RB_PT_STATIC_BUCKET_MATERIAL_INDEXES_REGISTER);

bool PathTraceStaticBucketInstanceInPublishedRange(
    uint instanceId,
    uint4 routeInfo,
    out uint routeIndex)
{
    routeIndex = 0u;
#if !RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS
    return false;
#else
    if (routeInfo.x !=
            PATH_TRACE_STATIC_BUCKET_INSTANCE_ID_BASE ||
        routeInfo.y == 0u ||
        (routeInfo.z | routeInfo.w) == 0u ||
        instanceId < routeInfo.x)
    {
        return false;
    }

    routeIndex = instanceId - routeInfo.x;
    return routeIndex < routeInfo.y;
#endif
}

bool PathTraceIsStaticBucketRouteInstance(
    uint instanceId,
    uint4 routeInfo)
{
    uint routeIndex = 0u;
    return PathTraceStaticBucketInstanceInPublishedRange(
        instanceId,
        routeInfo,
        routeIndex);
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

    uint routeIndex = 0u;
    if (!PathTraceStaticBucketInstanceInPublishedRange(
            instanceId,
            routeInfo,
            routeIndex))
    {
        return false;
    }

    uint routeBufferCount = 0u;
    uint routeStride = 0u;
    SmokeStaticBucketRoutes.GetDimensions(
        routeBufferCount,
        routeStride);
    if (routeInfo.y > routeBufferCount ||
        routeStride != 48u ||
        routeIndex >= routeBufferCount)
    {
        return false;
    }

    route = SmokeStaticBucketRoutes[routeIndex];
    if (route.instanceId != instanceId ||
        route.generationLo != routeInfo.z ||
        route.generationHi != routeInfo.w ||
        route.vertexCount == 0u ||
        route.triangleCount == 0u ||
        route.triangleCount > 0x55555555u ||
        route.indexCount != route.triangleCount * 3u ||
        primitiveIndex >= route.triangleCount)
    {
        return false;
    }

    uint vertexBufferCount = 0u;
    uint vertexStride = 0u;
    uint indexBufferCount = 0u;
    uint indexStride = 0u;
    uint classBufferCount = 0u;
    uint classStride = 0u;
    uint materialBufferCount = 0u;
    uint materialStride = 0u;
    uint materialIndexBufferCount = 0u;
    uint materialIndexStride = 0u;
    SmokeStaticBucketVertices.GetDimensions(
        vertexBufferCount,
        vertexStride);
    SmokeStaticBucketIndices.GetDimensions(
        indexBufferCount,
        indexStride);
    SmokeStaticBucketTriangleClasses.GetDimensions(
        classBufferCount,
        classStride);
    SmokeStaticBucketTriangleMaterials.GetDimensions(
        materialBufferCount,
        materialStride);
    SmokeStaticBucketTriangleMaterialIndexes.GetDimensions(
        materialIndexBufferCount,
        materialIndexStride);

    const uint vertexEnd =
        route.vertexOffset + route.vertexCount;
    const uint indexEnd =
        route.indexOffset + route.indexCount;
    const uint triangleEnd =
        route.triangleOffset + route.triangleCount;
    if (vertexEnd < route.vertexOffset ||
        indexEnd < route.indexOffset ||
        triangleEnd < route.triangleOffset ||
        vertexEnd > vertexBufferCount ||
        indexEnd > indexBufferCount ||
        triangleEnd > classBufferCount ||
        triangleEnd > materialBufferCount ||
        triangleEnd > materialIndexBufferCount ||
        vertexStride != 112u ||
        indexStride != 4u ||
        classStride != 4u ||
        materialStride != 4u ||
        materialIndexStride != 4u)
    {
        return false;
    }

    const uint localIndexOffset = primitiveIndex * 3u;
    const uint packedIndexOffset =
        route.indexOffset + localIndexOffset;
    if (packedIndexOffset < route.indexOffset ||
        packedIndexOffset + 2u >= indexEnd)
    {
        return false;
    }

    packedVertexIndexes = uint3(
        SmokeStaticBucketIndices[packedIndexOffset + 0u],
        SmokeStaticBucketIndices[packedIndexOffset + 1u],
        SmokeStaticBucketIndices[packedIndexOffset + 2u]);
    if (any(packedVertexIndexes < route.vertexOffset) ||
        any(packedVertexIndexes >= vertexEnd))
    {
        return false;
    }

    packedTriangleIndex =
        route.triangleOffset + primitiveIndex;
    return packedTriangleIndex >= route.triangleOffset &&
        packedTriangleIndex < triangleEnd;
}

#endif

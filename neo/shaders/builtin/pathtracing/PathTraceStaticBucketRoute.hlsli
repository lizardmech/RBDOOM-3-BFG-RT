#ifndef PATH_TRACE_STATIC_BUCKET_ROUTE_HLSLI
#define PATH_TRACE_STATIC_BUCKET_ROUTE_HLSLI

// GEO-10 checked static-bucket route ABI. The route remains shadow-only until
// the TLAS and primary-hit consumers switch under the live rollback gate.
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
    SmokeStaticBucketRoutes : register(t83);
StructuredBuffer<PathTraceSmokeVertex>
    SmokeStaticBucketVertices : register(t84);
StructuredBuffer<uint>
    SmokeStaticBucketIndices : register(t85);
StructuredBuffer<uint>
    SmokeStaticBucketTriangleClasses : register(t86);
StructuredBuffer<uint>
    SmokeStaticBucketTriangleMaterials : register(t87);
StructuredBuffer<uint>
    SmokeStaticBucketTriangleMaterialIndexes : register(t88);

#endif

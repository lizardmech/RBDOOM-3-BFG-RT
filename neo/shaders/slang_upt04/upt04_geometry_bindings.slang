import upt04_geometry_contract;
import upt04_initial_control;
import upt04_light_contract;

#ifndef UPT07_TEMPORAL_GEOMETRY_REPLAY
#define UPT07_TEMPORAL_GEOMETRY_REPLAY 0
#endif

#ifndef UPT09_SPATIAL_GEOMETRY_REPLAY
#define UPT09_SPATIAL_GEOMETRY_REPLAY 0
#endif

#if UPT07_TEMPORAL_GEOMETRY_REPLAY
import upt07_temporal_direct_bindings;
#elif UPT09_SPATIAL_GEOMETRY_REPLAY
import upt09_spatial_direct_bindings;
#else
import upt04_initial_bindings;
#endif

#ifndef UPT04_PACKED_GEOMETRY_VERTEX
#define UPT04_PACKED_GEOMETRY_VERTEX 0
#endif

public [[vk::binding(6, 0)]] StructuredBuffer<Upt04EmissiveReplayRecord> gUpt04EmissiveReplay;
#if UPT04_PACKED_GEOMETRY_VERTEX
public [[vk::binding(7, 0)]] StructuredBuffer<Upt04CompactVertex> gUpt04StaticVertices;
#else
public [[vk::binding(7, 0)]] StructuredBuffer<Upt04SmokeVertex> gUpt04StaticVertices;
#endif
public [[vk::binding(8, 0)]] StructuredBuffer<uint> gUpt04StaticIndices;
public [[vk::binding(9, 0)]] StructuredBuffer<uint> gUpt04StaticTriangleClasses;
public [[vk::binding(10, 0)]] StructuredBuffer<uint> gUpt04StaticTriangleMaterialIndices;
#if UPT04_PACKED_GEOMETRY_VERTEX
public [[vk::binding(11, 0)]] StructuredBuffer<Upt04CompactVertex> gUpt04DynamicVertices;
#else
public [[vk::binding(11, 0)]] StructuredBuffer<Upt04SmokeVertex> gUpt04DynamicVertices;
#endif
public [[vk::binding(12, 0)]] StructuredBuffer<uint> gUpt04DynamicIndices;
public [[vk::binding(13, 0)]] StructuredBuffer<uint> gUpt04DynamicTriangleClasses;
public [[vk::binding(14, 0)]] StructuredBuffer<uint> gUpt04DynamicTriangleMaterialIndices;
#if UPT04_PACKED_GEOMETRY_VERTEX
public [[vk::binding(15, 0)]] StructuredBuffer<Upt04CompactVertex> gUpt04RigidVertices;
#else
public [[vk::binding(15, 0)]] StructuredBuffer<Upt04SmokeVertex> gUpt04RigidVertices;
#endif
public [[vk::binding(16, 0)]] StructuredBuffer<uint> gUpt04RigidIndices;
// Word-address the large route records. Vulkan storage-buffer bindings do not
// encode a structured stride, so this preserves the CPU byte ABI while letting
// the shader avoid whole-record OpLoad instructions.
public [[vk::binding(17, 0)]] StructuredBuffer<uint4> gUpt04RigidInstanceWords;
#if UPT04_PACKED_GEOMETRY_VERTEX
public [[vk::binding(18, 0)]] StructuredBuffer<Upt04CompactVertex> gUpt04SkinnedCurrentVertices;
#else
public [[vk::binding(18, 0)]] StructuredBuffer<Upt04SmokeVertex> gUpt04SkinnedCurrentVertices;
#endif
public [[vk::binding(19, 0)]] StructuredBuffer<uint> gUpt04SkinnedSourceIndices;
public [[vk::binding(20, 0)]] StructuredBuffer<uint4> gUpt04SkinnedRouteWords;
public [[vk::binding(21, 0)]] StructuredBuffer<Upt04SkinnedRouteTriangle> gUpt04SkinnedTriangles;
public [[vk::binding(22, 0)]] StructuredBuffer<Upt04EmissiveLookupEntry> gUpt04EmissiveLookup;
public [[vk::binding(32, 0)]] StructuredBuffer<Upt04EmissiveGeometryRecord> gUpt04EmissiveGeometry;

// D0 owns explicit live counts in the first 128 bytes of its control block. T0 deliberately
// keeps camera reprojection in push constants and derives replay bounds from
// the bound resources instead of allocating or uploading another control
// buffer every frame. Current-light plus replay identity checks still gate
// every temporal emissive access.
public uint Upt04GeometryBufferCount<T>(StructuredBuffer<T> buffer)
{
    uint count;
    uint stride;
    buffer.GetDimensions(count, stride);
    return count;
}

public uint Upt04EmissiveGeometryCount()
{
    return Upt04GeometryBufferCount(gUpt04EmissiveGeometry);
}

public uint Upt04StaticVertexCount()
{
#if UPT07_TEMPORAL_GEOMETRY_REPLAY || UPT09_SPATIAL_GEOMETRY_REPLAY
    return Upt04GeometryBufferCount(gUpt04StaticVertices);
#else
    return gUpt04Control.staticVertexCount;
#endif
}

public uint Upt04StaticIndexCount()
{
#if UPT07_TEMPORAL_GEOMETRY_REPLAY || UPT09_SPATIAL_GEOMETRY_REPLAY
    return Upt04GeometryBufferCount(gUpt04StaticIndices);
#else
    return gUpt04Control.staticIndexCount;
#endif
}

public uint Upt04StaticTriangleCount()
{
#if UPT07_TEMPORAL_GEOMETRY_REPLAY || UPT09_SPATIAL_GEOMETRY_REPLAY
    return Upt04GeometryBufferCount(gUpt04StaticTriangleMaterialIndices);
#else
    return gUpt04Control.staticTriangleCount;
#endif
}

public uint Upt04DynamicVertexCount()
{
#if UPT07_TEMPORAL_GEOMETRY_REPLAY || UPT09_SPATIAL_GEOMETRY_REPLAY
    return Upt04GeometryBufferCount(gUpt04DynamicVertices);
#else
    return gUpt04Control.dynamicVertexCount;
#endif
}

public uint Upt04DynamicIndexCount()
{
#if UPT07_TEMPORAL_GEOMETRY_REPLAY || UPT09_SPATIAL_GEOMETRY_REPLAY
    return Upt04GeometryBufferCount(gUpt04DynamicIndices);
#else
    return gUpt04Control.dynamicIndexCount;
#endif
}

public uint Upt04DynamicTriangleCount()
{
#if UPT07_TEMPORAL_GEOMETRY_REPLAY || UPT09_SPATIAL_GEOMETRY_REPLAY
    return Upt04GeometryBufferCount(gUpt04DynamicTriangleMaterialIndices);
#else
    return gUpt04Control.dynamicTriangleCount;
#endif
}

public uint Upt04RigidVertexCount()
{
#if UPT07_TEMPORAL_GEOMETRY_REPLAY || UPT09_SPATIAL_GEOMETRY_REPLAY
    return Upt04GeometryBufferCount(gUpt04RigidVertices);
#else
    return gUpt04Control.rigidVertexCount;
#endif
}

public uint Upt04RigidIndexCount()
{
#if UPT07_TEMPORAL_GEOMETRY_REPLAY || UPT09_SPATIAL_GEOMETRY_REPLAY
    return Upt04GeometryBufferCount(gUpt04RigidIndices);
#else
    return gUpt04Control.rigidIndexCount;
#endif
}

public uint Upt04RigidTriangleCount()
{
#if UPT07_TEMPORAL_GEOMETRY_REPLAY || UPT09_SPATIAL_GEOMETRY_REPLAY
    // Temporal validates the selected route's own count and the bound index
    // buffer. No per-invocation scan of the route table is permitted.
    return 0xffffffffu;
#else
    return gUpt04Control.rigidTriangleCount;
#endif
}

public uint Upt04RigidInstanceCount()
{
#if UPT07_TEMPORAL_GEOMETRY_REPLAY || UPT09_SPATIAL_GEOMETRY_REPLAY
    return Upt04GeometryBufferCount(gUpt04RigidInstanceWords)
        / kUpt04RigidRouteUint4Stride;
#else
    return gUpt04Control.rigidInstanceCount;
#endif
}

public uint Upt04SkinnedRouteRecordCount()
{
#if UPT07_TEMPORAL_GEOMETRY_REPLAY || UPT09_SPATIAL_GEOMETRY_REPLAY
    return Upt04GeometryBufferCount(gUpt04SkinnedRouteWords)
        / kUpt04SkinnedRouteUint4Stride;
#else
    return gUpt04Control.skinnedRouteRecordCount;
#endif
}

public uint Upt04SkinnedRouteTriangleCount()
{
#if UPT07_TEMPORAL_GEOMETRY_REPLAY || UPT09_SPATIAL_GEOMETRY_REPLAY
    return Upt04GeometryBufferCount(gUpt04SkinnedTriangles);
#else
    return gUpt04Control.skinnedRouteTriangleCount;
#endif
}

public uint Upt04SkinnedSourceIndexCount()
{
#if UPT07_TEMPORAL_GEOMETRY_REPLAY || UPT09_SPATIAL_GEOMETRY_REPLAY
    return Upt04GeometryBufferCount(gUpt04SkinnedSourceIndices);
#else
    return gUpt04Control.skinnedSourceIndexCount;
#endif
}

public uint Upt04SkinnedCurrentVertexCount()
{
#if UPT07_TEMPORAL_GEOMETRY_REPLAY || UPT09_SPATIAL_GEOMETRY_REPLAY
    return Upt04GeometryBufferCount(gUpt04SkinnedCurrentVertices);
#else
    return gUpt04Control.skinnedCurrentVertexCount;
#endif
}

public uint Upt04EmissiveReplayCount()
{
    // The structured buffer is authoritative. Keeping a duplicate count in
    // three pass controls allowed their replay contracts to drift and spent a
    // push-constant word that is now used for the shared emissive scale.
    return Upt04GeometryBufferCount(gUpt04EmissiveReplay);
}

public bool Upt04StaticBucketRouteAvailable()
{
#if UPT07_TEMPORAL_GEOMETRY_REPLAY
    return (gUpt07Control.geometryAvailabilityFlags
        & kUpt04RouteStaticBuckets) != 0u;
#elif UPT09_SPATIAL_GEOMETRY_REPLAY
    return (gUpt09Control.geometryAvailabilityFlags
        & kUpt04RouteStaticBuckets) != 0u;
#else
    return (gUpt04Control.availabilityFlags & kUpt04RouteStaticBuckets) != 0u;
#endif
}

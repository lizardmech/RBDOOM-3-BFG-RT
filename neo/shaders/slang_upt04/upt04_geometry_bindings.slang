import upt04_initial_bindings;
import upt04_geometry_contract;
import upt04_light_contract;

#ifndef UPT04_PACKED_GEOMETRY_VERTEX
#define UPT04_PACKED_GEOMETRY_VERTEX 0
#endif

[[vk::binding(6, 0)]] StructuredBuffer<Upt04EmissiveReplayRecord> gUpt04EmissiveReplay;
#if UPT04_PACKED_GEOMETRY_VERTEX
[[vk::binding(7, 0)]] StructuredBuffer<Upt04CompactVertex> gUpt04StaticVertices;
#else
[[vk::binding(7, 0)]] StructuredBuffer<Upt04SmokeVertex> gUpt04StaticVertices;
#endif
[[vk::binding(8, 0)]] StructuredBuffer<uint> gUpt04StaticIndices;
[[vk::binding(9, 0)]] StructuredBuffer<uint> gUpt04StaticTriangleClasses;
[[vk::binding(10, 0)]] StructuredBuffer<uint> gUpt04StaticTriangleMaterialIndices;
#if UPT04_PACKED_GEOMETRY_VERTEX
[[vk::binding(11, 0)]] StructuredBuffer<Upt04CompactVertex> gUpt04DynamicVertices;
#else
[[vk::binding(11, 0)]] StructuredBuffer<Upt04SmokeVertex> gUpt04DynamicVertices;
#endif
[[vk::binding(12, 0)]] StructuredBuffer<uint> gUpt04DynamicIndices;
[[vk::binding(13, 0)]] StructuredBuffer<uint> gUpt04DynamicTriangleClasses;
[[vk::binding(14, 0)]] StructuredBuffer<uint> gUpt04DynamicTriangleMaterialIndices;
#if UPT04_PACKED_GEOMETRY_VERTEX
[[vk::binding(15, 0)]] StructuredBuffer<Upt04CompactVertex> gUpt04RigidVertices;
#else
[[vk::binding(15, 0)]] StructuredBuffer<Upt04SmokeVertex> gUpt04RigidVertices;
#endif
[[vk::binding(16, 0)]] StructuredBuffer<uint> gUpt04RigidIndices;
[[vk::binding(17, 0)]] StructuredBuffer<Upt04RigidRouteInstance> gUpt04RigidInstances;
#if UPT04_PACKED_GEOMETRY_VERTEX
[[vk::binding(18, 0)]] StructuredBuffer<Upt04CompactVertex> gUpt04SkinnedCurrentVertices;
#else
[[vk::binding(18, 0)]] StructuredBuffer<Upt04SmokeVertex> gUpt04SkinnedCurrentVertices;
#endif
[[vk::binding(19, 0)]] StructuredBuffer<uint> gUpt04SkinnedSourceIndices;
[[vk::binding(20, 0)]] StructuredBuffer<Upt04SkinnedRouteRecord> gUpt04SkinnedRoutes;
[[vk::binding(21, 0)]] StructuredBuffer<Upt04SkinnedRouteTriangle> gUpt04SkinnedTriangles;
[[vk::binding(22, 0)]] StructuredBuffer<Upt04EmissiveLookupEntry> gUpt04EmissiveLookup;

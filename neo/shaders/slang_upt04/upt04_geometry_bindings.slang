import upt04_initial_bindings;
import upt04_geometry_contract;

[[vk::binding(6, 0)]] StructuredBuffer<Upt04EmissiveReplayRecord> gUpt04EmissiveReplay;
[[vk::binding(7, 0)]] StructuredBuffer<Upt04SmokeVertex> gUpt04StaticVertices;
[[vk::binding(8, 0)]] StructuredBuffer<uint> gUpt04StaticIndices;
[[vk::binding(9, 0)]] StructuredBuffer<uint> gUpt04StaticTriangleClasses;
[[vk::binding(10, 0)]] StructuredBuffer<uint> gUpt04StaticTriangleMaterialIndices;
[[vk::binding(11, 0)]] StructuredBuffer<Upt04SmokeVertex> gUpt04DynamicVertices;
[[vk::binding(12, 0)]] StructuredBuffer<uint> gUpt04DynamicIndices;
[[vk::binding(13, 0)]] StructuredBuffer<uint> gUpt04DynamicTriangleClasses;
[[vk::binding(14, 0)]] StructuredBuffer<uint> gUpt04DynamicTriangleMaterialIndices;
[[vk::binding(15, 0)]] StructuredBuffer<Upt04SmokeVertex> gUpt04RigidVertices;
[[vk::binding(16, 0)]] StructuredBuffer<uint> gUpt04RigidIndices;
[[vk::binding(17, 0)]] StructuredBuffer<Upt04RigidRouteInstance> gUpt04RigidInstances;
[[vk::binding(18, 0)]] StructuredBuffer<Upt04SmokeVertex> gUpt04SkinnedCurrentVertices;
[[vk::binding(19, 0)]] StructuredBuffer<uint> gUpt04SkinnedSourceIndices;
[[vk::binding(20, 0)]] StructuredBuffer<Upt04SkinnedRouteRecord> gUpt04SkinnedRoutes;
[[vk::binding(21, 0)]] StructuredBuffer<Upt04SkinnedRouteTriangle> gUpt04SkinnedTriangles;

import upt04_geometry_contract;

struct Upt04CompactGeometryPackControl
{
    uint staticVertexCount;
    uint dynamicVertexCount;
    uint rigidVertexCount;
    uint skinnedVertexCount;
};

[[vk::binding(0, 0)]] StructuredBuffer<Upt04SmokeVertex> gStaticSource;
[[vk::binding(1, 0)]] StructuredBuffer<Upt04SmokeVertex> gDynamicSource;
[[vk::binding(2, 0)]] StructuredBuffer<Upt04SmokeVertex> gRigidSource;
[[vk::binding(3, 0)]] StructuredBuffer<Upt04SmokeVertex> gSkinnedSource;
[[vk::binding(4, 0)]] RWStructuredBuffer<Upt04CompactVertex> gStaticCompact;
[[vk::binding(5, 0)]] RWStructuredBuffer<Upt04CompactVertex> gDynamicCompact;
[[vk::binding(6, 0)]] RWStructuredBuffer<Upt04CompactVertex> gRigidCompact;
[[vk::binding(7, 0)]] RWStructuredBuffer<Upt04CompactVertex> gSkinnedCompact;
[[vk::push_constant]] Upt04CompactGeometryPackControl gPackControl;

[shader("compute")]
[numthreads(128, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint vertexIndex = dispatchThreadId.x;
    if (vertexIndex < gPackControl.staticVertexCount)
        gStaticCompact[vertexIndex] = Upt04PackCompactVertex(gStaticSource[vertexIndex]);
    if (vertexIndex < gPackControl.dynamicVertexCount)
        gDynamicCompact[vertexIndex] = Upt04PackCompactVertex(gDynamicSource[vertexIndex]);
    if (vertexIndex < gPackControl.rigidVertexCount)
        gRigidCompact[vertexIndex] = Upt04PackCompactVertex(gRigidSource[vertexIndex]);
    if (vertexIndex < gPackControl.skinnedVertexCount)
        gSkinnedCompact[vertexIndex] = Upt04PackCompactVertex(gSkinnedSource[vertexIndex]);
}

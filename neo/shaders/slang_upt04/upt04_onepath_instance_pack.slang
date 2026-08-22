import upt04_geometry_contract;

struct Upt04OnePathPackControl
{
    uint staticVertexCount;
    uint staticIndexCount;
    uint staticTriangleCount;
    uint dynamicVertexCount;
    uint dynamicIndexCount;
    uint dynamicTriangleCount;
    uint rigidVertexCount;
    uint rigidIndexCount;
    uint rigidInstanceCount;
    uint skinnedVertexCount;
    uint skinnedIndexCount;
    uint skinnedRouteCount;
    uint skinnedTriangleCount;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

[[vk::binding(0, 0)]] StructuredBuffer<uint4> gRigidInstanceWords;
[[vk::binding(1, 0)]] StructuredBuffer<uint4> gSkinnedRouteWords;
[[vk::binding(2, 0)]] StructuredBuffer<Upt04SkinnedRouteTriangle> gSkinnedTriangles;
[[vk::binding(3, 0)]] StructuredBuffer<uint> gStaticMaterials;
[[vk::binding(4, 0)]] StructuredBuffer<uint> gDynamicMaterials;
[[vk::binding(5, 0)]] StructuredBuffer<uint> gStaticClasses;
[[vk::binding(6, 0)]] StructuredBuffer<uint> gDynamicClasses;
[[vk::binding(7, 0)]] RWStructuredBuffer<Upt04OnePathInstanceRecord> gInstances;
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> gConcatMaterials;
[[vk::binding(9, 0)]] RWStructuredBuffer<uint> gConcatClasses;
[[vk::push_constant]] Upt04OnePathPackControl gPack;

public float4 Upt04OnePathIdentityRow(uint row)
{
    if (row == 0u)
        return float4(1.0f, 0.0f, 0.0f, 0.0f);
    if (row == 1u)
        return float4(0.0f, 1.0f, 0.0f, 0.0f);
    return float4(0.0f, 0.0f, 1.0f, 0.0f);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint tid = dispatchThreadId.x;
    const uint instanceCount =
        2u + gPack.rigidInstanceCount + gPack.skinnedRouteCount;
    const uint materialTail = gPack.staticTriangleCount + gPack.dynamicTriangleCount;

    if (tid < instanceCount)
    {
        Upt04OnePathInstanceRecord rec = (Upt04OnePathInstanceRecord)0;
        rec.objectToWorld0 = Upt04OnePathIdentityRow(0u);
        rec.objectToWorld1 = Upt04OnePathIdentityRow(1u);
        rec.objectToWorld2 = Upt04OnePathIdentityRow(2u);
        if (tid == 0u)
        {
            rec.vertexBase = 0u;
            rec.indexBase = 0u;
            rec.triangleCount = gPack.staticTriangleCount;
            rec.materialBase = 0u;
            rec.classBase = 0u;
            rec.vertexCount = gPack.staticVertexCount;
            rec.indexCount = gPack.staticIndexCount;
            rec.flags = kUpt04OnePathClassStatic
                | kUpt04OnePathPerTriangleMaterial
                | kUpt04OnePathPerTriangleClass;
        }
        else if (tid == 1u)
        {
            rec.vertexBase = gPack.staticVertexCount;
            rec.indexBase = gPack.staticIndexCount;
            rec.triangleCount = gPack.dynamicTriangleCount;
            rec.materialBase = gPack.staticTriangleCount;
            rec.classBase = gPack.staticTriangleCount;
            rec.vertexCount = gPack.dynamicVertexCount;
            rec.indexCount = gPack.dynamicIndexCount;
            rec.flags = kUpt04OnePathClassDynamic
                | kUpt04OnePathPerTriangleMaterial
                | kUpt04OnePathPerTriangleClass;
        }
        else if (tid < 2u + gPack.rigidInstanceCount)
        {
            const uint rigidIndex = tid - 2u;
            const uint wordBase = rigidIndex * kUpt04RigidRouteUint4Stride;
            const uint4 offsets = gRigidInstanceWords[wordBase + 0u];
            const uint4 counts = gRigidInstanceWords[wordBase + 1u];
            rec.vertexBase = gPack.staticVertexCount + gPack.dynamicVertexCount
                + offsets.x;
            rec.indexBase = gPack.staticIndexCount + gPack.dynamicIndexCount
                + offsets.y;
            rec.triangleCount = counts.w;
            rec.materialBase = counts.x;
            rec.classBase = 0u;
            rec.vertexCount = counts.y;
            rec.indexCount = counts.z;
            rec.flags = kUpt04OnePathClassRigid | kUpt04OnePathHasTransform;
            rec.objectToWorld0 = asfloat(gRigidInstanceWords[wordBase + 3u]);
            rec.objectToWorld1 = asfloat(gRigidInstanceWords[wordBase + 4u]);
            rec.objectToWorld2 = asfloat(gRigidInstanceWords[wordBase + 5u]);
        }
        else
        {
            const uint skinnedIndex = tid - 2u - gPack.rigidInstanceCount;
            const uint wordBase = skinnedIndex * kUpt04SkinnedRouteUint4Stride;
            const uint4 offsets = gSkinnedRouteWords[wordBase + 0u];
            const uint4 counts = gSkinnedRouteWords[wordBase + 1u];
            rec.vertexBase = gPack.staticVertexCount + gPack.dynamicVertexCount
                + gPack.rigidVertexCount + offsets.z;
            rec.indexBase = gPack.staticIndexCount + gPack.dynamicIndexCount
                + gPack.rigidIndexCount + offsets.y;
            rec.triangleCount = counts.w;
            rec.materialBase = materialTail + counts.x;
            rec.classBase = materialTail + counts.x;
            rec.vertexCount = counts.y;
            rec.indexCount = counts.z;
            rec.flags = kUpt04OnePathClassSkinned
                | kUpt04OnePathPerTriangleMaterial
                | kUpt04OnePathPerTriangleClass;
        }
        gInstances[tid] = rec;
    }

    if (tid < gPack.staticTriangleCount)
    {
        gConcatMaterials[tid] = gStaticMaterials[tid];
        gConcatClasses[tid] = gStaticClasses[tid];
    }
    if (tid < gPack.dynamicTriangleCount)
    {
        const uint dst = gPack.staticTriangleCount + tid;
        gConcatMaterials[dst] = gDynamicMaterials[tid];
        gConcatClasses[dst] = gDynamicClasses[tid];
    }
    if (tid < gPack.skinnedTriangleCount)
    {
        const uint dst = materialTail + tid;
        const Upt04SkinnedRouteTriangle meta = gSkinnedTriangles[tid];
        gConcatMaterials[dst] = meta.materialIndex;
        gConcatClasses[dst] = meta.triangleClassAndFlags;
    }
}

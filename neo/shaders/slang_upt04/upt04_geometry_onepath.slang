import upt04_geometry_contract;
import upt04_geometry_bindings;
import upt04_hit_contract;

public bool Upt04OnePathSelectRecord(
    Upt04HitFacts hit,
    out uint recordIndex,
    out uint primitive)
{
    recordIndex = 0u;
    primitive = 0u;
    if (hit.status != kUpt04HitTriangle || hit.geometryId != 0u)
        return false;
#if UPT_FROZEN_STATIC_GEOMETRY
    if (hit.instanceId != 0u)
        return false;
    recordIndex = 0u;
    primitive = hit.primitiveId;
    return true;
#else
    primitive = hit.primitiveId;
    if (Upt04StaticBucketRouteAvailable()
        && (hit.instanceId & ~kUpt04ShaderInstanceMask) == 0u
        && (hit.instanceId & kUpt04StaticBucketInstanceBit) != 0u)
    {
        recordIndex = 0u;
        primitive += hit.instanceId & kUpt04StaticBucketTriangleMask;
        return true;
    }
    recordIndex = hit.instanceId;
    return true;
#endif
}

public bool Upt04ResolveTriangle(Upt04HitFacts hit, out Upt04ResolvedTriangle triangle)
{
    triangle = (Upt04ResolvedTriangle)0;
    uint recordIndex;
    uint primitive;
    if (!Upt04OnePathSelectRecord(hit, recordIndex, primitive))
        return false;
    uint instanceCount;
    uint instanceStride;
    gUpt04OnePathInstances.GetDimensions(instanceCount, instanceStride);
    if (recordIndex >= instanceCount)
        return false;
    const Upt04OnePathInstanceRecord rec = gUpt04OnePathInstances[recordIndex];
    if (primitive >= rec.triangleCount || rec.triangleCount == 0u)
        return false;
    uint indexCount;
    uint indexStride;
    gUpt04StaticIndices.GetDimensions(indexCount, indexStride);
    if (primitive > (0xffffffffu - 2u) / 3u)
        return false;
    const uint localIndexOffset = primitive * 3u;
    if (localIndexOffset >= rec.indexCount
        || rec.indexCount - localIndexOffset < 3u)
        return false;
    const uint indexOffset = rec.indexBase + localIndexOffset;
    if (indexOffset + 2u >= indexCount)
        return false;
    const uint local0 = gUpt04StaticIndices[indexOffset + 0u];
    const uint local1 = gUpt04StaticIndices[indexOffset + 1u];
    const uint local2 = gUpt04StaticIndices[indexOffset + 2u];
    uint vertexCount;
    uint vertexStride;
    gUpt04StaticVertices.GetDimensions(vertexCount, vertexStride);
    if (local0 >= rec.vertexCount || local1 >= rec.vertexCount
        || local2 >= rec.vertexCount)
        return false;
    const uint v0 = rec.vertexBase + local0;
    const uint v1 = rec.vertexBase + local1;
    const uint v2 = rec.vertexBase + local2;
    if (v0 >= vertexCount || v1 >= vertexCount || v2 >= vertexCount)
        return false;
#if UPT04_PACKED_GEOMETRY_VERTEX
    triangle.vertex0 = Upt04UnpackCompactVertex(gUpt04StaticVertices[v0]);
    triangle.vertex1 = Upt04UnpackCompactVertex(gUpt04StaticVertices[v1]);
    triangle.vertex2 = Upt04UnpackCompactVertex(gUpt04StaticVertices[v2]);
#else
    triangle.vertex0 = gUpt04StaticVertices[v0];
    triangle.vertex1 = gUpt04StaticVertices[v1];
    triangle.vertex2 = gUpt04StaticVertices[v2];
#endif
    if ((rec.flags & kUpt04OnePathHasTransform) != 0u)
    {
        Upt04RigidResolveRoute route = (Upt04RigidResolveRoute)0;
        route.currentObjectToWorld0 = rec.objectToWorld0;
        route.currentObjectToWorld1 = rec.objectToWorld1;
        route.currentObjectToWorld2 = rec.objectToWorld2;
        triangle.vertex0 = Upt04RigidTransformVertex(route, triangle.vertex0);
        triangle.vertex1 = Upt04RigidTransformVertex(route, triangle.vertex1);
        triangle.vertex2 = Upt04RigidTransformVertex(route, triangle.vertex2);
    }
    if ((rec.flags & kUpt04OnePathPerTriangleMaterial) != 0u)
    {
        uint materialCount;
        uint materialStride;
        gUpt04StaticTriangleMaterialIndices.GetDimensions(
            materialCount, materialStride);
        const uint materialIndex = rec.materialBase + primitive;
        if (materialIndex >= materialCount)
            return false;
        triangle.materialIndex =
            gUpt04StaticTriangleMaterialIndices[materialIndex];
    }
    else
        triangle.materialIndex = rec.materialBase;
    if ((rec.flags & kUpt04OnePathPerTriangleClass) != 0u)
    {
        uint classCount;
        uint classStride;
        gUpt04StaticTriangleClasses.GetDimensions(classCount, classStride);
        const uint classIndex = rec.classBase + primitive;
        if (classIndex >= classCount)
            return false;
        triangle.triangleClassAndFlags = gUpt04StaticTriangleClasses[classIndex];
    }
    else
        triangle.triangleClassAndFlags =
            (rec.flags & kUpt04OnePathHasTransform) != 0u
            ? kUpt04RigidSurfaceClass
            : (rec.flags & kUpt04OnePathClassMask);
    triangle.instanceId = hit.instanceId;
    triangle.primitiveIndex = hit.primitiveId;
    triangle.valid = 1u;
    return true;
}

public bool Upt04ResolveTriangleMaterialIndex(
    Upt04HitFacts hit,
    out uint materialIndex)
{
    materialIndex = kUpt04InvalidIdentity;
    uint recordIndex;
    uint primitive;
    if (!Upt04OnePathSelectRecord(hit, recordIndex, primitive))
        return false;
    uint instanceCount;
    uint instanceStride;
    gUpt04OnePathInstances.GetDimensions(instanceCount, instanceStride);
    if (recordIndex >= instanceCount)
        return false;
    const Upt04OnePathInstanceRecord rec = gUpt04OnePathInstances[recordIndex];
    if (primitive >= rec.triangleCount)
        return false;
    if ((rec.flags & kUpt04OnePathPerTriangleMaterial) != 0u)
    {
        uint materialCount;
        uint materialStride;
        gUpt04StaticTriangleMaterialIndices.GetDimensions(
            materialCount, materialStride);
        const uint packedIndex = rec.materialBase + primitive;
        if (packedIndex >= materialCount)
            return false;
        materialIndex = gUpt04StaticTriangleMaterialIndices[packedIndex];
        return true;
    }
    materialIndex = rec.materialBase;
    return true;
}

public bool Upt04ResolveTriangleClassAndFlags(
    Upt04HitFacts hit,
    out uint triangleClassAndFlags)
{
    triangleClassAndFlags = 0u;
    uint recordIndex;
    uint primitive;
    if (!Upt04OnePathSelectRecord(hit, recordIndex, primitive))
        return false;
    uint instanceCount;
    uint instanceStride;
    gUpt04OnePathInstances.GetDimensions(instanceCount, instanceStride);
    if (recordIndex >= instanceCount)
        return false;
    const Upt04OnePathInstanceRecord rec = gUpt04OnePathInstances[recordIndex];
    if (primitive >= rec.triangleCount)
        return false;
    if ((rec.flags & kUpt04OnePathPerTriangleClass) != 0u)
    {
        uint classCount;
        uint classStride;
        gUpt04StaticTriangleClasses.GetDimensions(classCount, classStride);
        const uint packedIndex = rec.classBase + primitive;
        if (packedIndex >= classCount)
            return false;
        triangleClassAndFlags = gUpt04StaticTriangleClasses[packedIndex];
        return true;
    }
    triangleClassAndFlags =
        (rec.flags & kUpt04OnePathHasTransform) != 0u
        ? kUpt04RigidSurfaceClass
        : (rec.flags & kUpt04OnePathClassMask);
    return true;
}

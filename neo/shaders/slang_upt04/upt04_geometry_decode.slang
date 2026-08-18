import upt04_geometry_contract;
import upt04_geometry_bindings;
import upt04_initial_control;
import upt04_hit_contract;

#ifndef UPT_FROZEN_STATIC_SCENE
#define UPT_FROZEN_STATIC_SCENE 0
#endif

#ifndef UPT_FROZEN_STATIC_GEOMETRY
#define UPT_FROZEN_STATIC_GEOMETRY UPT_FROZEN_STATIC_SCENE
#endif
#ifndef UPT04_NO_SKINNED_GEOMETRY
#define UPT04_NO_SKINNED_GEOMETRY 0
#endif

#ifndef UPT04_PACKED_GEOMETRY_VERTEX
#define UPT04_PACKED_GEOMETRY_VERTEX 0
#endif

public float3 Upt04GeometrySafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return isfinite(lengthSquared) && lengthSquared > 1.0e-12f
        ? value * rsqrt(lengthSquared)
        : fallback;
}

public float3 Upt04GeometryConstrainShadingNormal(
    float3 shadingNormal,
    float3 geometricNormal)
{
    const float minGeometryDot = 0.02f;
    geometricNormal = Upt04GeometrySafeNormalize(
        geometricNormal, float3(0.0f, 0.0f, 1.0f));
    shadingNormal = Upt04GeometrySafeNormalize(shadingNormal, geometricNormal);
    const float geometryDot = dot(shadingNormal, geometricNormal);
    if (geometryDot >= minGeometryDot)
        return shadingNormal;

    const float3 tangentComponent =
        shadingNormal - geometricNormal * geometryDot;
    const float tangentLengthSquared = dot(tangentComponent, tangentComponent);
    if (tangentLengthSquared <= 1.0e-8f)
        return geometricNormal;

    const float tangentScale = sqrt(max(
        1.0f - minGeometryDot * minGeometryDot, 0.0f)
        / tangentLengthSquared);
    return Upt04GeometrySafeNormalize(
        tangentComponent * tangentScale
            + geometricNormal * minGeometryDot,
        geometricNormal);
}

public bool Upt04TriangleIndexOffset(uint primitiveIndex, uint indexCount, out uint indexOffset)
{
    indexOffset = 0u;
    if (primitiveIndex > (0xffffffffu - 2u) / 3u)
        return false;
    indexOffset = primitiveIndex * 3u;
    return indexOffset < indexCount && indexCount - indexOffset >= 3u;
}

public bool Upt04LoadIndexedTriangle(
    uint primitiveIndex,
    uint vertexCount,
    uint indexCount,
#if UPT04_PACKED_GEOMETRY_VERTEX
    StructuredBuffer<Upt04CompactVertex> vertices,
#else
    StructuredBuffer<Upt04SmokeVertex> vertices,
#endif
    StructuredBuffer<uint> indices,
    out Upt04SmokeVertex vertex0,
    out Upt04SmokeVertex vertex1,
    out Upt04SmokeVertex vertex2)
{
    vertex0 = (Upt04SmokeVertex)0;
    vertex1 = (Upt04SmokeVertex)0;
    vertex2 = (Upt04SmokeVertex)0;
    uint indexOffset;
    if (!Upt04TriangleIndexOffset(primitiveIndex, indexCount, indexOffset))
        return false;
    const uint index0 = indices[indexOffset + 0u];
    const uint index1 = indices[indexOffset + 1u];
    const uint index2 = indices[indexOffset + 2u];
    if (index0 >= vertexCount || index1 >= vertexCount || index2 >= vertexCount)
        return false;
#if UPT04_PACKED_GEOMETRY_VERTEX
    vertex0 = Upt04UnpackCompactVertex(vertices[index0]);
    vertex1 = Upt04UnpackCompactVertex(vertices[index1]);
    vertex2 = Upt04UnpackCompactVertex(vertices[index2]);
#else
    vertex0 = vertices[index0];
    vertex1 = vertices[index1];
    vertex2 = vertices[index2];
#endif
    return true;
}

public Upt04RigidResolveRoute Upt04LoadRigidResolveRoute(uint routeIndex)
{
    Upt04RigidResolveRoute route = (Upt04RigidResolveRoute)0;
    const uint wordBase = routeIndex * kUpt04RigidRouteUint4Stride;
    const uint4 offsets = gUpt04RigidInstanceWords[wordBase + 0u];
    const uint4 counts = gUpt04RigidInstanceWords[wordBase + 1u];
    route.vertexOffset = offsets.x;
    route.indexOffset = offsets.y;
    route.triangleOffset = offsets.z;
    route.materialIndex = counts.x;
    route.vertexCount = counts.y;
    route.indexCount = counts.z;
    route.triangleCount = counts.w;
    route.currentObjectToWorld0 = asfloat(
        gUpt04RigidInstanceWords[wordBase + 3u]);
    route.currentObjectToWorld1 = asfloat(
        gUpt04RigidInstanceWords[wordBase + 4u]);
    route.currentObjectToWorld2 = asfloat(
        gUpt04RigidInstanceWords[wordBase + 5u]);
    return route;
}

public uint2 Upt04LoadRigidMaterialAndTriangleCount(uint routeIndex)
{
    const uint wordBase = routeIndex * kUpt04RigidRouteUint4Stride;
    const uint4 counts = gUpt04RigidInstanceWords[wordBase + 1u];
    return uint2(counts.x, counts.w);
}

public Upt04SkinnedResolveRoute Upt04LoadSkinnedResolveRoute(uint routeIndex)
{
    Upt04SkinnedResolveRoute route = (Upt04SkinnedResolveRoute)0;
#if UPT04_NO_SKINNED_GEOMETRY
    return route;
#else
    const uint wordBase = routeIndex * kUpt04SkinnedRouteUint4Stride;
    const uint4 offsets = gUpt04SkinnedRouteWords[wordBase + 0u];
    const uint4 counts = gUpt04SkinnedRouteWords[wordBase + 1u];
    const uint4 tableCounts = gUpt04SkinnedRouteWords[wordBase + 4u];
    route.shaderInstanceId = offsets.x;
    route.sourceIndexOffset = offsets.y;
    route.outputVertexOffset = offsets.z;
    route.triangleMetadataOffset = counts.x;
    route.vertexCount = counts.y;
    route.indexCount = counts.z;
    route.triangleCount = counts.w;
    route.routeCount = tableCounts.y;
    route.triangleMetadataCount = tableCounts.z;
    return route;
#endif
}

public float3 Upt04RigidTransformPoint(Upt04RigidResolveRoute route, float3 point)
{
    return float3(
        dot(route.currentObjectToWorld0, float4(point, 1.0f)),
        dot(route.currentObjectToWorld1, float4(point, 1.0f)),
        dot(route.currentObjectToWorld2, float4(point, 1.0f)));
}

public float3 Upt04RigidTransformVector(Upt04RigidResolveRoute route, float3 vector)
{
    return float3(
        dot(route.currentObjectToWorld0.xyz, vector),
        dot(route.currentObjectToWorld1.xyz, vector),
        dot(route.currentObjectToWorld2.xyz, vector));
}

public Upt04SmokeVertex Upt04RigidTransformVertex(
    Upt04RigidResolveRoute route,
    Upt04SmokeVertex vertex)
{
    vertex.position.xyz = Upt04RigidTransformPoint(route, vertex.position.xyz);
    vertex.normal.xyz = Upt04GeometrySafeNormalize(
        Upt04RigidTransformVector(route, vertex.normal.xyz),
        float3(0.0f, 0.0f, 1.0f));
    vertex.tangent.xyz = Upt04GeometrySafeNormalize(
        Upt04RigidTransformVector(route, vertex.tangent.xyz),
        float3(1.0f, 0.0f, 0.0f));
    vertex.bitangent.xyz = Upt04GeometrySafeNormalize(
        Upt04RigidTransformVector(route, vertex.bitangent.xyz),
        float3(0.0f, 1.0f, 0.0f));
    return vertex;
}

public bool Upt04LoadStaticTriangle(
    uint triangleIndex,
    uint identityInstanceId,
    uint identityPrimitiveIndex,
    out Upt04ResolvedTriangle triangle)
{
    triangle = (Upt04ResolvedTriangle)0;
    if (triangleIndex >= Upt04StaticTriangleCount()
        || !Upt04LoadIndexedTriangle(
            triangleIndex,
            Upt04StaticVertexCount(),
            Upt04StaticIndexCount(),
            gUpt04StaticVertices,
            gUpt04StaticIndices,
            triangle.vertex0,
            triangle.vertex1,
            triangle.vertex2))
        return false;
    triangle.materialIndex = gUpt04StaticTriangleMaterialIndices[triangleIndex];
    triangle.triangleClassAndFlags = gUpt04StaticTriangleClasses[triangleIndex];
    triangle.instanceId = identityInstanceId;
    triangle.primitiveIndex = identityPrimitiveIndex;
    triangle.valid = 1u;
    return true;
}

public bool Upt04LoadDynamicTriangle(uint primitiveIndex, out Upt04ResolvedTriangle triangle)
{
    triangle = (Upt04ResolvedTriangle)0;
    if (primitiveIndex >= Upt04DynamicTriangleCount()
        || !Upt04LoadIndexedTriangle(
            primitiveIndex,
            Upt04DynamicVertexCount(),
            Upt04DynamicIndexCount(),
            gUpt04DynamicVertices,
            gUpt04DynamicIndices,
            triangle.vertex0,
            triangle.vertex1,
            triangle.vertex2))
        return false;
    triangle.materialIndex = gUpt04DynamicTriangleMaterialIndices[primitiveIndex];
    triangle.triangleClassAndFlags = gUpt04DynamicTriangleClasses[primitiveIndex];
    triangle.instanceId = 1u;
    triangle.primitiveIndex = primitiveIndex;
    triangle.valid = 1u;
    return true;
}

public bool Upt04LoadRigidTriangle(
    uint instanceId,
    uint primitiveIndex,
    out Upt04ResolvedTriangle triangle)
{
    triangle = (Upt04ResolvedTriangle)0;
    if (instanceId < 2u || instanceId - 2u >= Upt04RigidInstanceCount())
        return false;
    const Upt04RigidResolveRoute route =
        Upt04LoadRigidResolveRoute(instanceId - 2u);
    if (primitiveIndex >= route.triangleCount
        || route.triangleOffset > Upt04RigidTriangleCount()
        || primitiveIndex >= Upt04RigidTriangleCount() - route.triangleOffset
        || primitiveIndex > (0xffffffffu - 2u) / 3u)
        return false;
    const uint localIndexOffset = primitiveIndex * 3u;
    if (localIndexOffset >= route.indexCount || route.indexCount - localIndexOffset < 3u
        || route.indexOffset > Upt04RigidIndexCount()
        || localIndexOffset >= Upt04RigidIndexCount() - route.indexOffset
        || Upt04RigidIndexCount() - route.indexOffset - localIndexOffset < 3u)
        return false;
    const uint indexOffset = route.indexOffset + localIndexOffset;
    const uint localIndex0 = gUpt04RigidIndices[indexOffset + 0u];
    const uint localIndex1 = gUpt04RigidIndices[indexOffset + 1u];
    const uint localIndex2 = gUpt04RigidIndices[indexOffset + 2u];
    if (localIndex0 >= route.vertexCount || localIndex1 >= route.vertexCount || localIndex2 >= route.vertexCount
        || route.vertexOffset > Upt04RigidVertexCount()
        || localIndex0 >= Upt04RigidVertexCount() - route.vertexOffset
        || localIndex1 >= Upt04RigidVertexCount() - route.vertexOffset
        || localIndex2 >= Upt04RigidVertexCount() - route.vertexOffset)
        return false;
#if UPT04_PACKED_GEOMETRY_VERTEX
    triangle.vertex0 = Upt04RigidTransformVertex(route, Upt04UnpackCompactVertex(gUpt04RigidVertices[route.vertexOffset + localIndex0]));
    triangle.vertex1 = Upt04RigidTransformVertex(route, Upt04UnpackCompactVertex(gUpt04RigidVertices[route.vertexOffset + localIndex1]));
    triangle.vertex2 = Upt04RigidTransformVertex(route, Upt04UnpackCompactVertex(gUpt04RigidVertices[route.vertexOffset + localIndex2]));
#else
    triangle.vertex0 = Upt04RigidTransformVertex(route, gUpt04RigidVertices[route.vertexOffset + localIndex0]);
    triangle.vertex1 = Upt04RigidTransformVertex(route, gUpt04RigidVertices[route.vertexOffset + localIndex1]);
    triangle.vertex2 = Upt04RigidTransformVertex(route, gUpt04RigidVertices[route.vertexOffset + localIndex2]);
#endif
    triangle.materialIndex = route.materialIndex;
    triangle.triangleClassAndFlags = kUpt04RigidSurfaceClass;
    triangle.instanceId = instanceId;
    triangle.primitiveIndex = primitiveIndex;
    triangle.valid = 1u;
    return true;
}

public bool Upt04SkinnedInstanceInRange(uint instanceId, out uint routeIndex)
{
    routeIndex = 0u;
#if UPT04_NO_SKINNED_GEOMETRY
    return false;
#else
    const uint routeCount = Upt04SkinnedRouteRecordCount();
    const uint firstInstanceId = 2u + Upt04RigidInstanceCount();
    if (routeCount == 0u || instanceId < firstInstanceId
        || instanceId - firstInstanceId >= routeCount)
        return false;
    routeIndex = instanceId - firstInstanceId;
    return true;
#endif
}

public bool Upt04LoadSkinnedTriangle(
    uint instanceId,
    uint primitiveIndex,
    out Upt04ResolvedTriangle triangle)
{
    triangle = (Upt04ResolvedTriangle)0;
#if UPT04_NO_SKINNED_GEOMETRY
    return false;
#else
    uint routeIndex;
    if (!Upt04SkinnedInstanceInRange(instanceId, routeIndex))
        return false;
    const Upt04SkinnedResolveRoute route =
        Upt04LoadSkinnedResolveRoute(routeIndex);
    if (route.shaderInstanceId != instanceId
        || route.routeCount != Upt04SkinnedRouteRecordCount()
        || route.triangleMetadataCount != Upt04SkinnedRouteTriangleCount()
        || primitiveIndex >= route.triangleCount
        || primitiveIndex > (0xffffffffu - 2u) / 3u)
        return false;
    const uint localIndexOffset = primitiveIndex * 3u;
    if (localIndexOffset >= route.indexCount || route.indexCount - localIndexOffset < 3u
        || route.sourceIndexOffset > Upt04SkinnedSourceIndexCount()
        || localIndexOffset >= Upt04SkinnedSourceIndexCount() - route.sourceIndexOffset
        || Upt04SkinnedSourceIndexCount() - route.sourceIndexOffset - localIndexOffset < 3u
        || route.triangleMetadataOffset > Upt04SkinnedRouteTriangleCount()
        || primitiveIndex >= Upt04SkinnedRouteTriangleCount() - route.triangleMetadataOffset)
        return false;
    const Upt04SkinnedRouteTriangle metadata =
        gUpt04SkinnedTriangles[route.triangleMetadataOffset + primitiveIndex];
    if (metadata.sourcePrimitiveIndex != primitiveIndex)
        return false;
    const uint indexOffset = route.sourceIndexOffset + localIndexOffset;
    const uint localIndex0 = gUpt04SkinnedSourceIndices[indexOffset + 0u];
    const uint localIndex1 = gUpt04SkinnedSourceIndices[indexOffset + 1u];
    const uint localIndex2 = gUpt04SkinnedSourceIndices[indexOffset + 2u];
    if (localIndex0 >= route.vertexCount || localIndex1 >= route.vertexCount || localIndex2 >= route.vertexCount
        || route.outputVertexOffset > Upt04SkinnedCurrentVertexCount()
        || localIndex0 >= Upt04SkinnedCurrentVertexCount() - route.outputVertexOffset
        || localIndex1 >= Upt04SkinnedCurrentVertexCount() - route.outputVertexOffset
        || localIndex2 >= Upt04SkinnedCurrentVertexCount() - route.outputVertexOffset)
        return false;
#if UPT04_PACKED_GEOMETRY_VERTEX
    triangle.vertex0 = Upt04UnpackCompactVertex(gUpt04SkinnedCurrentVertices[route.outputVertexOffset + localIndex0]);
    triangle.vertex1 = Upt04UnpackCompactVertex(gUpt04SkinnedCurrentVertices[route.outputVertexOffset + localIndex1]);
    triangle.vertex2 = Upt04UnpackCompactVertex(gUpt04SkinnedCurrentVertices[route.outputVertexOffset + localIndex2]);
#else
    triangle.vertex0 = gUpt04SkinnedCurrentVertices[route.outputVertexOffset + localIndex0];
    triangle.vertex1 = gUpt04SkinnedCurrentVertices[route.outputVertexOffset + localIndex1];
    triangle.vertex2 = gUpt04SkinnedCurrentVertices[route.outputVertexOffset + localIndex2];
#endif
    triangle.materialIndex = metadata.materialIndex;
    triangle.triangleClassAndFlags = metadata.triangleClassAndFlags;
    triangle.instanceId = instanceId;
    triangle.primitiveIndex = primitiveIndex;
    triangle.valid = 1u;
    return true;
#endif
}

public bool Upt04ResolveTriangle(Upt04HitFacts hit, out Upt04ResolvedTriangle triangle)
{
    triangle = (Upt04ResolvedTriangle)0;
    if (hit.status != kUpt04HitTriangle || hit.geometryId != 0u)
        return false;

#if UPT_FROZEN_STATIC_GEOMETRY
    // The isolated diagnostic TLAS contains exactly the monolithic static
    // BLAS at instance zero. PrimitiveIndex is canonicalized across hardware-
    // opacity chunks before reaching this resolver.
    if (hit.instanceId != 0u)
        return false;
    return Upt04LoadStaticTriangle(
        hit.primitiveId, 0u, hit.primitiveId, triangle);
#else

    const bool staticBucket =
        Upt04StaticBucketRouteAvailable()
        && (hit.instanceId & ~kUpt04ShaderInstanceMask) == 0u
        && (hit.instanceId & kUpt04StaticBucketInstanceBit) != 0u;
    if (staticBucket)
    {
        const uint triangleBase = hit.instanceId & kUpt04StaticBucketTriangleMask;
        if (triangleBase >= Upt04StaticTriangleCount()
            || hit.primitiveId >= Upt04StaticTriangleCount() - triangleBase)
            return false;
        return Upt04LoadStaticTriangle(
            triangleBase + hit.primitiveId,
            hit.instanceId,
            hit.primitiveId,
            triangle);
    }
    if (hit.instanceId == 0u)
        return Upt04LoadStaticTriangle(hit.primitiveId, 0u, hit.primitiveId, triangle);
    if (hit.instanceId == 1u)
        return Upt04LoadDynamicTriangle(hit.primitiveId, triangle);

    uint skinnedRouteIndex;
    if (Upt04SkinnedInstanceInRange(hit.instanceId, skinnedRouteIndex))
        return Upt04LoadSkinnedTriangle(hit.instanceId, hit.primitiveId, triangle);
    return Upt04LoadRigidTriangle(hit.instanceId, hit.primitiveId, triangle);
#endif
}

// Resolve the material indirection without fetching triangle indices or
// vertices. Alpha traversal calls this before the expensive full decode; any
// malformed or stale route fails closed and remains solid.
public bool Upt04ResolveTriangleMaterialIndex(
    Upt04HitFacts hit,
    out uint materialIndex)
{
    materialIndex = kUpt04InvalidIdentity;
    if (hit.status != kUpt04HitTriangle || hit.geometryId != 0u)
        return false;

#if UPT_FROZEN_STATIC_GEOMETRY
    if (hit.instanceId != 0u ||
        hit.primitiveId >= Upt04StaticTriangleCount())
        return false;
    materialIndex =
        gUpt04StaticTriangleMaterialIndices[hit.primitiveId];
    return true;
#else

    const bool staticBucket =
        Upt04StaticBucketRouteAvailable()
        && (hit.instanceId & ~kUpt04ShaderInstanceMask) == 0u
        && (hit.instanceId & kUpt04StaticBucketInstanceBit) != 0u;
    if (staticBucket)
    {
        const uint triangleBase =
            hit.instanceId & kUpt04StaticBucketTriangleMask;
        if (triangleBase >= Upt04StaticTriangleCount()
            || hit.primitiveId >=
                Upt04StaticTriangleCount() - triangleBase)
            return false;
        materialIndex = gUpt04StaticTriangleMaterialIndices[
            triangleBase + hit.primitiveId];
        return true;
    }
    if (hit.instanceId == 0u)
    {
        if (hit.primitiveId >= Upt04StaticTriangleCount())
            return false;
        materialIndex =
            gUpt04StaticTriangleMaterialIndices[hit.primitiveId];
        return true;
    }
    if (hit.instanceId == 1u)
    {
        if (hit.primitiveId >= Upt04DynamicTriangleCount())
            return false;
        materialIndex =
            gUpt04DynamicTriangleMaterialIndices[hit.primitiveId];
        return true;
    }

    uint skinnedRouteIndex;
#if !UPT04_NO_SKINNED_GEOMETRY
    if (Upt04SkinnedInstanceInRange(
            hit.instanceId, skinnedRouteIndex))
    {
        const Upt04SkinnedResolveRoute route =
            Upt04LoadSkinnedResolveRoute(skinnedRouteIndex);
        if (route.shaderInstanceId != hit.instanceId
            || route.routeCount != Upt04SkinnedRouteRecordCount()
            || route.triangleMetadataCount !=
                Upt04SkinnedRouteTriangleCount()
            || hit.primitiveId >= route.triangleCount
            || route.triangleMetadataOffset >
                Upt04SkinnedRouteTriangleCount()
            || hit.primitiveId >=
                Upt04SkinnedRouteTriangleCount()
                    - route.triangleMetadataOffset)
            return false;
        const Upt04SkinnedRouteTriangle metadata =
            gUpt04SkinnedTriangles[
                route.triangleMetadataOffset + hit.primitiveId];
        if (metadata.sourcePrimitiveIndex != hit.primitiveId)
            return false;
        materialIndex = metadata.materialIndex;
        return true;
    }
#endif

    if (hit.instanceId < 2u
        || hit.instanceId - 2u >= Upt04RigidInstanceCount())
        return false;
    const uint2 rigidMaterialAndTriangleCount =
        Upt04LoadRigidMaterialAndTriangleCount(hit.instanceId - 2u);
    if (hit.primitiveId >= rigidMaterialAndTriangleCount.y)
        return false;
    materialIndex = rigidMaterialAndTriangleCount.x;
    return true;
#endif
}

// Resolve only the packed surface class. This is the narrow metadata companion
// to Upt04ResolveTriangleMaterialIndex: visibility traversal needs the
// authoritative translucent subtype but must not fetch indices or vertices.
public bool Upt04ResolveTriangleClassAndFlags(
    Upt04HitFacts hit,
    out uint triangleClassAndFlags)
{
    triangleClassAndFlags = 0u;
    if (hit.status != kUpt04HitTriangle || hit.geometryId != 0u)
        return false;

#if UPT_FROZEN_STATIC_GEOMETRY
    if (hit.instanceId != 0u
        || hit.primitiveId >= Upt04StaticTriangleCount())
        return false;
    triangleClassAndFlags = gUpt04StaticTriangleClasses[hit.primitiveId];
    return true;
#else
    const bool staticBucket =
        Upt04StaticBucketRouteAvailable()
        && (hit.instanceId & ~kUpt04ShaderInstanceMask) == 0u
        && (hit.instanceId & kUpt04StaticBucketInstanceBit) != 0u;
    if (staticBucket)
    {
        const uint triangleBase =
            hit.instanceId & kUpt04StaticBucketTriangleMask;
        if (triangleBase >= Upt04StaticTriangleCount()
            || hit.primitiveId >= Upt04StaticTriangleCount() - triangleBase)
            return false;
        triangleClassAndFlags = gUpt04StaticTriangleClasses[
            triangleBase + hit.primitiveId];
        return true;
    }
    if (hit.instanceId == 0u)
    {
        if (hit.primitiveId >= Upt04StaticTriangleCount())
            return false;
        triangleClassAndFlags = gUpt04StaticTriangleClasses[hit.primitiveId];
        return true;
    }
    if (hit.instanceId == 1u)
    {
        if (hit.primitiveId >= Upt04DynamicTriangleCount())
            return false;
        triangleClassAndFlags = gUpt04DynamicTriangleClasses[hit.primitiveId];
        return true;
    }

    uint skinnedRouteIndex;
#if !UPT04_NO_SKINNED_GEOMETRY
    if (Upt04SkinnedInstanceInRange(hit.instanceId, skinnedRouteIndex))
    {
        const Upt04SkinnedResolveRoute route =
            Upt04LoadSkinnedResolveRoute(skinnedRouteIndex);
        if (route.shaderInstanceId != hit.instanceId
            || route.routeCount != Upt04SkinnedRouteRecordCount()
            || route.triangleMetadataCount != Upt04SkinnedRouteTriangleCount()
            || hit.primitiveId >= route.triangleCount
            || route.triangleMetadataOffset > Upt04SkinnedRouteTriangleCount()
            || hit.primitiveId >= Upt04SkinnedRouteTriangleCount()
                - route.triangleMetadataOffset)
            return false;
        const Upt04SkinnedRouteTriangle metadata = gUpt04SkinnedTriangles[
            route.triangleMetadataOffset + hit.primitiveId];
        if (metadata.sourcePrimitiveIndex != hit.primitiveId)
            return false;
        triangleClassAndFlags = metadata.triangleClassAndFlags;
        return true;
    }
#endif

    if (hit.instanceId < 2u
        || hit.instanceId - 2u >= Upt04RigidInstanceCount())
        return false;
    const uint2 rigidMaterialAndTriangleCount =
        Upt04LoadRigidMaterialAndTriangleCount(hit.instanceId - 2u);
    if (hit.primitiveId >= rigidMaterialAndTriangleCount.y)
        return false;
    triangleClassAndFlags = kUpt04RigidSurfaceClass;
    return true;
#endif
}

public bool Upt04TriangleClassIsTransmittingGlass(uint triangleClassAndFlags)
{
    const uint surfaceClass = triangleClassAndFlags & 0x000000ffu;
    const uint subtype = (triangleClassAndFlags & 0x0f000000u) >> 24u;
    return surfaceClass == 3u && (subtype == 1u || subtype == 4u);
}

public Upt04GeometryHitSurface Upt04BuildGeometryHitSurface(
    Upt04ResolvedTriangle triangle,
    float2 hitBarycentrics,
    uint frontFace)
{
    Upt04GeometryHitSurface surface = (Upt04GeometryHitSurface)0;
    if (triangle.valid == 0u || !all(isfinite(hitBarycentrics)))
        return surface;
    const float3 barycentrics = float3(
        1.0f - hitBarycentrics.x - hitBarycentrics.y,
        hitBarycentrics.x,
        hitBarycentrics.y);
    if (any(barycentrics < -1.0e-4f) || any(barycentrics > 1.0001f))
        return surface;
    const float3 position0 = triangle.vertex0.position.xyz;
    const float3 position1 = triangle.vertex1.position.xyz;
    const float3 position2 = triangle.vertex2.position.xyz;
    float3 geometricNormal = Upt04GeometrySafeNormalize(
        cross(position1 - position0, position2 - position0),
        float3(0.0f, 0.0f, 1.0f));
    // RayQuery/TraceRay already publishes the committed triangle side. Match
    // the accepted legacy secondary-surface adapter: material evaluation and
    // the shadow-ray offset both consume normals oriented toward the incoming
    // continuation, never raw mesh-winding normals.
    if (frontFace == 0u)
        geometricNormal = -geometricNormal;
    float3 shadingNormal = Upt04GeometrySafeNormalize(
        triangle.vertex0.normal.xyz * barycentrics.x
            + triangle.vertex1.normal.xyz * barycentrics.y
            + triangle.vertex2.normal.xyz * barycentrics.z,
        geometricNormal);
    if (dot(shadingNormal, geometricNormal) < 0.0f)
        shadingNormal = -shadingNormal;
    shadingNormal = Upt04GeometryConstrainShadingNormal(
        shadingNormal, geometricNormal);
    const float3 rawTangent = triangle.vertex0.tangent.xyz * barycentrics.x
        + triangle.vertex1.tangent.xyz * barycentrics.y
        + triangle.vertex2.tangent.xyz * barycentrics.z;
    const float3 tangent = Upt04GeometrySafeNormalize(
        rawTangent - shadingNormal * dot(shadingNormal, rawTangent),
        Upt04GeometrySafeNormalize(cross(float3(0.0f, 0.0f, 1.0f), shadingNormal),
            float3(1.0f, 0.0f, 0.0f)));
    const float3 rawBitangent = triangle.vertex0.bitangent.xyz * barycentrics.x
        + triangle.vertex1.bitangent.xyz * barycentrics.y
        + triangle.vertex2.bitangent.xyz * barycentrics.z;
    const float3 bitangent = Upt04GeometrySafeNormalize(
        rawBitangent - shadingNormal * dot(shadingNormal, rawBitangent)
            - tangent * dot(tangent, rawBitangent),
        Upt04GeometrySafeNormalize(cross(shadingNormal, tangent), float3(0.0f, 1.0f, 0.0f)));

    surface.valid = 1u;
    surface.materialIndex = triangle.materialIndex;
    surface.triangleClassAndFlags = triangle.triangleClassAndFlags;
    surface.frontFace = frontFace;
    surface.position = position0 * barycentrics.x + position1 * barycentrics.y + position2 * barycentrics.z;
    surface.geometricNormal = geometricNormal;
    surface.shadingNormal = shadingNormal;
    surface.tangent = tangent;
    surface.bitangent = bitangent;
    surface.texCoord = triangle.vertex0.texCoord.xy * barycentrics.x
        + triangle.vertex1.texCoord.xy * barycentrics.y
        + triangle.vertex2.texCoord.xy * barycentrics.z;
    surface.normalTexCoord = triangle.vertex0.texCoord.zw * barycentrics.x
        + triangle.vertex1.texCoord.zw * barycentrics.y
        + triangle.vertex2.texCoord.zw * barycentrics.z;
    surface.vertexColor = triangle.vertex0.color * barycentrics.x
        + triangle.vertex1.color * barycentrics.y
        + triangle.vertex2.color * barycentrics.z;
    surface.vertexColorAdd = triangle.vertex0.color2 * barycentrics.x
        + triangle.vertex1.color2 * barycentrics.y
        + triangle.vertex2.color2 * barycentrics.z;
    return surface;
}

public Upt04GeometryHitSurface Upt04DecodeGeometryHit(Upt04HitFacts hit)
{
    Upt04ResolvedTriangle triangle;
    if (!Upt04ResolveTriangle(hit, triangle))
        return (Upt04GeometryHitSurface)0;
    return Upt04BuildGeometryHitSurface(triangle, hit.barycentrics, hit.frontFace);
}

import upt45_glass_contract;

[[vk::binding(6, 0)]] StructuredBuffer<uint4> gUpt45StaticVertexWords;
[[vk::binding(7, 0)]] StructuredBuffer<uint> gUpt45StaticIndices;
[[vk::binding(8, 0)]] StructuredBuffer<uint> gUpt45StaticClasses;
[[vk::binding(9, 0)]] StructuredBuffer<uint> gUpt45StaticMaterialIds;
[[vk::binding(10, 0)]] StructuredBuffer<uint> gUpt45StaticMaterialIndices;
[[vk::binding(11, 0)]] StructuredBuffer<uint4> gUpt45DynamicVertexWords;
[[vk::binding(12, 0)]] StructuredBuffer<uint> gUpt45DynamicIndices;
[[vk::binding(13, 0)]] StructuredBuffer<uint> gUpt45DynamicClasses;
[[vk::binding(14, 0)]] StructuredBuffer<uint> gUpt45DynamicMaterialIds;
[[vk::binding(15, 0)]] StructuredBuffer<uint> gUpt45DynamicMaterialIndices;
[[vk::binding(17, 0)]] StructuredBuffer<uint4> gUpt45RigidVertexWords;
[[vk::binding(18, 0)]] StructuredBuffer<uint> gUpt45RigidIndices;
[[vk::binding(19, 0)]] StructuredBuffer<uint4> gUpt45RigidRouteWords;
[[vk::binding(20, 0)]] StructuredBuffer<uint4> gUpt45SkinnedVertexWords;
[[vk::binding(21, 0)]] StructuredBuffer<uint> gUpt45SkinnedIndices;
[[vk::binding(22, 0)]] StructuredBuffer<uint4> gUpt45SkinnedRouteWords;
[[vk::binding(23, 0)]] StructuredBuffer<uint> gUpt45SkinnedTriangleWords;
[[vk::binding(24, 0)]] StructuredBuffer<float4> gUpt45SkinnedPreviousPositions;

public void Upt45LoadVertex(
    StructuredBuffer<uint4> words,
    uint vertexIndex,
    out float3 position,
    out float3 normal,
    out float2 texCoord)
{
    const uint wordBase = vertexIndex * 7u;
    position = asfloat(words[wordBase + 0u].xyz);
    normal = asfloat(words[wordBase + 1u].xyz);
    texCoord = asfloat(words[wordBase + 2u].xy);
}

public float3 Upt45TransformPoint(uint wordBase, float3 value, bool previous)
{
    const uint offset = previous ? 6u : 3u;
    return float3(
        dot(asfloat(gUpt45RigidRouteWords[wordBase + offset + 0u]), float4(value, 1.0f)),
        dot(asfloat(gUpt45RigidRouteWords[wordBase + offset + 1u]), float4(value, 1.0f)),
        dot(asfloat(gUpt45RigidRouteWords[wordBase + offset + 2u]), float4(value, 1.0f)));
}

public float3 Upt45TransformVector(uint wordBase, float3 value)
{
    return float3(
        dot(asfloat(gUpt45RigidRouteWords[wordBase + 3u]).xyz, value),
        dot(asfloat(gUpt45RigidRouteWords[wordBase + 4u]).xyz, value),
        dot(asfloat(gUpt45RigidRouteWords[wordBase + 5u]).xyz, value));
}

public bool Upt45CanonicalPrimitive(
    uint instanceId,
    uint geometryIndex,
    uint primitiveIndex,
    out uint canonicalPrimitive)
{
    canonicalPrimitive = kUpt45Invalid;
    const bool chunked = instanceId <= 1u ||
        ((instanceId & ~kUpt45ShaderInstanceMask) == 0u &&
            (instanceId & kUpt45StaticBucketInstanceBit) != 0u);
    if (!chunked)
    {
        if (geometryIndex != 0u)
            return false;
        canonicalPrimitive = primitiveIndex;
        return true;
    }
    if (primitiveIndex >= kUpt45GeometryChunkTriangles ||
        geometryIndex > (0xffffffffu - primitiveIndex) /
            kUpt45GeometryChunkTriangles)
        return false;
    canonicalPrimitive = geometryIndex * kUpt45GeometryChunkTriangles + primitiveIndex;
    return true;
}

public bool Upt45LoadIndexedNarrow(
    StructuredBuffer<uint4> vertexWords,
    StructuredBuffer<uint> indices,
    uint vertexCount,
    uint indexCount,
    uint triangleIndex,
    out uint3 vertexIndices,
    out float3 p0,
    out float3 p1,
    out float3 p2,
    out float3 n0,
    out float3 n1,
    out float3 n2,
    out float2 uv0,
    out float2 uv1,
    out float2 uv2)
{
    vertexIndices = uint3(0u);
    p0 = p1 = p2 = n0 = n1 = n2 = float3(0.0f);
    uv0 = uv1 = uv2 = float2(0.0f);
    if (triangleIndex > (0xffffffffu - 2u) / 3u)
        return false;
    const uint firstIndex = triangleIndex * 3u;
    if (firstIndex >= indexCount || indexCount - firstIndex < 3u)
        return false;
    vertexIndices = uint3(
        indices[firstIndex + 0u], indices[firstIndex + 1u], indices[firstIndex + 2u]);
    if (any(vertexIndices >= vertexCount))
        return false;
    Upt45LoadVertex(vertexWords, vertexIndices.x, p0, n0, uv0);
    Upt45LoadVertex(vertexWords, vertexIndices.y, p1, n1, uv1);
    Upt45LoadVertex(vertexWords, vertexIndices.z, p2, n2, uv2);
    return all(isfinite(p0)) && all(isfinite(p1)) && all(isfinite(p2));
}

public Upt45NarrowSurface Upt45FinishSurface(
    Upt45NarrowHit hit,
    uint materialIndex,
    uint materialId,
    uint triangleClassAndFlags,
    float3 p0,
    float3 p1,
    float3 p2,
    float3 n0,
    float3 n1,
    float3 n2,
    float2 uv0,
    float2 uv1,
    float2 uv2,
    float3 previous0,
    float3 previous1,
    float3 previous2,
    uint hasPrevious)
{
    Upt45NarrowSurface surface = (Upt45NarrowSurface)0;
    const float3 bary = float3(
        1.0f - hit.barycentrics.x - hit.barycentrics.y,
        hit.barycentrics.x,
        hit.barycentrics.y);
    if (!all(isfinite(bary)) || any(bary < -1.0e-4f) || any(bary > 1.0001f))
        return surface;
    float3 geometricNormal = Upt45SafeNormalize(
        cross(p1 - p0, p2 - p0), float3(0.0f, 0.0f, 1.0f));
    if (hit.frontFace == 0u)
        geometricNormal = -geometricNormal;
    float3 shadingNormal = Upt45SafeNormalize(
        n0 * bary.x + n1 * bary.y + n2 * bary.z, geometricNormal);
    if (dot(shadingNormal, geometricNormal) < 0.0f)
        shadingNormal = -shadingNormal;
    surface.valid = 1u;
    surface.materialIndex = materialIndex;
    surface.materialId = materialId;
    surface.triangleClassAndFlags = triangleClassAndFlags;
    surface.position = p0 * bary.x + p1 * bary.y + p2 * bary.z;
    surface.previousPosition = previous0 * bary.x + previous1 * bary.y + previous2 * bary.z;
    surface.geometricNormal = geometricNormal;
    surface.shadingNormal = shadingNormal;
    surface.texCoord = uv0 * bary.x + uv1 * bary.y + uv2 * bary.z;
    surface.hasPreviousPosition = hasPrevious;
    return surface;
}

public Upt45NarrowSurface Upt45ResolveNarrowHit(Upt45NarrowHit hit, Upt45Control control)
{
    Upt45NarrowSurface invalid = (Upt45NarrowSurface)0;
    uint3 vi;
    float3 p0, p1, p2, n0, n1, n2;
    float2 uv0, uv1, uv2;
    uint materialIndex = kUpt45Invalid;
    uint materialId = kUpt45Invalid;
    uint triangleClass = 0u;
    float3 previous0, previous1, previous2;
    uint hasPrevious = 1u;

    const bool staticBucket =
        (hit.instanceId & ~kUpt45ShaderInstanceMask) == 0u &&
        (hit.instanceId & kUpt45StaticBucketInstanceBit) != 0u;
    if (staticBucket || hit.instanceId == 0u)
    {
        const uint triangleIndex = staticBucket
            ? (hit.instanceId & kUpt45StaticBucketTriangleMask) + hit.primitiveId
            : hit.primitiveId;
        if (triangleIndex >= control.staticTriangleCount ||
            !Upt45LoadIndexedNarrow(
                gUpt45StaticVertexWords, gUpt45StaticIndices,
                control.staticVertexCount, control.staticIndexCount,
                triangleIndex, vi, p0, p1, p2, n0, n1, n2, uv0, uv1, uv2))
            return invalid;
        materialIndex = gUpt45StaticMaterialIndices[triangleIndex];
        materialId = gUpt45StaticMaterialIds[triangleIndex];
        triangleClass = gUpt45StaticClasses[triangleIndex];
        previous0 = p0; previous1 = p1; previous2 = p2;
    }
    else if (hit.instanceId == 1u)
    {
        if (hit.primitiveId >= control.dynamicTriangleCount ||
            !Upt45LoadIndexedNarrow(
                gUpt45DynamicVertexWords, gUpt45DynamicIndices,
                control.dynamicVertexCount, control.dynamicIndexCount,
                hit.primitiveId, vi, p0, p1, p2, n0, n1, n2, uv0, uv1, uv2))
            return invalid;
        materialIndex = gUpt45DynamicMaterialIndices[hit.primitiveId];
        materialId = gUpt45DynamicMaterialIds[hit.primitiveId];
        triangleClass = gUpt45DynamicClasses[hit.primitiveId];
        previous0 = p0; previous1 = p1; previous2 = p2;
        hasPrevious = 0u;
    }
    else
    {
        const uint skinnedFirst = 2u + control.rigidInstanceCount;
        if (hit.instanceId >= skinnedFirst &&
            hit.instanceId - skinnedFirst < control.skinnedRouteCount)
        {
            const uint routeIndex = hit.instanceId - skinnedFirst;
            const uint wordBase = routeIndex * 5u;
            const uint4 offsets = gUpt45SkinnedRouteWords[wordBase + 0u];
            const uint4 counts = gUpt45SkinnedRouteWords[wordBase + 1u];
            const uint4 metadataCounts = gUpt45SkinnedRouteWords[wordBase + 4u];
            if (offsets.x != hit.instanceId || hit.primitiveId >= counts.w ||
                metadataCounts.y != control.skinnedRouteCount ||
                metadataCounts.z != control.skinnedTriangleCount ||
                hit.primitiveId > (0xffffffffu - 2u) / 3u)
                return invalid;
            const uint firstIndex = offsets.y + hit.primitiveId * 3u;
            if (firstIndex >= control.skinnedIndexCount ||
                control.skinnedIndexCount - firstIndex < 3u)
                return invalid;
            const uint3 localIndices = uint3(
                gUpt45SkinnedIndices[firstIndex + 0u],
                gUpt45SkinnedIndices[firstIndex + 1u],
                gUpt45SkinnedIndices[firstIndex + 2u]);
            if (any(localIndices >= counts.y))
                return invalid;
            vi = offsets.z + localIndices;
            if (any(vi >= control.skinnedVertexCount))
                return invalid;
            Upt45LoadVertex(gUpt45SkinnedVertexWords, vi.x, p0, n0, uv0);
            Upt45LoadVertex(gUpt45SkinnedVertexWords, vi.y, p1, n1, uv1);
            Upt45LoadVertex(gUpt45SkinnedVertexWords, vi.z, p2, n2, uv2);
            const uint metadataWord = (counts.x + hit.primitiveId) * 9u;
            if (metadataWord + 4u >= control.skinnedTriangleCount * 9u ||
                gUpt45SkinnedTriangleWords[metadataWord + 0u] != hit.primitiveId)
                return invalid;
            materialId = gUpt45SkinnedTriangleWords[metadataWord + 2u];
            materialIndex = gUpt45SkinnedTriangleWords[metadataWord + 3u];
            triangleClass = gUpt45SkinnedTriangleWords[metadataWord + 4u];
            const bool previousValid = (gUpt45SkinnedRouteWords[wordBase + 2u].x & 1u) != 0u &&
                offsets.w != kUpt45Invalid &&
                offsets.w + max(localIndices.x, max(localIndices.y, localIndices.z)) <
                    control.skinnedPreviousCount;
            if (previousValid)
            {
                previous0 = gUpt45SkinnedPreviousPositions[offsets.w + localIndices.x].xyz;
                previous1 = gUpt45SkinnedPreviousPositions[offsets.w + localIndices.y].xyz;
                previous2 = gUpt45SkinnedPreviousPositions[offsets.w + localIndices.z].xyz;
            }
            else
            {
                previous0 = p0; previous1 = p1; previous2 = p2;
                hasPrevious = 0u;
            }
        }
        else
        {
            if (hit.instanceId < 2u || hit.instanceId - 2u >= control.rigidInstanceCount)
                return invalid;
            const uint routeIndex = hit.instanceId - 2u;
            const uint wordBase = routeIndex * 9u;
            const uint4 offsets = gUpt45RigidRouteWords[wordBase + 0u];
            const uint4 counts = gUpt45RigidRouteWords[wordBase + 1u];
            const uint4 identityAndFlags = gUpt45RigidRouteWords[wordBase + 2u];
            if (hit.primitiveId >= counts.w || hit.primitiveId > (0xffffffffu - 2u) / 3u)
                return invalid;
            const uint firstIndex = offsets.y + hit.primitiveId * 3u;
            if (firstIndex >= control.rigidIndexCount ||
                control.rigidIndexCount - firstIndex < 3u)
                return invalid;
            const uint3 localIndices = uint3(
                gUpt45RigidIndices[firstIndex + 0u],
                gUpt45RigidIndices[firstIndex + 1u],
                gUpt45RigidIndices[firstIndex + 2u]);
            if (any(localIndices >= counts.y))
                return invalid;
            vi = offsets.x + localIndices;
            if (any(vi >= control.rigidVertexCount))
                return invalid;
            Upt45LoadVertex(gUpt45RigidVertexWords, vi.x, p0, n0, uv0);
            Upt45LoadVertex(gUpt45RigidVertexWords, vi.y, p1, n1, uv1);
            Upt45LoadVertex(gUpt45RigidVertexWords, vi.z, p2, n2, uv2);
            const float3 local0 = p0, local1 = p1, local2 = p2;
            p0 = Upt45TransformPoint(wordBase, local0, false);
            p1 = Upt45TransformPoint(wordBase, local1, false);
            p2 = Upt45TransformPoint(wordBase, local2, false);
            n0 = Upt45TransformVector(wordBase, n0);
            n1 = Upt45TransformVector(wordBase, n1);
            n2 = Upt45TransformVector(wordBase, n2);
            const bool previousValid = (identityAndFlags.x & 3u) == 3u;
            previous0 = previousValid ? Upt45TransformPoint(wordBase, local0, true) : p0;
            previous1 = previousValid ? Upt45TransformPoint(wordBase, local1, true) : p1;
            previous2 = previousValid ? Upt45TransformPoint(wordBase, local2, true) : p2;
            hasPrevious = previousValid ? 1u : 0u;
            materialId = offsets.w;
            materialIndex = counts.x;
            triangleClass = 1u;
        }
    }
    return Upt45FinishSurface(
        hit, materialIndex, materialId, triangleClass,
        p0, p1, p2, n0, n1, n2, uv0, uv1, uv2,
        previous0, previous1, previous2, hasPrevious);
}

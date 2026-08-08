import upt04_geometry_contract;
import upt04_geometry_bindings;
import upt04_geometry_decode;
import upt04_light_contract;
import upt04_hit_contract;

// Shared replay policy bit. Each owner supplies this bit from its own control
// block; this module is imported by D0, T0 and S0 and must not capture one
// pass's globals.
public static const uint kUpt04EmissiveReplayTwoSided = 0x00000040u;

public Upt04EmissiveGeometrySample Upt04ReplayEmissiveTriangleBarycentrics(
    Upt04UnifiedLightRecord light,
    float3 receiverPosition,
    float2 barycentrics,
    bool twoSidedEmissives)
{
    Upt04EmissiveGeometrySample sample = (Upt04EmissiveGeometrySample)0;
    if (light.type != kUpt04LightEmissiveTriangle
        || light.sourceIndex >= Upt04EmissiveReplayCount()
        || !all(isfinite(receiverPosition))
        || !all(isfinite(barycentrics)) || any(barycentrics < 0.0f)
        || barycentrics.x + barycentrics.y > 1.0001f)
        return sample;

    const Upt04EmissiveReplayRecord replay = gUpt04EmissiveReplay[light.sourceIndex];
    if (replay.instanceId != light.instanceId
        || replay.primitiveIndex != light.primitiveIndex
        || replay.materialIndex != light.materialOrLightId
        || replay.identityHashLo != light.identityA
        || replay.identityHashHi != light.identityB)
        return sample;

    Upt04HitFacts replayHit = Upt04MissFacts(0.0f);
    replayHit.status = kUpt04HitTriangle;
    replayHit.instanceId = replay.instanceId;
    replayHit.geometryId = 0u;
    replayHit.primitiveId = replay.primitiveIndex;
    replayHit.barycentrics = barycentrics;
    replayHit.frontFace = 1u;

    Upt04ResolvedTriangle triangle;
    if (!Upt04ResolveTriangle(replayHit, triangle)
        || triangle.materialIndex != replay.materialIndex)
        return sample;
    const Upt04GeometryHitSurface surface = Upt04BuildGeometryHitSurface(
        triangle,
        replayHit.barycentrics,
        1u);
    if (surface.valid == 0u)
        return sample;

    const float3 edge0 = triangle.vertex1.position.xyz - triangle.vertex0.position.xyz;
    const float3 edge1 = triangle.vertex2.position.xyz - triangle.vertex0.position.xyz;
    const float3 areaVector = cross(edge0, edge1);
    const float doubleAreaSquared = dot(areaVector, areaVector);
    if (!isfinite(doubleAreaSquared) || doubleAreaSquared <= 1.0e-12f)
        return sample;
    const float doubleArea = sqrt(doubleAreaSquared);
    const float area = 0.5f * doubleArea;
    float3 normal = areaVector / doubleArea;
    const float3 publishedNormal = Upt04GeometrySafeNormalize(
        light.normalAndArea.xyz,
        normal);
    if (dot(normal, publishedNormal) < 0.0f)
        normal = -normal;

    const float3 toLight = surface.position - receiverPosition;
    const float distanceSquared = dot(toLight, toLight);
    if (!isfinite(distanceSquared) || distanceSquared <= 1.0e-8f)
        return sample;
    const float distance = sqrt(distanceSquared);
    const float3 direction = toLight / distance;
    const float lightFacingRaw = dot(normal, -direction);
    const float lightFacing = twoSidedEmissives
        ? abs(lightFacingRaw)
        : saturate(lightFacingRaw);
    const float boundedFacing = max(lightFacing, 1.0e-8f);
    const float solidAnglePdf = distanceSquared / max(area * boundedFacing, 1.0e-8f);
    if (!Upt04FinitePositive(solidAnglePdf))
        return sample;

    sample.status = lightFacing > 0.0f
        ? kUpt04SampleValidPositive
        : kUpt04SampleValidZero;
    sample.materialIndex = triangle.materialIndex;
    sample.triangleClassAndFlags = triangle.triangleClassAndFlags;
    sample.replayIndex = light.sourceIndex;
    sample.position = surface.position;
    sample.normal = normal;
    sample.direction = direction;
    sample.distance = distance;
    sample.solidAnglePdf = solidAnglePdf;
    sample.texCoord = surface.texCoord;

    // Keep the exact endpoint UV and the triangle UV basis.  The caller uses
    // the basis to build a bounded four-region texture-importance proposal,
    // then evaluates radiance at the exact sampled endpoint.  A single
    // centroid footprint is not a valid emitted-radiance value for Doom
    // materials: a small indicator panel often occupies only a few percent of
    // a much larger wall/object triangle, and assigning its centroid colour to
    // every point turns the non-emitting remainder into a huge ghost light.
    const float2 uv0 = triangle.vertex0.texCoord.xy;
    const float2 uv1 = triangle.vertex1.texCoord.xy;
    const float2 uv2 = triangle.vertex2.texCoord.xy;
    sample.emissionTexCoord = surface.texCoord;
    sample.emissionUvGradient0 = uv1 - uv0;
    sample.emissionUvGradient1 = uv2 - uv0;
    if (!all(isfinite(sample.emissionTexCoord))
        || !all(isfinite(sample.emissionUvGradient0))
        || !all(isfinite(sample.emissionUvGradient1)))
    {
        sample.emissionTexCoord = surface.texCoord;
        sample.emissionUvGradient0 = float2(0.0f);
        sample.emissionUvGradient1 = float2(0.0f);
    }
    sample.barycentrics = replayHit.barycentrics;
    return sample;
}

// Four equal-area subtriangles formed by the three edge midpoints.  The
// region id is recoverable from the stored barycentrics, so T0/S0 can rebuild
// the same conditional proposal PDF without widening the persistent ABI.
public uint Upt04EmissiveRegionFromBarycentrics(float2 barycentrics)
{
    const float3 bary = float3(
        1.0f - barycentrics.x - barycentrics.y,
        barycentrics.x,
        barycentrics.y);
    if (bary.x >= 0.5f)
        return 0u;
    if (bary.y >= 0.5f)
        return 1u;
    if (bary.z >= 0.5f)
        return 2u;
    return 3u;
}

public float2 Upt04EmissiveRegionProbeBarycentrics(uint region)
{
    if (region == 0u)
        return float2(1.0f / 6.0f, 1.0f / 6.0f);
    if (region == 1u)
        return float2(2.0f / 3.0f, 1.0f / 6.0f);
    if (region == 2u)
        return float2(1.0f / 6.0f, 2.0f / 3.0f);
    return float2(1.0f / 3.0f, 1.0f / 3.0f);
}

public float2 Upt04EmissiveUvAtBarycentrics(
    Upt04EmissiveGeometrySample sample,
    float2 barycentrics)
{
    const float2 uv0 = sample.emissionTexCoord
        - sample.barycentrics.x * sample.emissionUvGradient0
        - sample.barycentrics.y * sample.emissionUvGradient1;
    return uv0
        + barycentrics.x * sample.emissionUvGradient0
        + barycentrics.y * sample.emissionUvGradient1;
}

public float2 Upt04SampleEmissiveRegionBarycentrics(
    uint region,
    float2 random)
{
    const float squareRootU = sqrt(saturate(random.x));
    const float local1 = saturate(random.y) * squareRootU;
    const float local2 = squareRootU - local1;
    const float local0 = 1.0f - squareRootU;
    if (region == 0u)
        return float2(0.5f * local1, 0.5f * local2);
    if (region == 1u)
        return float2(0.5f + 0.5f * local0, 0.5f * local2);
    if (region == 2u)
        return float2(0.5f * local2, 0.5f + 0.5f * local0);
    return float2(
        0.5f * (local0 + local1),
        0.5f * (local1 + local2));
}

public float Upt04EmissiveRegionSelectionPdf(float4 weights, uint region)
{
    // Preserve support in every region even when the four coarse footprint
    // probes miss a tiny emissive texel.  The exact endpoint evaluation keeps
    // the estimator unbiased; this mixture only changes where work is spent.
    const float uniformMixture = 0.125f;
    const float weightSum = weights.x + weights.y + weights.z + weights.w;
    const float normalized = isfinite(weightSum) && weightSum > 1.0e-8f
        ? max(weights[min(region, 3u)], 0.0f) / weightSum
        : 0.25f;
    return uniformMixture * 0.25f + (1.0f - uniformMixture) * normalized;
}

public uint Upt04SelectEmissiveRegion(float4 weights, float random)
{
    float threshold = saturate(random);
    [unroll]
    for (uint region = 0u; region < 3u; ++region)
    {
        const float probability = Upt04EmissiveRegionSelectionPdf(
            weights, region);
        if (threshold < probability)
            return region;
        threshold -= probability;
    }
    return 3u;
}

public Upt04EmissiveGeometrySample Upt04ReplayEmissiveTriangleGeometry(
    Upt04UnifiedLightRecord light,
    float3 receiverPosition,
    float2 random,
    bool twoSidedEmissives)
{
    if (!all(isfinite(random)) || any(random < 0.0f) || any(random >= 1.0f))
        return (Upt04EmissiveGeometrySample)0;
    const float squareRootU = sqrt(random.x);
    const float barycentric1 = random.y * squareRootU;
    const float barycentric2 = squareRootU - barycentric1;
    return Upt04ReplayEmissiveTriangleBarycentrics(
        light,
        receiverPosition,
        float2(barycentric1, barycentric2),
        twoSidedEmissives);
}

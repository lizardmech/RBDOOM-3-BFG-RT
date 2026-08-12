import upt04_geometry_contract;
import upt04_geometry_bindings;
import upt04_geometry_decode;
import upt04_light_contract;
import upt04_hit_contract;

// Shared replay policy bit. Each owner supplies this bit from its own control
// block; this module is imported by D0, T0 and S0 and must not capture one
// pass's globals.
public static const uint kUpt04EmissiveReplayTwoSided = 0x00000040u;
public static const uint kUpt04EmissiveGeometryValid = 0x47505545u;

public Upt04EmissiveGeometrySample Upt04ReplayEmissiveTriangleBarycentrics(
    Upt04UnifiedLightRecord light,
    float3 receiverPosition,
    float2 barycentrics,
    bool twoSidedEmissives)
{
    Upt04EmissiveGeometrySample sample = (Upt04EmissiveGeometrySample)0;
    if (light.type != kUpt04LightEmissiveTriangle
        || light.sourceIndex >= Upt04EmissiveReplayCount()
        || light.sourceIndex >= Upt04EmissiveGeometryCount()
        || !all(isfinite(receiverPosition))
        || !all(isfinite(barycentrics)) || any(barycentrics < 0.0f)
        || barycentrics.x + barycentrics.y > 1.0001f)
        return sample;

    const Upt04EmissiveReplayRecord replay = gUpt04EmissiveReplay[light.sourceIndex];
    const Upt04EmissiveGeometryRecord geometry = gUpt04EmissiveGeometry[light.sourceIndex];
    if (replay.instanceId != light.instanceId
        || replay.primitiveIndex != light.primitiveIndex
        || replay.materialIndex != light.materialOrLightId
        || replay.identityHashLo != light.identityA
        || replay.identityHashHi != light.identityB
        || geometry.validity != kUpt04EmissiveGeometryValid
        || geometry.identityHashLo != light.identityA
        || geometry.identityHashHi != light.identityB)
        return sample;

    const float3 position0 = geometry.position0AndUv0X.xyz;
    const float3 position1 = geometry.position1AndUv0Y.xyz;
    const float3 position2 = geometry.position2AndUv1X.xyz;
    const float2 uv0 = float2(
        geometry.position0AndUv0X.w,
        geometry.position1AndUv0Y.w);
    const float2 uv1 = float2(
        geometry.position2AndUv1X.w,
        geometry.uv1Y);
    const float2 uv2 = 3.0f * replay.centroidUvAndWeight.xy - uv0 - uv1;
    const float barycentric0 = 1.0f - barycentrics.x - barycentrics.y;
    const float3 surfacePosition =
        position0 * barycentric0
        + position1 * barycentrics.x
        + position2 * barycentrics.y;
    const float2 surfaceTexCoord =
        uv0 * barycentric0
        + uv1 * barycentrics.x
        + uv2 * barycentrics.y;
    if (!all(isfinite(surfacePosition)) || !all(isfinite(surfaceTexCoord)))
        return sample;

    const float3 edge0 = position1 - position0;
    const float3 edge1 = position2 - position0;
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

    const float3 toLight = surfacePosition - receiverPosition;
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
    sample.materialIndex = replay.materialIndex;
    sample.triangleClassAndFlags = replay.padding0;
    sample.replayIndex = light.sourceIndex;
    sample.position = surfacePosition;
    sample.normal = normal;
    sample.direction = direction;
    sample.distance = distance;
    sample.solidAnglePdf = solidAnglePdf;
    sample.texCoord = surfaceTexCoord;

    // Keep the exact endpoint UV and the triangle UV basis.  The caller uses
    // the basis to build a bounded four-region texture-importance proposal,
    // then evaluates radiance at the exact sampled endpoint.  A single
    // centroid footprint is not a valid emitted-radiance value for Doom
    // materials: a small indicator panel often occupies only a few percent of
    // a much larger wall/object triangle, and assigning its centroid colour to
    // every point turns the non-emitting remainder into a huge ghost light.
    sample.emissionTexCoord = surfaceTexCoord;
    sample.emissionUvGradient0 = uv1 - uv0;
    sample.emissionUvGradient1 = uv2 - uv0;
    if (!all(isfinite(sample.emissionTexCoord))
        || !all(isfinite(sample.emissionUvGradient0))
        || !all(isfinite(sample.emissionUvGradient1)))
    {
        sample.emissionTexCoord = surfaceTexCoord;
        sample.emissionUvGradient0 = float2(0.0f);
        sample.emissionUvGradient1 = float2(0.0f);
    }
    sample.barycentrics = barycentrics;
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

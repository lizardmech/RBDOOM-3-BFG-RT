import upt04_light_contract;
import upt04_binary16;
import upt04_random_white_hash;

public static const uint kUpt56ResolvedEmissiveValid = 1u;
public static const uint kUpt56ResolvedTwoSided = 0x00000040u;

public struct Upt56ResolvedEmissiveRecord
{
    public float4 p0AndArea;
    public float4 e1AndPad;
    public float4 e2AndPad;
    public float4 nAndFlags;
    public float4 radiance0;
    public float4 radiance1;
    public float4 radiance2;
    public float4 radiance3;
};

public float2 Upt56RegionProbeBarycentrics(uint region)
{
    if (region == 0u)
        return float2(1.0f / 6.0f, 1.0f / 6.0f);
    if (region == 1u)
        return float2(2.0f / 3.0f, 1.0f / 6.0f);
    if (region == 2u)
        return float2(1.0f / 6.0f, 2.0f / 3.0f);
    return float2(1.0f / 3.0f, 1.0f / 3.0f);
}

public uint Upt56RegionFromBarycentrics(float2 barycentrics)
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

public float2 Upt56SampleRegionBarycentrics(uint region, float2 random)
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

public float Upt56RegionSelectionPdf(float4 weights, uint region)
{
    const float uniformMixture = 0.125f;
    const float weightSum = weights.x + weights.y + weights.z + weights.w;
    const float normalized = isfinite(weightSum) && weightSum > 1.0e-8f
        ? max(weights[min(region, 3u)], 0.0f) / weightSum
        : 0.25f;
    return uniformMixture * 0.25f + (1.0f - uniformMixture) * normalized;
}

public uint Upt56SelectRegion(float4 weights, float random)
{
    float threshold = saturate(random);
    [unroll]
    for (uint region = 0u; region < 3u; ++region)
    {
        const float probability = Upt56RegionSelectionPdf(weights, region);
        if (threshold < probability)
            return region;
        threshold -= probability;
    }
    return 3u;
}

public float3 Upt56ResolvedRegionRadiance(
    Upt56ResolvedEmissiveRecord resolved,
    uint region)
{
    if (region == 0u)
        return resolved.radiance0.xyz;
    if (region == 1u)
        return resolved.radiance1.xyz;
    if (region == 2u)
        return resolved.radiance2.xyz;
    return resolved.radiance3.xyz;
}

public bool Upt56ResolvedRecordValid(Upt56ResolvedEmissiveRecord resolved)
{
    return (asuint(resolved.nAndFlags.w) & kUpt56ResolvedEmissiveValid) != 0u
        && Upt04FinitePositive(resolved.p0AndArea.w);
}

public Upt04EvaluatedLocalLight Upt56EvaluateResolvedEmissive(
    Upt56ResolvedEmissiveRecord resolved,
    Upt04UnifiedLightRecord light,
    float3 receiverPosition,
    float2 sampleCoordinates,
    bool twoSided)
{
    Upt04EvaluatedLocalLight result = (Upt04EvaluatedLocalLight)0;
    result.storedSampleCoordinates = sampleCoordinates;
    if (!Upt56ResolvedRecordValid(resolved)
        || !all(isfinite(receiverPosition)))
        return result;

    const float4 regionWeights = light.positionAndRadius;
    if (!all(isfinite(regionWeights)) || any(regionWeights < 0.0f))
        return result;

    uint regionSeed = Upt04HashWord(
        asuint(sampleCoordinates.x)
        ^ Upt04HashWord(asuint(sampleCoordinates.y))
        ^ light.identityA ^ (light.identityB * 0x9e3779b9u));
    const float regionRandom = float(regionSeed >> 8u) * (1.0f / 16777216.0f);
    const uint region = Upt56SelectRegion(regionWeights, regionRandom);
    const float2 barycentrics = Upt56SampleRegionBarycentrics(
        region, sampleCoordinates);
    const float3 p0 = resolved.p0AndArea.xyz;
    const float3 e1 = resolved.e1AndPad.xyz;
    const float3 e2 = resolved.e2AndPad.xyz;
    const float area = resolved.p0AndArea.w;
    float3 normal = resolved.nAndFlags.xyz;
    const float3 position = p0 + e1 * barycentrics.x + e2 * barycentrics.y;
    const float3 toLight = position - receiverPosition;
    const float distanceSquared = dot(toLight, toLight);
    if (!isfinite(distanceSquared) || distanceSquared <= 1.0e-8f
        || !Upt04FinitePositive(area))
        return result;
    const float distance = sqrt(distanceSquared);
    const float3 direction = toLight / distance;
    const float3 publishedNormal = light.normalAndArea.xyz;
    if (dot(normal, publishedNormal) < 0.0f)
        normal = -normal;
    const float lightFacingRaw = dot(normal, -direction);
    const float lightFacing = twoSided
        ? abs(lightFacingRaw)
        : saturate(lightFacingRaw);
    const float solidAnglePdf = distanceSquared
        / max(area * max(lightFacing, 1.0e-8f), 1.0e-8f);
    if (!Upt04FinitePositive(solidAnglePdf))
        return result;

    const float3 radiance = max(
        Upt56ResolvedRegionRadiance(resolved, region),
        float3(0.0f));
    const float regionPdf = 4.0f * Upt56RegionSelectionPdf(regionWeights, region);
    result.sample.status = lightFacing > 0.0f && any(radiance > 0.0f)
        ? kUpt04SampleValidPositive
        : kUpt04SampleValidZero;
    result.sample.position = position;
    result.sample.normal = normal;
    result.sample.distance = distance;
    result.sample.direction = direction;
    result.sample.solidAnglePdf = solidAnglePdf * regionPdf;
    result.sample.reserved = asuint(regionPdf);
    result.sample.radiance = radiance;
    result.storedSampleCoordinates = barycentrics;
    result.valid = 1u;
    return result;
}

public float3 Upt56ResolvedRadianceAtBarycentrics(
    Upt56ResolvedEmissiveRecord resolved,
    float2 barycentrics)
{
    if (!Upt56ResolvedRecordValid(resolved))
        return float3(0.0f);
    return max(
        Upt56ResolvedRegionRadiance(
            resolved, Upt56RegionFromBarycentrics(barycentrics)),
        float3(0.0f));
}

public float Upt56ResolvedSolidAnglePdf(
    Upt56ResolvedEmissiveRecord resolved,
    Upt04UnifiedLightRecord light,
    float3 receiverPosition,
    float2 barycentrics,
    bool twoSided)
{
    if (!Upt56ResolvedRecordValid(resolved)
        || !all(isfinite(receiverPosition))
        || !all(isfinite(barycentrics)))
        return 0.0f;
    const float3 p0 = resolved.p0AndArea.xyz;
    const float3 e1 = resolved.e1AndPad.xyz;
    const float3 e2 = resolved.e2AndPad.xyz;
    const float area = resolved.p0AndArea.w;
    float3 normal = resolved.nAndFlags.xyz;
    const float3 position = p0 + e1 * barycentrics.x + e2 * barycentrics.y;
    const float3 toLight = position - receiverPosition;
    const float distanceSquared = dot(toLight, toLight);
    if (!isfinite(distanceSquared) || distanceSquared <= 1.0e-8f
        || !Upt04FinitePositive(area))
        return 0.0f;
    const float distance = sqrt(distanceSquared);
    const float3 direction = toLight / distance;
    const float3 publishedNormal = light.normalAndArea.xyz;
    if (dot(normal, publishedNormal) < 0.0f)
        normal = -normal;
    const float lightFacingRaw = dot(normal, -direction);
    const float lightFacing = twoSided
        ? abs(lightFacingRaw)
        : saturate(lightFacingRaw);
    if (lightFacing <= 0.0f)
        return 0.0f;
    const float solidAnglePdf = distanceSquared
        / max(area * max(lightFacing, 1.0e-8f), 1.0e-8f);
    return Upt04FinitePositive(solidAnglePdf) ? solidAnglePdf : 0.0f;
}

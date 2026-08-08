import upt04_light_contract;

// Fixed initial-RIS budget for the first reference-shaped D0 slice. Both
// populated light domains receive proposals; visibility remains one ray for
// the locally selected result, not one ray per trial.
public static const uint kUpt04DirectRisBaselineCandidateCount = 8u;
public static const uint kUpt04DirectRisParityAnalyticCount = 32u;
public static const uint kUpt04DirectRisMaximumEmissiveCount = 16u;
public static const uint kUpt04DirectRisMaximumCandidateCount = 48u;

public uint Upt04DirectRisDomainTrialCount(
    Upt04LocalLightRanges ranges,
    uint lightType,
    uint proposalParity,
    uint configuredEmissiveTrials)
{
    const bool hasEmissive = ranges.emissiveCount != 0u;
    const bool hasAnalytic = ranges.analyticCount != 0u;
    if (!hasEmissive && !hasAnalytic)
        return 0u;
    if (proposalParity != 0u)
    {
        if (lightType == kUpt04LightEmissiveTriangle)
            return hasEmissive
                ? min(configuredEmissiveTrials,
                    kUpt04DirectRisMaximumEmissiveCount)
                : 0u;
        return hasAnalytic
            ? min(ranges.analyticCount, kUpt04DirectRisParityAnalyticCount)
            : 0u;
    }
    if (hasEmissive && hasAnalytic)
        return lightType == kUpt04LightEmissiveTriangle
            ? kUpt04DirectRisBaselineCandidateCount / 2u
            : kUpt04DirectRisBaselineCandidateCount
                - kUpt04DirectRisBaselineCandidateCount / 2u;
    if (lightType == kUpt04LightEmissiveTriangle)
        return hasEmissive ? kUpt04DirectRisBaselineCandidateCount : 0u;
    return hasAnalytic ? kUpt04DirectRisBaselineCandidateCount : 0u;
}

public uint Upt04DirectRisCandidateCount(
    Upt04LocalLightRanges ranges,
    uint proposalParity,
    uint configuredEmissiveTrials)
{
    return Upt04DirectRisDomainTrialCount(
            ranges, kUpt04LightEmissiveTriangle, proposalParity,
            configuredEmissiveTrials)
        + Upt04DirectRisDomainTrialCount(
            ranges, kUpt04LightDoomAnalytic, proposalParity,
            configuredEmissiveTrials);
}

public float Upt04DirectRisIdentityPdf(
    Upt04LocalLightRanges ranges,
    uint lightType,
    uint proposalParity,
    uint configuredEmissiveTrials)
{
    const uint domainCount = lightType == kUpt04LightEmissiveTriangle
        ? ranges.emissiveCount : ranges.analyticCount;
    const uint domainTrials = Upt04DirectRisDomainTrialCount(
        ranges, lightType, proposalParity, configuredEmissiveTrials);
    const uint totalTrials = Upt04DirectRisCandidateCount(
        ranges, proposalParity, configuredEmissiveTrials);
    return domainCount != 0u && domainTrials != 0u && totalTrials != 0u
        ? float(domainTrials)
            / float(totalTrials * domainCount)
        : 0.0f;
}

public bool Upt04LocalLightRangesStructurallyValid(
    Upt04LocalLightRanges ranges,
    uint currentLightCount)
{
    const uint totalCount = ranges.emissiveCount + ranges.analyticCount;
    return totalCount >= ranges.emissiveCount
        && ranges.emissiveStart <= currentLightCount
        && ranges.emissiveCount <= currentLightCount - ranges.emissiveStart
        && ranges.analyticStart <= currentLightCount
        && ranges.analyticCount <= currentLightCount - ranges.analyticStart;
}

public uint Upt04LocalLightRangeCount(Upt04LocalLightRanges ranges)
{
    const uint totalCount = ranges.emissiveCount + ranges.analyticCount;
    return totalCount >= ranges.emissiveCount ? totalCount : 0u;
}

public Upt04LocalLightSelection Upt04SelectUniformLocalLight(
    Upt04LocalLightRanges ranges,
    float identityRandom,
    float2 surfaceRandom)
{
    Upt04LocalLightSelection result = (Upt04LocalLightSelection)0;
    const uint totalCount = ranges.emissiveCount + ranges.analyticCount;
    if (totalCount == 0u || totalCount < ranges.emissiveCount
        || !isfinite(identityRandom)
        || identityRandom < 0.0f || identityRandom >= 1.0f
        || !all(isfinite(surfaceRandom))
        || any(surfaceRandom < 0.0f) || any(surfaceRandom >= 1.0f))
        return result;

    const uint localIndex = min(uint(identityRandom * float(totalCount)), totalCount - 1u);
    const bool emissive = localIndex < ranges.emissiveCount;
    result.valid = 1u;
    result.denseIndex = emissive
        ? ranges.emissiveStart + localIndex
        : ranges.analyticStart + (localIndex - ranges.emissiveCount);
    result.type = emissive ? kUpt04LightEmissiveTriangle : kUpt04LightDoomAnalytic;
    // Emissive selection is not a center-point light.  A geometry adapter must
    // replay its triangle before transport may build a visibility segment.
    result.requiresTriangleReplay = emissive ? 1u : 0u;
    result.selectionPdf = 1.0f / float(totalCount);
    result.sampleCoordinates = surfaceRandom;
    return result;
}

public bool Upt04SelectionMatchesRecord(
    Upt04LocalLightSelection selection,
    Upt04UnifiedLightRecord record)
{
    return selection.valid != 0u
        && selection.type == record.type
        && Upt04UnifiedLightRecordStructurallyValid(record);
}

public float3 Upt04BuildPerpendicular(float3 axis)
{
    const float3 candidate = abs(axis.z) < 0.999f
        ? cross(float3(0.0f, 0.0f, 1.0f), axis)
        : cross(float3(0.0f, 1.0f, 0.0f), axis);
    return normalize(candidate);
}

public float Upt04RaySphereNearT(
    float3 origin,
    float3 direction,
    float3 center,
    float radius)
{
    const float3 originToCenter = origin - center;
    const float b = dot(originToCenter, direction);
    const float c = dot(originToCenter, originToCenter) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant <= 0.0f)
        return 0.0f;
    return -b - sqrt(discriminant);
}

// Samples the visible solid angle of the manager's analytic sphere.  This is
// not a center-point proxy: the sample position and directional PDF replay from
// the two stored coordinates.
public Upt04LocalLightSample Upt04SampleAnalyticSphere(
    Upt04UnifiedLightRecord light,
    float3 receiverPosition,
    float2 random)
{
    Upt04LocalLightSample result = (Upt04LocalLightSample)0;
    if (light.type != kUpt04LightDoomAnalytic
        || !Upt04UnifiedLightRecordStructurallyValid(light)
        || !all(isfinite(receiverPosition))
        || !all(isfinite(random)) || any(random < 0.0f) || any(random >= 1.0f))
        return result;

    const float radius = light.positionAndRadius.w;
    const float3 toCenter = light.positionAndRadius.xyz - receiverPosition;
    const float centerDistanceSquared = dot(toCenter, toCenter);
    if (!isfinite(centerDistanceSquared) || centerDistanceSquared <= radius * radius + 1.0e-6f)
        return result;
    const float centerDistance = sqrt(centerDistanceSquared);
    const float3 centerDirection = toCenter / centerDistance;
    const float sinThetaMax = saturate(radius / centerDistance);
    const float cosThetaMax = sqrt(max(0.0f, 1.0f - sinThetaMax * sinThetaMax));
    const float cosTheta = 1.0f - random.x * (1.0f - cosThetaMax);
    const float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = 6.283185307179586f * random.y;
    const float3 tangent = Upt04BuildPerpendicular(centerDirection);
    const float3 bitangent = cross(centerDirection, tangent);
    const float3 direction = normalize(
        tangent * (cos(phi) * sinTheta)
        + bitangent * (sin(phi) * sinTheta)
        + centerDirection * cosTheta);
    const float hitT = Upt04RaySphereNearT(
        receiverPosition,
        direction,
        light.positionAndRadius.xyz,
        radius);
    const float solidAngle = 6.283185307179586f * (1.0f - cosThetaMax);
    if (!isfinite(hitT) || hitT <= 0.01f || !Upt04FinitePositive(solidAngle))
        return result;

    result.position = receiverPosition + direction * hitT;
    result.normal = normalize(result.position - light.positionAndRadius.xyz);
    result.distance = hitT;
    result.direction = direction;
    result.solidAnglePdf = 1.0f / solidAngle;
    // Match the accepted legacy/RRX default: the authored Doom radius remains
    // proposal metadata, not a hard shading boundary.  Applying its quadratic
    // mask here produced rigid visible light volumes in the UPT output.
    result.radiance = max(light.radianceAndLuminance.xyz, float3(0.0f));
    result.status = any(result.radiance > 0.0f)
        ? kUpt04SampleValidPositive : kUpt04SampleValidZero;
    return result;
}

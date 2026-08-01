import upt04_light_contract;

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

    const float influenceRadius = light.uvOrDoomParams.x;
    const float radiusFraction = saturate(centerDistance / influenceRadius);
    const float influence = saturate(1.0f - radiusFraction * radiusFraction);
    result.position = receiverPosition + direction * hitT;
    result.normal = normalize(result.position - light.positionAndRadius.xyz);
    result.distance = hitT;
    result.direction = direction;
    result.solidAnglePdf = 1.0f / solidAngle;
    result.radiance = max(light.radianceAndLuminance.xyz, float3(0.0f)) * influence;
    result.status = influence > 0.0f
        ? kUpt04SampleValidPositive
        : kUpt04SampleValidZero;
    return result;
}

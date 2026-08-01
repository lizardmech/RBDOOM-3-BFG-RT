import upt04_initial_bindings;
import upt04_geometry_bindings;
import upt04_light_contract;
import upt04_local_light_proposal;

public uint Upt04EmissiveLookupHash(uint instanceId, uint primitiveIndex)
{
    uint value = instanceId ^ (primitiveIndex + 0x9e3779b9u
        + (instanceId << 6u) + (instanceId >> 2u));
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

public bool Upt04FindEmissiveDenseLight(
    uint instanceId,
    uint primitiveIndex,
    out uint denseLightIndex)
{
    denseLightIndex = kUpt04InvalidLightIndex;
    uint entryCount = 0u;
    uint entryStride = 0u;
    gUpt04EmissiveLookup.GetDimensions(entryCount, entryStride);
    if (entryCount < 2u || (entryCount & (entryCount - 1u)) != 0u
        || entryStride != 16u)
        return false;

    const uint mask = entryCount - 1u;
    const uint firstSlot = Upt04EmissiveLookupHash(instanceId, primitiveIndex) & mask;
    [unroll]
    for (uint probe = 0u; probe < kUpt04EmissiveLookupMaxProbes; ++probe)
    {
        const Upt04EmissiveLookupEntry entry =
            gUpt04EmissiveLookup[(firstSlot + probe) & mask];
        if (entry.occupied == 0u)
            return false;
        if (entry.instanceId == instanceId
            && entry.primitiveIndex == primitiveIndex)
        {
            if (entry.denseLightIndex >= gUpt04Control.currentLightCount)
                return false;
            const Upt04UnifiedLightRecord light =
                gUpt04CurrentLights[entry.denseLightIndex];
            if (light.type != kUpt04LightEmissiveTriangle
                || light.instanceId != instanceId
                || light.primitiveIndex != primitiveIndex)
                return false;
            denseLightIndex = entry.denseLightIndex;
            return true;
        }
    }
    return false;
}

public float Upt04ExactNeePdfForEmissiveHit(
    uint instanceId,
    uint primitiveIndex,
    float3 receiverPosition,
    float3 hitPosition)
{
    uint denseLightIndex = kUpt04InvalidLightIndex;
    if (!Upt04FindEmissiveDenseLight(
            instanceId, primitiveIndex, denseLightIndex))
        return 0.0f;

    Upt04LocalLightRanges ranges;
    ranges.emissiveStart = gUpt04Control.emissiveRangeStart;
    ranges.emissiveCount = gUpt04Control.emissiveRangeCount;
    ranges.analyticStart = gUpt04Control.analyticRangeStart;
    ranges.analyticCount = gUpt04Control.analyticRangeCount;
    const uint lightCount = Upt04LocalLightRangeCount(ranges);
    if (lightCount == 0u)
        return 0.0f;

    const Upt04UnifiedLightRecord light = gUpt04CurrentLights[denseLightIndex];
    const float area = light.normalAndArea.w;
    const float3 toLight = hitPosition - receiverPosition;
    const float distanceSquared = dot(toLight, toLight);
    if (!Upt04FinitePositive(area) || !Upt04FinitePositive(distanceSquared))
        return 0.0f;
    const float3 direction = toLight * rsqrt(distanceSquared);
    const float3 normal = normalize(light.normalAndArea.xyz);
    const float lightFacing = dot(normal, -direction);
    if (!Upt04FinitePositive(lightFacing))
        return 0.0f;
    const float solidAnglePdf = distanceSquared / (area * lightFacing);
    const float selectionPdf = 1.0f / float(lightCount);
    const float neePdf = selectionPdf * solidAnglePdf;
    return Upt04FinitePositive(neePdf) ? neePdf : 0.0f;
}

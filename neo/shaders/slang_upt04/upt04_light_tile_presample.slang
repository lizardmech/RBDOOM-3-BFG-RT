import upt04_light_contract;
import upt04_light_tile_contract;
import upt04_random_white_hash;

public struct Upt04LightTilePresampleControl
{
    public uint tileCount;
    public uint tileDomainSize;
    public uint frameSampleIndex;
    public uint emissiveDistributionCount;
    public uint emissiveRangeStart;
    public uint emissiveRangeCount;
    public uint analyticRangeStart;
    public uint analyticRangeCount;
    public uint emissiveTrials;
    public uint analyticTrials;
    public uint candidateCount;
    public uint reserved0;
};

[[vk::binding(0, 0)]] StructuredBuffer<Upt04EmissiveDistributionEntry>
    gUpt04TileEmissiveDistribution;
[[vk::binding(1, 0)]] RWStructuredBuffer<Upt04LightTileEntry>
    gUpt04LightTilesOut;
[[vk::push_constant]] ConstantBuffer<Upt04LightTilePresampleControl>
    gUpt04TileControl;

public float Upt04LightTileHash01(uint tileIndex, uint entry, uint domain)
{
    uint key = gUpt04TileControl.frameSampleIndex ^ 0x51ed270bu;
    key = Upt04HashWord(key ^ (tileIndex * 0x9e3779b9u));
    key = Upt04HashWord(key ^ (entry * 0x85ebca6bu));
    key = Upt04HashWord(key ^ (domain * 0xc2b2ae35u));
    return float(key >> 8u) * (1.0f / 16777216.0f);
}
public Upt04LightTileEntry Upt04PresampleEmissiveTileEntry(
    uint tileIndex,
    uint entryInDomain)
{
    Upt04LightTileEntry result = (Upt04LightTileEntry)0;
    result.denseLightIndex = kUpt04InvalidLightIndex;
    if (gUpt04TileControl.emissiveRangeCount == 0u
        || gUpt04TileControl.emissiveTrials == 0u
        || gUpt04TileControl.candidateCount == 0u
        || gUpt04TileControl.emissiveDistributionCount == 0u)
        return result;

    uint distributionCapacity = 0u;
    uint distributionStride = 0u;
    gUpt04TileEmissiveDistribution.GetDimensions(
        distributionCapacity, distributionStride);
    const uint distributionCount =
        gUpt04TileControl.emissiveDistributionCount;
    if (distributionCount > distributionCapacity)
        return result;

    const float identityRandom = Upt04LightTileHash01(
        tileIndex, entryInDomain, kUpt04LightTileDomainEmissive);
    uint low = 0u;
    uint high = distributionCount;
    [loop]
    while (low < high)
    {
        const uint mid = low + ((high - low) >> 1u);
        const Upt04EmissiveDistributionEntry entry =
            gUpt04TileEmissiveDistribution[mid];
        if (identityRandom <= saturate(entry.cumulativePdf))
            high = mid;
        else
            low = mid + 1u;
    }
    if (low >= distributionCount)
        return result;

    const Upt04EmissiveDistributionEntry entry =
        gUpt04TileEmissiveDistribution[low];
    const float previousCdf = low != 0u
        ? saturate(gUpt04TileEmissiveDistribution[low - 1u].cumulativePdf)
        : 0.0f;
    const float conditionalPdf = saturate(entry.cumulativePdf) - previousCdf;
    const float domainMixture = float(gUpt04TileControl.emissiveTrials)
        / float(gUpt04TileControl.candidateCount);
    if (entry.emissiveTriangleIndex == kUpt04InvalidLightIndex
        || entry.denseLightIndex == kUpt04InvalidLightIndex
        || entry.denseLightIndex < gUpt04TileControl.emissiveRangeStart
        || entry.denseLightIndex - gUpt04TileControl.emissiveRangeStart
            >= gUpt04TileControl.emissiveRangeCount
        || !Upt04FinitePositive(conditionalPdf)
        || !Upt04FinitePositive(domainMixture))
        return result;

    result.denseLightIndex = entry.denseLightIndex;
    result.replayGuard = entry.emissiveTriangleIndex + 1u;
    result.selectionPdf = conditionalPdf * domainMixture;
    result.lightType = kUpt04LightEmissiveTriangle;
    return result;
}

public Upt04LightTileEntry Upt04PresampleAnalyticTileEntry(
    uint tileIndex,
    uint entryInDomain)
{
    Upt04LightTileEntry result = (Upt04LightTileEntry)0;
    result.denseLightIndex = kUpt04InvalidLightIndex;
    const uint analyticOrdinal = entryInDomain
        % kUpt04LightTileAnalyticOrdinalCount;
    if (gUpt04TileControl.analyticRangeCount == 0u
        || gUpt04TileControl.analyticTrials == 0u
        || analyticOrdinal >= gUpt04TileControl.analyticTrials
        || gUpt04TileControl.candidateCount == 0u)
        return result;

    const float identityRandom = Upt04LightTileHash01(
        tileIndex, entryInDomain, kUpt04LightTileDomainAnalytic);
    const float stride = max(
        1.0f,
        float(gUpt04TileControl.analyticRangeCount)
            / float(gUpt04TileControl.analyticTrials));
    const float lightIndexInRange = min(
        (float(analyticOrdinal) + identityRandom) * stride,
        float(gUpt04TileControl.analyticRangeCount - 1u));
    const uint localIndex = min(
        uint(lightIndexInRange + 0.5f),
        gUpt04TileControl.analyticRangeCount - 1u);
    const float selectionPdf = float(gUpt04TileControl.analyticTrials)
        / float(gUpt04TileControl.candidateCount
            * gUpt04TileControl.analyticRangeCount);
    if (!Upt04FinitePositive(selectionPdf))
        return result;

    result.denseLightIndex =
        gUpt04TileControl.analyticRangeStart + localIndex;
    result.replayGuard = 0u;
    result.selectionPdf = selectionPdf;
    result.lightType = kUpt04LightDoomAnalytic;
    return result;
}

[numthreads(128, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint entryInTile = dispatchThreadId.x;
    const uint tileIndex = dispatchThreadId.y;
    if (tileIndex >= gUpt04TileControl.tileCount
        || entryInTile >= gUpt04TileControl.tileDomainSize * 2u)
        return;

    const bool emissive = entryInTile < gUpt04TileControl.tileDomainSize;
    const uint entryInDomain = emissive
        ? entryInTile : entryInTile - gUpt04TileControl.tileDomainSize;
    const uint domain = emissive
        ? kUpt04LightTileDomainEmissive : kUpt04LightTileDomainAnalytic;
    const uint outputIndex = Upt04LightTileLinearIndex(
        domain, tileIndex, entryInDomain);
    gUpt04LightTilesOut[outputIndex] = emissive
        ? Upt04PresampleEmissiveTileEntry(tileIndex, entryInDomain)
        : Upt04PresampleAnalyticTileEntry(tileIndex, entryInDomain);
}

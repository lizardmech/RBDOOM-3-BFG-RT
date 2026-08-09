public static const uint kUpt04LightTileCount = 128u;
public static const uint kUpt04LightTileDomainSize = 1024u;
public static const uint kUpt04LightTileAnalyticOrdinalCount = 32u;
public static const uint kUpt04LightTileAnalyticVariantsPerOrdinal = 32u;
public static const uint kUpt04LightTileDomainEmissive = 0u;
public static const uint kUpt04LightTileDomainAnalytic = 1u;
public static const uint kUpt04LightTileDomainCount = 2u;
public static const uint kUpt04LightTileEntryCount =
    kUpt04LightTileCount * kUpt04LightTileDomainSize
        * kUpt04LightTileDomainCount;

public struct Upt04LightTileEntry
{
    public uint denseLightIndex;
    public uint replayGuard;
    public float selectionPdf;
    public uint lightType;
};

public uint Upt04LightTileLinearIndex(
    uint domain,
    uint tileIndex,
    uint entryInDomain)
{
    return (domain * kUpt04LightTileCount + tileIndex)
        * kUpt04LightTileDomainSize + entryInDomain;
}

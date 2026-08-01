public struct Upt04RandomKey
{
    public uint pixelX;
    public uint pixelY;
    public uint frameSampleIndex;
    public uint passNamespace;
    public uint pathVertex;
    public uint streamNamespace;
    public uint dimension;
    public uint sampleOrdinal;
    public uint replayEpoch;
};

public static const uint kUpt04PassInitial = 0x0001u;
public static const uint kUpt04StreamReservoir = 0x0002u;
public static const uint kUpt04DimensionReservoirSelection = 0u;

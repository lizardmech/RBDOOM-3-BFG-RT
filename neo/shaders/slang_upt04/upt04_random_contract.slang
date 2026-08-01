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

// Fixed cross-language slot map.  Providers address (stream, dimension)
// directly; disabling a family never advances a shared cursor.
#define UPT_RANDOM_SLOT(name, streamValue, dimensionValue) \
    public static const uint kUpt04Stream##name = streamValue; \
    public static const uint kUpt04Dimension##name = dimensionValue;
#include "PathTraceUnifiedPtRandomDimensions.inc"
#undef UPT_RANDOM_SLOT
